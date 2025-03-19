#include "dericlient.h"
#include <iostream>
#include <simdjson.h>

// DeriClient implementation

DeriClient::DeriClient(const std::string& key_path, std::string client_id, quill::Logger* logger)
    : ssl_ctx(ssl::context::tlsv12_client), 
      client_id(client_id), 
      request_id(0),
      logger(logger) {
    ssl_ctx.set_default_verify_paths();
    load_private_key(key_path);
}

DeriClient::~DeriClient() {
    if (private_key) {
        EVP_PKEY_free(private_key);
    }
}

long long DeriClient::get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string DeriClient::generate_nonce() {
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

void DeriClient::load_private_key(const std::string& private_key_path) {
    FILE* private_key_file = fopen(private_key_path.c_str(), "r");
    if (!private_key_file) {
        throw std::runtime_error("Failed to open private key file");
    }

    private_key = PEM_read_PrivateKey(private_key_file, nullptr, nullptr, nullptr);
    fclose(private_key_file);

    if (!private_key || EVP_PKEY_id(private_key) != EVP_PKEY_ED25519) {
        throw std::runtime_error("Invalid or unsupported private key type. Expected Ed25519.");
    }

    // std::cout << "Private key loaded successfully.\n";
    LOG_INFO(logger, "Private key loaded successfully.");
}

std::string DeriClient::sign_data(const std::string& data) {
    if (!private_key) {
        LOG_ERROR(logger, "Private key not loaded");
        throw std::runtime_error("Private key not loaded");

    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        LOG_ERROR(logger, "Failed to create EVP_MD_CTX");
        throw std::runtime_error("Failed to create EVP_MD_CTX");

    }

    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, private_key) != 1) {
        EVP_MD_CTX_free(md_ctx);
        LOG_ERROR(logger, "Failed to initialize signing");
        throw std::runtime_error("Failed to initialize signing");

    }

    size_t signature_len = 0;
    if (EVP_DigestSign(md_ctx, nullptr, &signature_len,
                       reinterpret_cast<const unsigned char*>(data.c_str()), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        LOG_ERROR(logger, "Failed to calculate signature length");
        throw std::runtime_error("Failed to calculate signature length");
    }

    std::vector<unsigned char> signature(signature_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &signature_len,
                       reinterpret_cast<const unsigned char*>(data.c_str()), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        LOG_ERROR(logger, "Failed to generate signature");
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

void DeriClient::enable_tcp_nodelay(tcp::socket& socket) {
    boost::asio::ip::tcp::no_delay option(true);
    socket.set_option(option);
}

void DeriClient::write_next() {
    if (is_writing.load()) return;

    std::string msg;
    if (!write_queue.try_dequeue(msg)) return; // Empty queue check

    // Try to set is_writing to true (if already true, don't proceed)
    if (!is_writing.exchange(true)) {
        ws->async_write(
            asio::buffer(msg),
            [this, msg](boost::system::error_code ec, std::size_t bytes_transferred) {
                is_writing.store(false);

                if (ec) {
                    // std::cerr << "Write error: " << ec.message() << "\n";
                    LOG_ERROR(logger, "Write error: {}", ec.message());
                    return;
                }

                // std::cout << "Successfully wrote (" << bytes_transferred << " bytes)\n";
                LOG_DEBUG(logger, "Successfully Sent request : {}", msg);
                write_next();
            });
    }
}

void DeriClient::connect(const std::string& host, const std::string& port) {
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

        // Construct JSON request
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

        write_queue.enqueue(auth_request);
        write_next();

        async_read_response();

    } catch (const std::exception& e) {
        // std::cerr << "Connection error: " << e.what() << "\n";
        LOG_ERROR(logger, "Connection error: {}", e.what());
    }
}

void DeriClient::async_read_response() {
    auto buffer = std::make_shared<beast::flat_buffer>();
    ws->async_read(
        *buffer,
        [this, buffer](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                // std::cerr << "Read error: " << ec.message() << "\n";
                LOG_ERROR(logger, "Read error: {}", ec.message());
                return;
            }
            
            std::string response = beast::buffers_to_string(buffer->data());
            simdjson::dom::parser parser;
            
            try {
                auto json = parser.parse(response);

                // std::cout << "Received response: " << response << "\n\n";
                LOG_DEBUG(logger, "Received response: {}", response);
                
                // Check if this is a streaming update
                std::string_view method;
                uint64_t id = -1;
                
                if(json["id"].get(id) == simdjson::SUCCESS) {
                    LOG_INFO(logger, "Received response for request ID {}.", id);
                    auto it = order_latency_timers.find(id);
                    if (it != order_latency_timers.end()) {
                        auto end_time = std::chrono::high_resolution_clock::now();
                        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - it->second).count();
                        
                        // Check if there's an error in the response
                        simdjson::error_code error;
                        simdjson::dom::object error_obj;
                        if (json["error"].get(error_obj) == simdjson::SUCCESS) {
                            // Order failed
                            int64_t error_code;
                            std::string_view error_message;
                            error_obj["code"].get(error_code);
                            error_obj["message"].get(error_message);
                            
                            LOG_ERROR(logger, "Order failed. Error code: {}, message: {}. Latency: {} us", 
                                     error_code, error_message, latency);
                        } else {
                             // Order succeeded - Add more detailed logging here
                            std::string_view instrument;
                            std::string_view direction;
                            double price = 0.0;
                            double amount = 0.0;
                            std::string_view order_id;
                            
                            // Extract order details from the response
                            json["result"]["order"]["instrument_name"].get(instrument);
                            json["result"]["order"]["direction"].get(direction);
                            json["result"]["order"]["price"].get(price);
                            json["result"]["order"]["amount"].get(amount);
                            json["result"]["order"]["order_id"].get(order_id);
                            
                            LOG_INFO(logger, "Order placed successfully: {} {} {} @ {} USD. Order ID: {}. Latency: {} us", 
                                    direction, amount, instrument, price, order_id, latency);
                            
                        }
                        
                        // Store for later analysis
                        order_latencies.push_back(latency);
                        order_latency_timers.erase(it);
                    }
                }
                

                if (json["method"].get_string().get(method) == simdjson::SUCCESS && 
                    method == "subscription") {
                    
                    // Get the channel name
                    LOG_INFO(logger, "Received streaming update.");
                    std::string_view channel;
                    if (json["params"]["channel"].get_string().get(channel) == simdjson::SUCCESS) {
                        std::string channel_str(channel);
                        
                        // Find and call the appropriate handler
                        auto it = streaming_handlers.find(channel_str);
                        if (it != streaming_handlers.end()) {
                            it->second(response);
                        }
                    }
                }
                
            } catch (const simdjson::simdjson_error &e) {
                // std::cerr << "JSON parse error: " << e.what() << "\n";
                LOG_ERROR(logger, "JSON parse error: {}", e.what());
            }
            
            // Continue reading the next response
            async_read_response();
        });
}

