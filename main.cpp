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

// for demo purposes
std::vector<std::string> order_ids;

void print_latency_statistics(const std::string& metric_name, const std::vector<long long int>& latencies, quill::Logger* logger) {
    if (latencies.empty()) {
        LOG_INFO(logger, "{}: No measurements recorded", metric_name);
        return;
    }
    
    // Create a copy for calculations (with proper type)
    std::vector<long long int> sorted_latencies = latencies;
    std::sort(sorted_latencies.begin(), sorted_latencies.end());
    
    // Calculate statistics (convert to double for calculations)
    double min = static_cast<double>(sorted_latencies.front());
    double max = static_cast<double>(sorted_latencies.back());
    double sum = 0.0;
    for (const auto& latency : sorted_latencies) {
        sum += static_cast<double>(latency);
    }
    double avg = sum / sorted_latencies.size();
    double median = static_cast<double>(sorted_latencies[sorted_latencies.size() / 2]);
    
    // Calculate standard deviation
    double variance = 0.0;
    for (const auto& latency : sorted_latencies) {
        double latency_d = static_cast<double>(latency);
        variance += (latency_d - avg) * (latency_d - avg);
    }
    variance /= sorted_latencies.size();
    double std_dev = std::sqrt(variance);
    
    // Print results
    LOG_INFO(logger, "===== {} Statistics (ns) =====", metric_name);
    LOG_INFO(logger, "  Sample count: {}", sorted_latencies.size());
    LOG_INFO(logger, "  Min: {:.2f}", min);
    LOG_INFO(logger, "  Max: {:.2f}", max);
    LOG_INFO(logger, "  Average: {:.2f}", avg);
    LOG_INFO(logger, "  Median: {:.2f}", median);
    LOG_INFO(logger, "  Standard deviation: {:.2f}", std_dev);
}



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

        auto file_sink_perf = quill::Frontend::create_or_get_sink<quill::FileSink>("logs/performance.log");
        quill::Logger* perf_logger = quill::Frontend::create_or_get_logger(
            "Performance",
            {file_sink_perf},
            quill::PatternFormatterOptions{"[%(time)] [%(logger)] %(message)"});

        // Set log level for performance logs
        file_sink_perf->set_log_level_filter(quill::LogLevel::Info);
        perf_logger->set_log_level(quill::LogLevel::Info);

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

        // Test perpetual (BTC-PERPETUAL exists and uses whole number contract sizes)
        client.place_order("BTC-PERPETUAL", "buy", 85000.0, 10, "limit");

        // Test future (adjusted to a future date that would exist in March 2025)
        client.place_order("BTC-28MAR25", "buy", 86000.0, 10, "market");

        // Test option (adjusted to a valid option expiry that would exist in March 2025)
        client.place_order("BTC-28MAR25-60000-C", "buy", 0.344, 2, "market");

        // Test spot (BTC_USDC exists, adjusted price to whole number per tick size change)
        client.place_order("BTC_USDC", "buy", 85000.0, 0.1, "limit");

        // Test perpetual
        client.place_order("BTC-PERPETUAL", "sell", 85000.0, 10, "limit");

        // Test future
        client.place_order("BTC-28MAR25", "sell", 86000.0, 10, "market");

        // Test option
        client.place_order("BTC-28MAR25-60000-C", "sell", 1, 2, "limit");

        // Test spot
        client.place_order("BTC_USDC", "sell", 85000.0, 0.1, "market");

        // Sleep to wait for responses
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // Try to modify all orders and make amount to 13 and price to 85000
        for(auto order_id : order_ids) {
            client.modify_order(order_id, 85000, 20);
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));

        // Cancel all orders
        for(auto order_id : order_ids) {
            client.cancel_order(order_id);
        }

        // Test positions
        client.get_positions();

        // Test order book
        client.get_order_book("BTC-PERPETUAL", 10);


        std::this_thread::sleep_for(std::chrono::seconds(10));

        client.ioc.stop();
        server_ioc.stop();

        print_latency_statistics("Order placement statistics", client.order_latencies, perf_logger);
        print_latency_statistics("Market data processing statistics", subscription_manager.market_data_processing_latencies, perf_logger);

        
        
        server_thread.join();
        client_thread.join();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

