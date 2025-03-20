/**
 * @file streamer.h
 * @brief WebSocket server for real-time market data distribution
 * @date 2025-03-20
 *
 * This file contains declarations for a WebSocket server implementation
 * that handles client connections, manages subscriptions to different
 * trading symbols, and efficiently broadcasts market data updates to
 * subscribed clients.
 */

 #ifndef STREAMER_H
 #define STREAMER_H
 
 #include <boost/beast/core.hpp>
 #include <boost/beast/websocket.hpp>
 #include <boost/asio.hpp>
 #include <string>
 #include <vector>
 #include <set>
 #include <unordered_map>
 #include <memory>
 #include <mutex>
 #include <array>
 #include <simdjson.h>
 #include "dericlient.h"
 
 namespace beast = boost::beast;
 namespace websocket = beast::websocket;
 namespace asio = boost::asio;
 using tcp = asio::ip::tcp;
 
 class WebSocketSession;
 
 /**
  * @class OrderBook
  * @brief Represents a single order book with bids and asks.
  *
  * This class maintains the current state of an order book by applying
  * both snapshots and incremental updates.
  */
 class OrderBook {
 public:
     /**
      * @brief Constructs an OrderBook for a specific symbol.
      * @param symbol The trading symbol this order book represents.
      */
     OrderBook(const std::string& symbol);
     
     /**
      * @brief Default constructor for use in containers.
      */
     OrderBook();
 
     /**
      * @brief Updates the order book with new data (either snapshot or incremental update).
      * @param data The data from the exchange.
      * @param is_snapshot Whether this is a full snapshot (true) or an incremental update (false).
      */
     void update(simdjson::dom::element data, bool is_snapshot);
 
     /**
      * @brief Generates a JSON string representing the current state of the order book.
      * @return A JSON string containing the complete order book.
      */
     std::string to_json() const;
 
 private:
     std::string symbol_;
     std::map<double, double, std::greater<double>> bids_; // Price -> Size, sorted descending
     std::map<double, double, std::less<double>> asks_;    // Price -> Size, sorted ascending
     std::string channel_; // Store the channel name for reconstructing messages
     uint64_t timestamp_ = 0; // Latest timestamp
 };
 
 /**
  * @class OrderBookManager
  * @brief Manages order books for different trading symbols.
  *
  * This class is responsible for:
  * - Processing and storing order book data from the exchange
  * - Maintaining up-to-date local copies of order books
  * - Providing access to the latest order book state for each symbol
  * - Thread-safe access to order book data
  */
 class OrderBookManager {
 public:
     /**
      * @brief Constructs an OrderBookManager with the specified logger.
      * @param logger Pointer to a quill logger for logging events.
      */
     OrderBookManager(quill::Logger* logger);
 
     /**
      * @brief Processes an incoming WebSocket message and updates the local order book.
      *
      * This method parses the message, updates the appropriate order book,
      * and maintains an up-to-date local copy.
      *
      * @param message The JSON message received from the WebSocket connection.
      * @return The symbol that was updated, or empty string if not applicable.
      */
     std::string process_message(const std::string& message);
 
     /**
      * @brief Retrieves the latest order book state for a given symbol.
      *
      * @param symbol The trading symbol (e.g., "BTC-PERPETUAL").
      * @return The JSON representation of the current order book, or an empty string if not available.
      */
     std::string get_current_orderbook(const std::string& symbol);
 
     /**
      * @brief Checks if an order book exists for a given symbol.
      *
      * @param symbol The trading symbol to check.
      * @return true if an order book exists, false otherwise.
      */
     bool has_orderbook(const std::string& symbol);
 
 private:
     std::mutex book_mutex_; ///< Mutex for thread-safe access to the order books
     std::unordered_map<std::string, OrderBook> order_books_; ///< Map of symbol -> OrderBook
     quill::Logger* logger; ///< Logger for recording events
     simdjson::dom::parser parser_; ///< JSON parser for processing messages
 };
 
 /**
  * @class SubscriptionManager
  * @brief Manages client subscriptions to market data symbols
  *
  * This class maintains a mapping between symbols and the clients
  * subscribed to them. It handles subscription/unsubscription requests
  * and broadcasts market data updates to interested clients.
  */
 class SubscriptionManager {
 private:
     std::mutex subscription_mutex; ///< Mutex for thread-safe access to the subscribers map
     std::unordered_map<std::string, std::set<std::shared_ptr<WebSocketSession>>> symbol_subscribers; ///< Map of symbol -> set of subscribers
     OrderBookManager order_book_manager_; ///< Manager for order book data
     DeriClient& deri_client_; ///< Client for communicating with the exchange
     quill::Logger* logger; ///< Logger for recording events
     simdjson::dom::parser parser_; ///< JSON parser for processing message
 
 public:
     /**
      * @brief Vector to store market data processing latency measurements in nanoseconds
      */
     std::vector<long long> market_data_processing_latencies;
 
     /**
      * @brief Constructs a SubscriptionManager with the specified client and logger
      * @param deri_client Reference to the Deribit client
      * @param logger Pointer to a quill logger for logging events
      */
     SubscriptionManager(DeriClient& deri_client, quill::Logger* logger);
 
     /**
      * @brief Ensures that we are subscribed to a symbol on Deribit
      * @param symbol The trading symbol to subscribe to
      */
     void ensure_deribit_subscription(const std::string& symbol);
 
     /**
      * @brief Subscribe a client session to a market data symbol
      * @param symbol The trading symbol to subscribe to (e.g., "BTC-PERPETUAL")
      * @param session The client session requesting the subscription
      */
     void subscribe(const std::string& symbol, std::shared_ptr<WebSocketSession> session);
 
     /**
      * @brief Unsubscribe a client session from a market data symbol
      * @param symbol The trading symbol to unsubscribe from
      * @param session The client session requesting unsubscription
      */
     void unsubscribe(const std::string& symbol, std::shared_ptr<WebSocketSession> session);
 
     /**
      * @brief Unsubscribe a client session from all symbols
      * @param session The client session to unsubscribe (raw pointer version)
      *
      * This is typically called when a client disconnects.
      */
     void unsubscribe_all(WebSocketSession* session);
 
     /**
      * @brief Unsubscribe a client session from all symbols
      * @param session The client session to unsubscribe (shared_ptr version)
      *
      * This is typically called when a client disconnects.
      */
     void unsubscribe_all(std::shared_ptr<WebSocketSession> session);
 
     /**
      * @brief Broadcast a market data update to all subscribed clients
      * @param symbol The symbol for which data is being broadcasted
      * @param message The market data message to broadcast
      */
     void broadcast(const std::string& symbol, const std::string& message);
 
     /**
      * @brief Get a list of all symbols with active subscribers
      * @return Vector of symbol strings
      */
     std::vector<std::string> get_active_symbols();
 };
 
 /**
  * @class WebSocketSession
  * @brief Handles an individual client WebSocket connection
  *
  * This class manages the lifecycle of a client connection, including
  * accepting the connection, processing subscription requests, and
  * sending market data updates.
  */
 class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
 private:
     /// The WebSocket stream for this client
     websocket::stream<beast::tcp_stream> ws_;
     /// Buffer for reading messages from the client
     beast::flat_buffer buffer_;
     /// Reference to the subscription manager
     SubscriptionManager& subscription_manager_;
     /// Set of symbols this client is subscribed to
     std::set<std::string> subscribed_symbols_;
     /// Mutex to protect concurrent access to the subscribed symbols set
     std::mutex symbols_mutex_;
     
     /// Queue for outgoing messages
     moodycamel::ConcurrentQueue<std::string> write_queue_;
     /// Flag to track if a write operation is in progress
     std::atomic<bool> is_writing_{false};
     
     quill::Logger* logger;
 
 public:
     /**
      * @brief Construct a new WebSocket session
      * @param socket TCP socket for the connection
      * @param manager Reference to the subscription manager
      * @param logger Logger for recording events
      */
     WebSocketSession(tcp::socket&& socket, SubscriptionManager& manager, quill::Logger* logger);
 
     /**
      * @brief Start the session by accepting the WebSocket handshake
      */
     void run();
 
     /**
      * @brief Send a message to this client
      * @param message The message to send
      *
      * This method is thread-safe and can be called from any thread.
      */
     void send(const std::string& message);
 
     /**
      * @brief Destructor that ensures client is unsubscribed from all symbols
      */
     ~WebSocketSession();
 
 private:
     /**
      * @brief Process the next message in the write queue
      */
     void write_next();
 
     /**
      * @brief Handler for WebSocket accept completion
      * @param ec Error code
      */
     void on_accept(beast::error_code ec);
 
     /**
      * @brief Start an asynchronous read operation
      */
     void do_read();
 
     /**
      * @brief Handler for read completion
      * @param ec Error code
      * @param bytes_transferred Number of bytes read
      */
     void on_read(beast::error_code ec, std::size_t bytes_transferred);
 
     /**
      * @brief Handler for write completion
      * @param ec Error code
      * @param bytes_transferred Number of bytes written
      */
     void on_write(beast::error_code ec, std::size_t bytes_transferred);
 
     /**
      * @brief Process a subscription/unsubscription message from the client
      * @param message The message to process
      */
     void process_message(const std::string& message);
 };
 
 /**
  * @class Server
  * @brief Accepts incoming WebSocket connections
  *
  * This class listens for incoming TCP connections and creates a new
  * WebSocketSession for each accepted connection.
  */
 class Server : public std::enable_shared_from_this<Server> {
 private:
     /// The I/O context
     asio::io_context& ioc_;
     quill::Logger* logger;
     /// The acceptor for incoming connections
     tcp::acceptor acceptor_;
     /// Reference to the subscription manager
     SubscriptionManager& subscription_manager_;
 
 public:
     /**
      * @brief Construct a new Server
      * @param ioc The I/O context
      * @param endpoint The endpoint to listen on
      * @param manager Reference to the subscription manager
      * @param logger Logger for recording events
      */
     Server(asio::io_context& ioc, tcp::endpoint endpoint, SubscriptionManager& manager, quill::Logger* logger);
 
     /**
      * @brief Start accepting connections
      */
     void run();
 
 private:
     /**
      * @brief Start an asynchronous accept operation
      */
     void do_accept();
 
     /**
      * @brief Handler for accept completion
      * @param ec Error code
      * @param socket The accepted socket
      */
     void on_accept(beast::error_code ec, tcp::socket socket);
 };
 
 #endif // STREAMER_H
 