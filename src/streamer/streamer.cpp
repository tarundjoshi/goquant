/**
 * @file websocket_server.cpp
 * @brief Implementation of the WebSocket server components
 * @date 2025-03-18
 */

 #include "streamer.h"
 #include <iostream>
 #include <simdjson.h>
 
 
 // SubscriptionManager implementation

SubscriptionManager::SubscriptionManager(DeriClient& deri_client, quill::Logger* logger)
: deri_client_(deri_client), logger(logger), order_book_manager_(logger) {}

 void SubscriptionManager::ensure_deribit_subscription(const std::string& symbol) {
    // Check if we're already subscribed to this symbol on Deribit
    if (deri_client_.streaming_handlers.find("book." + symbol + ".100ms") == 
        deri_client_.streaming_handlers.end()) {
        
        // std::cout << "Subscribing to " << symbol << " on Deribit" << std::endl;
        LOG_INFO(logger, "Subscribing to {} on Deribit", symbol);
        
        // Subscribe to the symbol on Deribit
        deri_client_.subscribe_to_orderbook(symbol, [this, symbol](const std::string& data) {
            // Forward the data to all subscribers
            this->broadcast(symbol, data);
            // std::cout << "Broadcasted update for " << symbol << std::endl;
            LOG_INFO(logger, "Broadcasted update for {}", symbol);
        });
    }
}
 
void SubscriptionManager::subscribe(const std::string& symbol, std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(subscription_mutex);
    
    // Add to subscribers list
    symbol_subscribers[symbol].insert(session);
    LOG_INFO(logger, "Session subscribed to {}", symbol);
    
    // Check if we have an up-to-date order book for this symbol
    if (order_book_manager_.has_orderbook(symbol)) {
        // Send the current order book to the new client
        std::string current_orderbook = order_book_manager_.get_current_orderbook(symbol);
        if (!current_orderbook.empty()) {
            LOG_INFO(logger, "Sending current order book for {} to new client", symbol);
            session->send(current_orderbook);
        }
    }

}
 
 void SubscriptionManager::unsubscribe(const std::string& symbol, std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(subscription_mutex);
    auto it = symbol_subscribers.find(symbol);
    if (it != symbol_subscribers.end()) {
        it->second.erase(session);
        
        if (it->second.empty()) {
            symbol_subscribers.erase(it);
            LOG_INFO(logger, "Session unsubscribed from {}. No more subscribers, unsubscribing from Deribit", symbol);
            deri_client_.unsubscribe_from_orderbook(symbol);
        } else {
            LOG_INFO(logger, "Session unsubscribed from {}. {} subscribers remaining", 
                    symbol, it->second.size());
        }
    }
}

 
void SubscriptionManager::unsubscribe_all(WebSocketSession* raw_ptr) {
    std::lock_guard<std::mutex> lock(subscription_mutex);
    std::vector<std::string> symbols_to_unsubscribe;
    
    // First collect symbols that will have no subscribers
    for (auto it = symbol_subscribers.begin(); it != symbol_subscribers.end(); ++it) {
        auto& subscribers = it->second;
        // Check if this session is the only subscriber
        if (subscribers.size() == 1 && 
            std::any_of(subscribers.begin(), subscribers.end(), 
                       [raw_ptr](const std::shared_ptr<WebSocketSession>& s) { 
                           return s.get() == raw_ptr; 
                       })) {
            symbols_to_unsubscribe.push_back(it->first);
        }
    }
    
    // Now remove the session from all subscription lists
    for (auto it = symbol_subscribers.begin(); it != symbol_subscribers.end();) {
        auto& subscribers = it->second;
        // Manual loop for removing from the set
        for (auto sub_it = subscribers.begin(); sub_it != subscribers.end();) {
            if (sub_it->get() == raw_ptr) {
                sub_it = subscribers.erase(sub_it);
            } else {
                ++sub_it;
            }
        }
        
        if (subscribers.empty()) {
            it = symbol_subscribers.erase(it);
        } else {
            ++it;
        }
    }
    
    LOG_INFO(logger, "Session unsubscribed from all symbols");
    
    // Unsubscribe from Deribit for symbols with no more subscribers
    for (const auto& symbol : symbols_to_unsubscribe) {
        LOG_INFO(logger, "Unsubscribing from {} on Deribit (no more subscribers)", symbol);
        deri_client_.unsubscribe_from_orderbook(symbol);
    }
}


 
 void SubscriptionManager::broadcast(const std::string& symbol, const std::string& message) {
     // Create a vector to store sessions to avoid holding the lock during sending
     std::vector<std::shared_ptr<WebSocketSession>> sessions_to_notify;

     auto start = std::chrono::high_resolution_clock::now();

     order_book_manager_.process_message(message);

     auto end = std::chrono::high_resolution_clock::now();
     auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
     market_data_processing_latencies.push_back(duration.count());
     
     {
         std::lock_guard<std::mutex> lock(subscription_mutex);
         auto it = symbol_subscribers.find(symbol);
         if (it != symbol_subscribers.end()) {
             sessions_to_notify.reserve(it->second.size());
             for (auto& session : it->second) {
                 sessions_to_notify.push_back(session);
             }
         }
     }
     
     // Send the message to all subscribers outside the lock
     for (auto& session : sessions_to_notify) {
         session->send(message);
     }
 }
 
 std::vector<std::string> SubscriptionManager::get_active_symbols() {
     std::lock_guard<std::mutex> lock(subscription_mutex);
     std::vector<std::string> symbols;
     symbols.reserve(symbol_subscribers.size());
     for (const auto& pair : symbol_subscribers) {
         symbols.push_back(pair.first);
     }
     return symbols;
 }
 
 // WebSocketSession implementation
 
 WebSocketSession::WebSocketSession(tcp::socket&& socket, SubscriptionManager& manager, quill::Logger* logger)
     : ws_(std::move(socket)), subscription_manager_(manager), logger(logger) {
     // Enable TCP_NODELAY for reduced latency
     ws_.next_layer().socket().set_option(tcp::no_delay(true));
     
     // Disable WebSocket buffering for reduced write delay
     ws_.auto_fragment(false);
     ws_.write_buffer_bytes(4096);
 }
 
 void WebSocketSession::run() {
     // Set timeout options
     ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
     
     // Accept the WebSocket handshake
     ws_.async_accept(
         beast::bind_front_handler(
             &WebSocketSession::on_accept,
             shared_from_this()));
 }
 
 void WebSocketSession::send(const std::string& message) {
    // Add message to the queue
    write_queue_.enqueue(message);
    // Trigger write operation if not already writing
    write_next();
}
 