void DeriClient::place_order(const std::string& instrument, const std::string& direction, 
                double price, double amount, const std::string& order_type) {
    long long timestamp = get_current_timestamp();
    std::string nonce = generate_nonce();

    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Store the request ID and start time
    int order_req_id = ++request_id;
    order_latency_timers[order_req_id] = start_time;
    
    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(order_req_id) + R"(,
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

    write_queue.enqueue(request);
    write_next();

}

void DeriClient::cancel_order(const std::string& order_id) {

    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "private/cancel",
        "params": {
            "order_id": ")" + order_id + R"("
        }
    })";

    write_queue.enqueue(request);
    write_next();

}

void DeriClient::modify_order(const std::string& order_id, double new_price, double new_amount) {

    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "private/edit",
        "params": {
            "order_id": ")" + order_id + R"(",
            "price": )" + std::to_string(new_price) + R"(,
            "amount": )" + std::to_string(new_amount) + R"(
        }
    })";

    write_queue.enqueue(request);
    write_next();

}

void DeriClient::get_order_book(const std::string& instrument, int depth) {

    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "public/get_order_book",
        "params": {
            "instrument_name": ")" + instrument + R"(",
            "depth": )" + std::to_string(depth) + R"(
        }
    })";

    write_queue.enqueue(request);
    write_next();
}

void DeriClient::get_positions() {
    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "private/get_positions",
        "params": {
        }
    })";

    // Send the request
    write_queue.enqueue(request);
    write_next();

}

void DeriClient::subscribe_to_orderbook(const std::string& instrument, 
    std::function<void(const std::string&)> callback) {
    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "public/subscribe",
        "params": {
            "channels": ["book.)" + instrument + R"(.100ms"]
        }
    })";

    streaming_handlers["book." + instrument + ".100ms"] = callback;

    write_queue.enqueue(request);
    write_next();

}

void DeriClient::unsubscribe_from_orderbook(const std::string& instrument) {
    std::string request = R"({
        "jsonrpc": "2.0",
        "id": )" + std::to_string(++request_id) + R"(,
        "method": "public/unsubscribe",
        "params": {
            "channels": ["book.)" + instrument + R"(.100ms"]
        }
    })";

    // Remove the handler from the map
    streaming_handlers.erase("book." + instrument + ".100ms");

    // Send the unsubscribe request
    write_queue.enqueue(request);
    write_next();
}
