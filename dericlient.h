/**
 * @file websocket_client.hpp
 * @brief WebSocket client for connecting to Deribit API
 * @date 2025-03-18
 * 
 * This file contains declarations for a WebSocket client implementation
 * that connects to Deribit's API, handles authentication, and provides
 * methods for order management and market data streaming.
 */

 #ifndef WEBSOCKET_CLIENT_HPP
 #define WEBSOCKET_CLIENT_HPP
 
 #include <boost/beast/core.hpp>
 #include <boost/beast/websocket.hpp>
 #include <boost/beast/websocket/ssl.hpp>
 #include <boost/asio.hpp>
 #include <boost/asio/ssl.hpp>
 #include <openssl/pem.h>
 #include <openssl/evp.h>
 #include <string>
 #include <memory>
 #include <unordered_map>
 #include <functional>
 #include <atomic>
 #include "concurrentqueue.h" // moodycamel's concurrent queue

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"

 
 namespace beast = boost::beast;
 namespace websocket = beast::websocket;
 namespace asio = boost::asio;
 namespace ssl = asio::ssl;
 using tcp = asio::ip::tcp;
 
 /**
  * @class DeriClient
  * @brief Client for connecting to Deribit's WebSocket API
  * 
  * This class handles connection to Deribit, authentication, order management,
  * and market data streaming.
  */
 class DeriClient {
 public:
     asio::io_context ioc;
     ssl::context ssl_ctx;
     std::unique_ptr<websocket::stream<ssl::stream<tcp::socket>>> ws;
     std::unordered_map<std::string, std::function<void(const std::string&)>> streaming_handlers;
     std::unordered_map<int, std::chrono::high_resolution_clock::time_point> order_latency_timers;
     std::vector<long long> order_latencies;
     moodycamel::ConcurrentQueue<std::string> write_queue;
     uint64_t request_id;
     std::atomic<bool> is_writing{false};
     const std::string client_id;
     EVP_PKEY* private_key = nullptr;
     quill::Logger* logger;

 
     /**
      * @brief Construct a new WebSocket Client
      * @param key_path Path to the Ed25519 private key file
      * @param client_id Deribit API client ID
      */
     DeriClient(const std::string& key_path, std::string client_id, quill::Logger* logger);
     
     /**
      * @brief Destructor that cleans up resources
      */
     ~DeriClient();
     
     /**
      * @brief Get current timestamp in milliseconds
      * @return Timestamp as a long long
      */
     long long get_current_timestamp();
     
     /**
      * @brief Generate a random nonce string
      * @return Random nonce string
      */
     std::string generate_nonce();
     
     /**
      * @brief Load the Ed25519 private key from file
      * @param private_key_path Path to the private key file
      */
     void load_private_key(const std::string& private_key_path);
     
     /**
      * @brief Sign data using the Ed25519 private key
      * @param data Data to sign
      * @return URL-safe base64 encoded signature
      */
     std::string sign_data(const std::string& data);
     
     /**
      * @brief Enable TCP_NODELAY for reduced latency
      * @param socket TCP socket to configure
      */
     void enable_tcp_nodelay(tcp::socket& socket);
     
     /**
      * @brief Write the next message in the queue
      */
     void write_next();
     
     /**
      * @brief Connect to Deribit's WebSocket API
      * @param host Hostname (e.g., "test.deribit.com")
      * @param port Port (e.g., "443")
      */
     void connect(const std::string& host, const std::string& port);
     
     /**
      * @brief Start asynchronous reading of responses
      */
     void async_read_response();
     
     /**
      * @brief Place an order
      * @param instrument Instrument name (e.g., "BTC-PERPETUAL")
      * @param direction "buy" or "sell"
      * @param price Order price
      * @param amount Order amount
      * @param order_type Order type (default: "limit")
      * @return Future containing the response
      */
     void place_order(const std::string& instrument, const std::string& direction, 
                     double price, double amount, const std::string& order_type = "limit");
     
     /**
      * @brief Cancel an order
      * @param order_id ID of the order to cancel
      * @return Future containing the response
      */
     void cancel_order(const std::string& order_id);
     
     /**
      * @brief Modify an existing order
      * @param order_id ID of the order to modify
      * @param new_price New price
      * @param new_amount New amount
      * @return Future containing the response
      */
     void modify_order(const std::string& order_id, double new_price, double new_amount);
     
     /**
      * @brief Get the order book for an instrument
      * @param instrument Instrument name
      * @param depth Order book depth
      * @return Future containing the response
      */
     void get_order_book(const std::string& instrument, int depth);
     
     /**
      * @brief Get current positions
      * @return Future containing the response
      */
     void get_positions();
     
     /**
      * @brief Subscribe to real-time orderbook updates
      * @param instrument Instrument name
      * @param callback Function to call when updates are received
      */
     void subscribe_to_orderbook(const std::string& instrument, 
         std::function<void(const std::string&)> callback);

     /**
      * @brief Unsubscribe from real-time orderbook updates
      * @param instrument Instrument name
      */
     void unsubscribe_from_orderbook(const std::string& instrument);
 };
 
 #endif // WEBSOCKET_CLIENT_HPP
 