void WebSocketSession::write_next() {
    if (is_writing_.load()) return;
    
    std::string msg;
    if (!write_queue_.try_dequeue(msg)) return; // Empty queue check
    
    // Try to set is_writing to true (if already true, don't proceed)
    if (!is_writing_.exchange(true)) {
        ws_.async_write(
            asio::buffer(msg),
            [this](beast::error_code ec, std::size_t bytes_transferred) {
                is_writing_.store(false);
                if (ec) {
                    LOG_ERROR(logger, "Write error: {}", ec.message());
                    return;
                }
                
                LOG_DEBUG(logger, "Successfully sent message: {} bytes", bytes_transferred);
                write_next(); // Process next message in queue
            });
    }
}
 
 void WebSocketSession::on_accept(beast::error_code ec) {
     if(ec) {
        //  std::cerr << "Accept failed: " << ec.message() << std::endl;
        LOG_ERROR(logger, "Accept failed: {}", ec.message());
         return;
     }
     
    //  std::cout << "WebSocket connection established" << std::endl;
    LOG_INFO(logger, "WebSocket connection established");
     
     // Start reading messages
     do_read();
 }
 
 void WebSocketSession::do_read() {
    auto buffer = buffer_pool.acquire();
    
    ws_.async_read(
        *buffer,
        [self = shared_from_this(), buffer](beast::error_code ec, std::size_t bytes_transferred) {
            self->on_read(ec, bytes_transferred, buffer);
        });
}

