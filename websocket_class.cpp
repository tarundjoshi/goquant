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
#include <concurrentqueue.h>
#include "dericlient.h"
#include "streamer.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"  // For file logging

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace simdjson;

#define CLIENT_ID "zavRq-oY"
#define KEY_PATH "private.pem"
#define STREAM_ADDR "127.0.0.1"
#define STREAM_PORT 8080


int main() {
    try {
        // Initialize Quill logging
        quill::BackendOptions backend_options;
        backend_options.thread_name = "QuillLogger";
        quill::Backend::start(backend_options);

        // Create logs directory if it doesn't exist
        std::filesystem::create_directories("logs");

        // Create sinks
        auto file_sink_client = quill::Frontend::create_or_get_sink<quill::FileSink>("logs/dericlient.log");
        auto file_sink_server = quill::Frontend::create_or_get_sink<quill::FileSink>("logs/server.log");
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");

        // Create loggers with pattern formatting
        quill::Logger* client_logger = quill::Frontend::create_or_get_logger(
            "DeriClient", 
            {file_sink_client, console_sink},
            quill::PatternFormatterOptions{"[%(time)] [%(logger)] [%(log_level)] %(message)"});

        quill::Logger* server_logger = quill::Frontend::create_or_get_logger(
            "Streamer", 
            {file_sink_server, console_sink},
            quill::PatternFormatterOptions{"[%(time)] [%(logger)] [%(log_level)] %(message)"});

        // Set log levels - console shows INFO and above, files show DEBUG and above
        file_sink_client->set_log_level_filter(quill::LogLevel::Debug);
        file_sink_server->set_log_level_filter(quill::LogLevel::Debug);
        console_sink->set_log_level_filter(quill::LogLevel::Info);

        client_logger->set_log_level(quill::LogLevel::Debug);
        server_logger->set_log_level(quill::LogLevel::Debug);

        // Configuration
        const std::string private_key_path = KEY_PATH;
        const auto server_address = asio::ip::make_address(STREAM_ADDR);
        const unsigned short server_port = STREAM_PORT;
        
        // Create the io_context for the WebSocket server
        asio::io_context server_ioc{1};

        // Create the Deribit client
        DeriClient client(private_key_path, CLIENT_ID, client_logger);
        
        // // Create the subscription manager
        SubscriptionManager subscription_manager(client, server_logger);
        
        // Create and run the Server for the WebSocket server
        std::make_shared<Server>(
            server_ioc, 
            tcp::endpoint{server_address, server_port}, 
            subscription_manager,
            server_logger)->run();
        
        
        // Connect to Deribit
        client.connect("test.deribit.com", "443");
        
        // Run the server in a separate thread
        std::thread server_thread([&server_ioc]() {
            server_ioc.run();
        });
        
        // Run the client in a separate thread
        std::thread client_thread([&client]() {
            client.ioc.run();
        });
        
        // Subscribe to market data for various instruments
        std::vector<std::string> instruments = {
            "BTC-PERPETUAL", 
            "ETH-PERPETUAL", 
            "BTC-25MAR25-60000-C",  // Example option
            "ETH_USDC"              // Example spot
        };
        
        // Wait for the threads (this won't be reached in this example)

        client.place_order("ADA_USDC-PERPETUAL", "buy", 0.72, 10, "market");
        client.place_order("ADA_USDC-PERPETUAL", "buy", 0.72, 0.72, "market");

        
        server_thread.join();
        client_thread.join();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

