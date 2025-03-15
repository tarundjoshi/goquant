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

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace simdjson;

class WebSocketClient {
public:
    asio::io_context ioc;
    asio::thread_pool pool;
    ssl::context ssl_ctx;
    std::unique_ptr<websocket::stream<ssl::stream<tcp::socket>>> ws;
    std::mutex mtx;
    EVP_PKEY* private_key = nullptr;

    WebSocketClient(const std::string& key_path)
        : ssl_ctx(ssl::context::tlsv12_client), pool(std::thread::hardware_concurrency()) {
        ssl_ctx.set_default_verify_paths();
        load_private_key(key_path);
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

    void on_read(beast::flat_buffer& buffer, boost::system::error_code ec) {
        if (ec) {
            std::cerr << "Read error: " << ec.message() << "\n";
            return;
        }

        std::cout << "Response: " << beast::make_printable(buffer.data()) << "\n";
    }

    void connect(const std::string& host, const std::string& port, const std::string& auth_request) {
        post(pool, [this, host, port, auth_request]() {
            try {
                tcp::resolver resolver(ioc);
                auto results = resolver.resolve(host, port);

                ws = std::make_unique<websocket::stream<ssl::stream<tcp::socket>>>(ioc, ssl_ctx);
                asio::connect(ws->next_layer().next_layer(), results.begin(), results.end());

                enable_tcp_nodelay(ws->next_layer().next_layer());
                ws->next_layer().handshake(ssl::stream_base::client);
                ws->handshake(host, "/ws/api/v2");

                beast::flat_buffer buffer;
                ws->async_write(asio::buffer(auth_request),
                                [this, &buffer](boost::system::error_code ec, std::size_t) {
                                    if (!ec) {
                                        ws->async_read(buffer, 
                                                       [this, &buffer](boost::system::error_code ec, std::size_t) {
                                                           on_read(buffer, ec);
                                                       });
                                    }
                                });
                ioc.run();

            } catch (const std::exception& e) {
                std::cerr << "Connection error: " << e.what() << "\n";
            }
        });
    }

    void run() {
        pool.join();
    }
};

// Main function
int main() {
    try {
        const std::string client_id = "zavRq-oY";
        const std::string private_key_path = "private.pem";

        WebSocketClient client(private_key_path);

        long long timestamp = client.get_current_timestamp();
        std::string nonce = client.generate_nonce();


        // Construct JSON request using simdjson
        std::string auth_request = R"({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "public/auth",
            "params": {
                "grant_type": "client_signature",
                "client_id": ")" + client_id + R"(",
                "timestamp": )" + std::to_string(timestamp) + R"(,
                "signature": ")" + client.sign_data(std::to_string(timestamp) + "\n" + nonce + "\n") + R"(",
                "nonce": ")" + nonce + R"(",
                "data": ""
            }
        })";

        std::cout << "Auth request: " << auth_request << std::endl;

        // client.connect("test.deribit.com", "443", auth_request);
        // client.run();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    return 0;
}