void WebSocketSession::on_read(beast::error_code ec, std::size_t bytes_transferred, 
                              std::shared_ptr<beast::flat_buffer> buffer) {
    // Check for connection closed or reset
    if(ec == websocket::error::closed || ec == boost::asio::error::connection_reset) {
        LOG_INFO(logger, "WebSocket connection closed: {}", ec.message());
        // Clean up resources properly
        subscription_manager_.unsubscribe_all(this);
        buffer_pool.release(buffer); // Release the buffer before returning
        return;  // Don't attempt further reads
    }
     
    if(ec) {
        LOG_ERROR(logger, "Read failed: {}", ec.message());
        buffer_pool.release(buffer); // Release the buffer before returning
        return;
    }
     
    // Process the message (e.g., handle subscriptions)
    process_message(beast::buffers_to_string(buffer->data()));
     
    // Release the buffer back to the pool
    buffer_pool.release(buffer);
     
    // Read the next message
    do_read();
}

 
 void WebSocketSession::on_write(beast::error_code ec, std::size_t bytes_transferred) {
     if(ec) {
        //  std::cerr << "Write failed: " << ec.message() << std::endl;
        LOG_ERROR(logger, "Write failed: {}", ec.message());
         return;
     }
 }
 
 void WebSocketSession::process_message(const std::string& message) {
     try {
         simdjson::dom::parser parser;
         auto json = parser.parse(message);
         
         std::string action;
         std::string symbol;
         
         // Try to parse as JSON
         try {
             action = std::string(json["action"].get_string().value());

             if (action == "ping") {
                // Extract ping ID if present
                uint64_t ping_id = 0;
                if (json["id"].get(ping_id) == simdjson::SUCCESS) {
                    // Send pong response with same ID
                    std::string pong_msg = "{\"action\":\"pong\",\"id\":" + std::to_string(ping_id) + "}";
                    LOG_INFO(logger, "Received ping message with ID: {}, replying back.", ping_id);
                    send(pong_msg);
                }
             
             } else {
                symbol = std::string(json["symbol"].get_string().value());
             }

             if (action == "subscribe") {
                 {
                     std::lock_guard<std::mutex> lock(symbols_mutex_);
                     subscribed_symbols_.insert(symbol);
                 }
                 subscription_manager_.subscribe(symbol, shared_from_this());

                 subscription_manager_.ensure_deribit_subscription(symbol);

                 
                 // Send confirmation
                 send("{\"status\":\"subscribed\",\"symbol\":\"" + symbol + "\"}");
             } else if (action == "unsubscribe") {
                 {
                     std::lock_guard<std::mutex> lock(symbols_mutex_);
                     subscribed_symbols_.erase(symbol);
                 }
                 subscription_manager_.unsubscribe(symbol, shared_from_this());
                 
                 // Send confirmation
                 send("{\"status\":\"unsubscribed\",\"symbol\":\"" + symbol + "\"}");
             }
         } catch (...) {
            //  std::cerr << "Invalid message format: " << message << std::endl;
            LOG_ERROR(logger, "Invalid message format: {}", message);
             send("{\"error\":\"Invalid message format\"}");
         }
     } catch (const std::exception& e) {
        //  std::cerr << "Error processing message: " << e.what() << std::endl;
        LOG_ERROR(logger, "Error processing message: {}", e.what());
         send("{\"error\":\"Invalid message format\"}");
     }
 }
 
 WebSocketSession::~WebSocketSession() {
     subscription_manager_.unsubscribe_all(this);
    //  std::cout << "Session destroyed" << std::endl;
     LOG_INFO(logger, "Session destroyed");
 }
 
 // Server implementation
 
 Server::Server(asio::io_context& ioc, tcp::endpoint endpoint, SubscriptionManager& manager, quill::Logger* logger)
     : ioc_(ioc), acceptor_(ioc), subscription_manager_(manager), logger(logger) {
     
     beast::error_code ec;
     
     // Open the acceptor
     acceptor_.open(endpoint.protocol(), ec);
     if(ec) {
        //  std::cerr << "Open failed: " << ec.message() << std::endl;
         LOG_ERROR(logger, "Open failed: {}", ec.message());
         return;
     }
     
     // Allow address reuse
     acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
     if(ec) {
        //  std::cerr << "Set option failed: " << ec.message() << std::endl;
         LOG_ERROR(logger, "Set option failed: {}", ec.message());
         return;
     }
     
     // Bind to the server address
     acceptor_.bind(endpoint, ec);
     if(ec) {
        //  std::cerr << "Bind failed: " << ec.message() << std::endl;
         LOG_ERROR(logger, "Bind failed: {}", ec.message());
         return;
     }
     
     // Start listening for connections
     acceptor_.listen(asio::socket_base::max_listen_connections, ec);
     if(ec) {
        //  std::cerr << "Listen failed: " << ec.message() << std::endl;
         LOG_ERROR(logger, "Listen failed: {}", ec.message());
         return;
     }
     
    //  std::cout << "WebSocket server listening on " << 
    //      endpoint.address().to_string() << ":" << endpoint.port() << std::endl;
     LOG_INFO(logger, "WebSocket server listening on {}:{}",
              endpoint.address().to_string(), endpoint.port());
 }
 
 void Server::run() {
     do_accept();
 }
 
 void Server::do_accept() {
     // The new connection gets its own strand
     acceptor_.async_accept(
         asio::make_strand(ioc_),
         beast::bind_front_handler(
             &Server::on_accept,
             shared_from_this()));
 }
 
 void Server::on_accept(beast::error_code ec, tcp::socket socket) {
     if(ec) {
        //  std::cerr << "Accept failed: " << ec.message() << std::endl;
         LOG_ERROR(logger, "Accept failed: {}", ec.message());
     } else {
         // Create the session and run it
         std::make_shared<WebSocketSession>(std::move(socket), subscription_manager_, logger)->run();
        //  std::cout << "New WebSocket connection accepted" << std::endl;
         LOG_INFO(logger, "New WebSocket connection accepted");
     }
     
     // Accept another connection
     do_accept();
 }

 OrderBook::OrderBook(const std::string& symbol) : symbol_(symbol) {}

 OrderBook::OrderBook() : symbol_("") {}

 void OrderBook::update(simdjson::dom::element data, bool is_snapshot) {
    // If this is a snapshot, clear existing data
    if (is_snapshot) {
        bids_.clear();
        asks_.clear();
    }
    
    // Store channel name if available
    std::string_view channel;
    if (data["channel"].get(channel) == simdjson::SUCCESS) {
        channel_ = std::string(channel);
    }
    
    // Extract timestamp if available
    uint64_t timestamp;
    if (data["timestamp"].get(timestamp) == simdjson::SUCCESS) {
        timestamp_ = timestamp;
    }
    
    // Process bids
    simdjson::dom::element bids;
    if (data["bids"].get(bids) == simdjson::SUCCESS && bids.type() == simdjson::dom::element_type::ARRAY) {
        for (auto bid_entry : bids) {
            // Each bid is an array like ["change", 85892.5, 74380.0]
            std::string_view action;
            double price = 0.0, size = 0.0;
            
            if (bid_entry.at(0).get(action) == simdjson::SUCCESS && 
                bid_entry.at(1).get(price) == simdjson::SUCCESS && 
                bid_entry.at(2).get(size) == simdjson::SUCCESS) {
                
                if (size > 0) {
                    // Add or update price level
                    bids_[price] = size;
                } else {
                    // Remove price level (only for updates, not snapshots)
                    if (!is_snapshot) {
                        bids_.erase(price);
                    }
                }
            }
        }
    }
    
    // Process asks
    simdjson::dom::element asks;
    if (data["asks"].get(asks) == simdjson::SUCCESS && asks.type() == simdjson::dom::element_type::ARRAY) {
        for (auto ask_entry : asks) {
            // Each ask is an array like ["new", 88189.5, 30.0]
            std::string_view action;
            double price = 0.0, size = 0.0;
            
            if (ask_entry.at(0).get(action) == simdjson::SUCCESS && 
                ask_entry.at(1).get(price) == simdjson::SUCCESS && 
                ask_entry.at(2).get(size) == simdjson::SUCCESS) {
                
                if (size > 0) {
                    // Add or update price level
                    asks_[price] = size;
                } else {
                    // Remove price level (only for updates, not snapshots)
                    if (!is_snapshot) {
                        asks_.erase(price);
                    }
                }
            }
        }
    }
}


