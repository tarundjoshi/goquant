/**
 * @file websocket_server.cpp
 * @brief Implementation of the WebSocket server components
 * @date 2025-03-18
 */

 #include "streamer.h"
 #include <iostream>
 #include <simdjson.h>
 
 // SubscriptionManager implementation

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
     symbol_subscribers[symbol].insert(session);
    //  std::cout << "Session subscribed to " << symbol << std::endl;
    LOG_INFO(logger, "Session subscribed to {}", symbol);
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
     // Post the message to the strand to ensure thread safety
     asio::post(
         ws_.get_executor(),
         [self = shared_from_this(), message]() {
             self->do_send(message);
         });
 }
 
 void WebSocketSession::do_send(const std::string& message) {
     // Copy message to the fixed-size buffer to avoid heap allocations
     size_t message_size = std::min(message.size(), send_buffer_.size());
     std::copy_n(message.data(), message_size, send_buffer_.data());
     
     // Write asynchronously
     ws_.async_write(
         asio::buffer(send_buffer_.data(), message_size),
         beast::bind_front_handler(
             &WebSocketSession::on_write,
             shared_from_this()));
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
     ws_.async_read(
         buffer_,
         beast::bind_front_handler(
             &WebSocketSession::on_read,
             shared_from_this()));
 }
 
 void WebSocketSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    // Check for connection closed or reset
    if(ec == websocket::error::closed || ec == boost::asio::error::connection_reset) {
        LOG_INFO(logger, "WebSocket connection closed: {}", ec.message());
        // Clean up resources properly
        subscription_manager_.unsubscribe_all(this);
        return;  // Don't attempt further reads
    }
     
     if(ec) {
        //  std::cerr << "Read failed: " << ec.message() << std::endl;
        LOG_ERROR(logger, "Read failed: {}", ec.message());
         return;
     }
     
     // Process the message (e.g., handle subscriptions)
     process_message(beast::buffers_to_string(buffer_.data()));
     
     // Clear the buffer
     buffer_.consume(buffer_.size());
     
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
             symbol = std::string(json["symbol"].get_string().value());
             
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
         std::cerr << "Error processing message: " << e.what() << std::endl;
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
 