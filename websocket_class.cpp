#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <simdjson.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <mutex>
#include <thread>
#include <queue>
#include <unordered_map>
#include <set>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace simdjson;

#define CLIENT_ID "zavRq-oY"


class WebSocketClient {
public:
    asio::io_context ioc;
    ssl::context ssl_ctx;
    std::unique_ptr<websocket::stream<ssl::stream<tcp::socket>>> ws;
    std::unordered_map<int, std::function<void(const std::string&)>> response_handlers;
    std::queue<std::string> write_queue;
    int request_id;
    std::atomic<bool> is_writing{false}; // Make atomic
    const std::string client_id;
    EVP_PKEY* private_key = nullptr;

    WebSocketClient(const std::string& key_path, std::string client_id)
        :   ssl_ctx(ssl::context::tlsv12_client), 
            client_id(client_id), 
            request_id(0) {
        ssl_ctx.set_default_verify_paths();
        load_private_key(key_path);
        // asio::executor_work_guard<asio::io_context::executor_type> work_guard = asio::make_work_guard(ioc);

    }

    ~WebSocketClient() {
        if (private_key) {
            EVP_PKEY_free(private_key);
        }
    }

    long long get_current_timestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string generate_nonce() {
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        constexpr size_t length = 16;
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

        std::string nonce;
        nonce.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            nonce += charset[dist(generator)];
        }
        return nonce;
    }

    void load_private_key(const std::string& private_key_path) {
        FILE* private_key_file = fopen(private_key_path.c_str(), "r");
        if (!private_key_file) {
            throw std::runtime_error("Failed to open private key file");
        }

        private_key = PEM_read_PrivateKey(private_key_file, nullptr, nullptr, nullptr);
        fclose(private_key_file);

        if (!private_key || EVP_PKEY_id(private_key) != EVP_PKEY_ED25519) {
            throw std::runtime_error("Invalid or unsupported private key type. Expected Ed25519.");
        }

        std::cout << "Private key loaded successfully.\n";
    }

    std::string sign_data(const std::string& data) {
        if (!private_key) {
            throw std::runtime_error("Private key not loaded");
        }

        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        if (!md_ctx) {
            throw std::runtime_error("Failed to create EVP_MD_CTX");
        }

        if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, private_key) != 1) {
            EVP_MD_CTX_free(md_ctx);
            throw std::runtime_error("Failed to initialize signing");
        }

        size_t signature_len = 0;
        if (EVP_DigestSign(md_ctx, nullptr, &signature_len,
                           reinterpret_cast<const unsigned char*>(data.c_str()), data.size()) != 1) {
            EVP_MD_CTX_free(md_ctx);
            throw std::runtime_error("Failed to calculate signature length");
        }

        std::vector<unsigned char> signature(signature_len);
        if (EVP_DigestSign(md_ctx, signature.data(), &signature_len,
                           reinterpret_cast<const unsigned char*>(data.c_str()), data.size()) != 1) {
            EVP_MD_CTX_free(md_ctx);
            throw std::runtime_error("Failed to generate signature");
        }

        EVP_MD_CTX_free(md_ctx);

        // Base64 encode the signature
        BIO* bio = BIO_new(BIO_s_mem());
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);
        BIO_write(bio, signature.data(), static_cast<int>(signature.size()));
        BIO_flush(bio);

        BUF_MEM* buffer_ptr;
        BIO_get_mem_ptr(bio, &buffer_ptr);
        std::string base64_string(buffer_ptr->data, buffer_ptr->length);
        BIO_free_all(bio);

        // Make it URL-safe
        std::replace(base64_string.begin(), base64_string.end(), '+', '-');
        std::replace(base64_string.begin(), base64_string.end(), '/', '_');
        base64_string.erase(std::remove(base64_string.begin(), base64_string.end(), '='), base64_string.end());

        return base64_string;

    }

    void enable_tcp_nodelay(tcp::socket& socket) {
        boost::asio::ip::tcp::no_delay option(true);
        socket.set_option(option);
    }

    void write_next() {
        if (is_writing.load() || write_queue.empty()) return;

        // Try to set is_writing to true (if already true, don't proceed)
        if (!is_writing.exchange(true)) {
            auto msg = write_queue.front();

            ws->async_write(
                asio::buffer(msg),
                [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                    is_writing.store(false);

                    if (ec) {
                        std::cerr << "Write error: " << ec.message() << "\n";
                        return;
                    }

                    std::cout << "Successfully wrote (" << bytes_transferred << "bytes)\n";

                    // Remove the completed request from the queue
                    write_queue.pop();

                    // Try writing the next message
                    // async_read_response();
                    write_next();
                });
        }
    }

    void connect(const std::string& host, const std::string& port) {
        try {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(host, port);

            ws = std::make_unique<websocket::stream<ssl::stream<tcp::socket>>>(ioc, ssl_ctx);
            asio::connect(ws->next_layer().next_layer(), results.begin(), results.end());

            enable_tcp_nodelay(ws->next_layer().next_layer());
            ws->next_layer().handshake(ssl::stream_base::client);
            ws->handshake(host, "/ws/api/v2");


            long long timestamp = get_current_timestamp();
            std::string nonce = generate_nonce();


            // Construct JSON request using simdjson
            std::string auth_request = R"({
                "jsonrpc": "2.0",
                "id": 0,
                "method": "public/auth",
                "params": {
                    "grant_type": "client_signature",
                    "client_id": ")" + client_id + R"(",
                    "timestamp": )" + std::to_string(timestamp) + R"(,
                    "signature": ")" + sign_data(std::to_string(timestamp) + "\n" + nonce + "\n") + R"(",
                    "nonce": ")" + nonce + R"(",
                    "data": ""
                }
            })";


            write_queue.push(auth_request);
            write_next();

            async_read_response();

        } catch (const std::exception& e) {
            std::cerr << "Connection error: " << e.what() << "\n";
        }
    }

    

    void async_read_response() {
        auto buffer = std::make_shared<beast::flat_buffer>();
        ws->async_read(
            *buffer,
            [this, buffer](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (ec) {
                    std::cerr << "Read error: " << ec.message() << "\n";
                    return;
                }
                
                std::cout << "Response (" << bytes_transferred << " bytes): "
                          << beast::make_printable(buffer->data()) << "\n";
    
                simdjson::dom::parser parser;
                try {
                    auto json = parser.parse(beast::buffers_to_string(buffer->data()));
                    std::string raw_json = simdjson::minify(json);

                    int id = json["id"].get_int64();

                    if (response_handlers.find(id) != response_handlers.end()) {
                        response_handlers[id](raw_json);  // Fulfill the promise
                        response_handlers.erase(id);      // Clean up
                    }   
                
                    int indent = 0;
                    for (char c : raw_json) {
                        if (c == '{' || c == '[') {
                            std::cout << c << "\n" << std::string(++indent * 4, ' ');
                        } else if (c == '}' || c == ']') {
                            std::cout << "\n" << std::string(--indent * 4, ' ') << c;
                        } else if (c == ',') {
                            std::cout << c << "\n" << std::string(indent * 4, ' ');
                        } else if (c == ':') {
                            std::cout << c << ' ';
                        } else {
                            std::cout << c;
                        }
                    }
                    std::cout << "\n"; 

                } catch (const simdjson::simdjson_error &e) {
                    std::cerr << "JSON parse error: " << e.what() << "\n";
                }
                          
    
                // Continue reading the next response
                async_read_response();
            });
    }
    

    // Place an order
    std::future<std::string> place_order(const std::string& instrument, const std::string& direction, 
                    double price, double amount, const std::string& order_type = "limit") {
        long long timestamp = get_current_timestamp();
        std::string nonce = generate_nonce();
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();
        
        std::string request = R"({
            "jsonrpc": "2.0",
            "id": )" + std::to_string(++request_id) + R"(,
            "method": ")" + std::string(direction == "buy" ? "private/buy" : "private/sell") + R"(",
            "params": {
                "instrument_name": ")" + instrument + R"(",
                "amount": )" + std::to_string(amount) + R"(,
                "price": )" + std::to_string(price) + R"(,
                "type": ")" + order_type + R"(",
                "label": "test",
                "timestamp": )" + std::to_string(timestamp) + R"(,
                "signature": ")" + sign_data(std::to_string(timestamp) + "\n" + nonce + "\n") + R"("
            }
        })";

        write_queue.push(request);
        write_next();

        response_handlers[request_id] = [promise](const std::string& response) {
            promise->set_value(response);
        };
    
        return future;
    }

    // Cancel an order
    std::future<std::string> cancel_order(const std::string& order_id) {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();

        std::string request = R"({
            "jsonrpc": "2.0",
            "id": )" + std::to_string(++request_id) + R"(,
            "method": "private/cancel",
            "params": {
                "order_id": ")" + order_id + R"("
            }
        })";

        write_queue.push(request);
        write_next();

        response_handlers[request_id] = [promise](const std::string& response) {
            promise->set_value(response);
        };
    
        return future;
    }

    // Modify an order
    std::future<std::string> modify_order(const std::string& order_id, double new_price, double new_amount) {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();

        std::string request = R"({
            "jsonrpc": "2.0",
            "id": )" + std::to_string(++request_id) + R"(,
            "method": "private/edit",
            "params": {
                "order_id": ")" + order_id + R"(",
                "price": )" + std::to_string(new_price) + R"(,
                "amount": )" + std::to_string(new_amount) + R"("
            }
        })";

        write_queue.push(request);
        write_next();

        response_handlers[request_id] = [promise](const std::string& response) {
            promise->set_value(response);
        };
    
        return future;
    }

    std::future<std::string> get_order_book(const std::string& instrument, int depth) {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();

        std::string request = R"({
            "jsonrpc": "2.0",
            "id": )" + std::to_string(++request_id) + R"(,
            "method": "public/get_order_book",
            "params": {
                "instrument_name": ")" + instrument + R"(",
                "depth": )" + std::to_string(depth) + R"(
            }
        })";
    
        write_queue.push(request);
        write_next();

        response_handlers[request_id] = [promise](const std::string& response) {
            promise->set_value(response);
        };
    
        return future;
    }

    std::future<std::string> get_positions() {
    
        std::string request = R"({
            "jsonrpc": "2.0",
            "id": )" + std::to_string(++request_id) + R"(,
            "method": "private/get_positions",
            "params": {
            }
        })";
    
        // Create promise and store future
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();
    
        // Store the promise in the map
        response_handlers[request_id] = [promise](const std::string& response) {
            promise->set_value(response);
        };
    
        // Send the request
        write_queue.push(request);
        write_next();
    
        return future;
    }
    
    

};


// Main function
int main() {
    try {
        const std::string private_key_path = "private.pem";

        WebSocketClient client(private_key_path, CLIENT_ID);

        client.connect("test.deribit.com", "443");
        std::thread t([&]() { client.ioc.run(); });


        // client.place_order("ADA_USDC-PERPETUAL", "buy", 0.77, 996, "limit");
        // client.place_order("ADA_USDC-PERPETUAL", "buy", 0.77, 996, "limit");

        // client.get_order_book("ADA_USDC-PERPETUAL", 10);
        client.get_positions();

        t.join();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    return 0;
}