std::string OrderBook::to_json() const {
    // Create a JSON string that mimics the original snapshot format
    std::stringstream ss;
    
    ss << "{\"jsonrpc\":\"2.0\",\"method\":\"subscription\",\"params\":{";
    
    // Add channel if available
    if (!channel_.empty()) {
        ss << "\"channel\":\"" << channel_ << "\",";
    }
    
    ss << "\"data\":{";
    
    // Add timestamp if available
    if (timestamp_ > 0) {
        ss << "\"timestamp\":" << timestamp_ << ",";
    }
    
    // Add type (always snapshot for reconstructed messages)
    ss << "\"type\":\"snapshot\",";
    
    // Add change_id if available (use timestamp as fallback)
    ss << "\"change_id\":" << (timestamp_ > 0 ? timestamp_ : std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) << ",";
    
    // Add instrument name
    ss << "\"instrument_name\":\"" << symbol_ << "\",";
    
    // Add bids
    ss << "\"bids\":[";
    bool first = true;
    for (const auto& bid : bids_) {
        if (!first) ss << ",";
        
        // Format price with scientific notation if needed
        std::ostringstream price_ss;
        if (bid.first >= 100000 && std::fmod(bid.first, 1000) == 0) {
            price_ss << std::scientific << std::setprecision(1) << bid.first;
        } else {
            price_ss << std::fixed << std::setprecision(1) << bid.first;
        }
        
        // Format size with scientific notation if needed
        std::ostringstream size_ss;
        if (bid.second >= 10000 && std::fmod(bid.second, 1000) == 0) {
            size_ss << std::scientific << std::setprecision(1) << bid.second;
        } else {
            size_ss << std::fixed << std::setprecision(1) << bid.second;
        }
        
        ss << "[\"new\"," << price_ss.str() << "," << size_ss.str() << "]";
        first = false;
    }
    ss << "],";
    
    // Add asks
    ss << "\"asks\":[";
    first = true;
    for (const auto& ask : asks_) {
        if (!first) ss << ",";
        
        // Format price with scientific notation if needed
        std::ostringstream price_ss;
        if (ask.first >= 100000 && std::fmod(ask.first, 1000) == 0) {
            price_ss << std::scientific << std::setprecision(1) << ask.first;
        } else {
            price_ss << std::fixed << std::setprecision(1) << ask.first;
        }
        
        // Format size with scientific notation if needed
        std::ostringstream size_ss;
        if (ask.second >= 10000 && std::fmod(ask.second, 1000) == 0) {
            size_ss << std::scientific << std::setprecision(1) << ask.second;
        } else {
            size_ss << std::fixed << std::setprecision(1) << ask.second;
        }
        
        ss << "[\"new\"," << price_ss.str() << "," << size_ss.str() << "]";
        first = false;
    }
    ss << "]";
    
    // Close the JSON structure
    ss << "}}}";
    
    return ss.str();
}



OrderBookManager::OrderBookManager(quill::Logger* logger) : logger(logger) {}

std::string OrderBookManager::process_message(const std::string& message) {
    try {
        auto json = parser_.parse(message);
        
        // Check if this is an orderbook message
        std::string_view channel;
        if (json["params"]["channel"].get(channel) == simdjson::SUCCESS) {
            // Extract symbol from channel (format: "book.BTC-PERPETUAL.100ms")
            std::string symbol;
            size_t first_dot = channel.find('.');
            size_t last_dot = channel.rfind('.');
            if (first_dot != std::string::npos && last_dot != std::string::npos && first_dot != last_dot) {
                symbol = std::string(channel.substr(first_dot + 1, last_dot - first_dot - 1));
            }
            
            if (!symbol.empty()) {
                // Get the data element
                simdjson::dom::element data;
                if (json["params"]["data"].get(data) == simdjson::SUCCESS) {
                    // Check if this is a snapshot or update
                    std::string_view type;
                    bool is_snapshot = false;
                    if (data["type"].get(type) == simdjson::SUCCESS) {
                        is_snapshot = (type == "snapshot");
                    }
                    
                    std::lock_guard<std::mutex> lock(book_mutex_);
                    
                    // Create order book if it doesn't exist
                    if (order_books_.find(symbol) == order_books_.end()) {
                        order_books_[symbol] = OrderBook(symbol);
                    }
                    
                    // // Update the order book
                    order_books_[symbol].update(data, is_snapshot);
                    
                    if (is_snapshot) {
                        LOG_INFO(logger, "Applied order book snapshot for {}", symbol);
                    } else {
                        LOG_INFO(logger, "Applied order book update for {}", symbol);
                    }
                    
                    return symbol;
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        LOG_ERROR(logger, "Error processing message: {}", e.what());
    }
    
    return ""; // Return empty string if not an order book message or error
}

std::string OrderBookManager::get_current_orderbook(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    auto it = order_books_.find(symbol);
    if (it != order_books_.end()) {
        return it->second.to_json();
    }
    return "";
}

bool OrderBookManager::has_orderbook(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    return order_books_.find(symbol) != order_books_.end();
}
 