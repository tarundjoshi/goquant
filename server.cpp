#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <set>
#include <unordered_map>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

class WebSocketServer : public std::enable_shared_from_this<WebSocketServer> {
public:
    WebSocketServer(asio::io_context& ioc, tcp::endpoint endpoint)
        : acceptor_(ioc, endpoint), socket_(ioc) {}

    void run() {
        accept_connection();
    }

    void broadcast(const std::string& message) {
        for (auto& client : clients_) {
            if (auto sp = client.lock()) {
                sp->async_write(
                    asio::buffer(message),
                    [self = sp](boost::system::error_code ec, std::size_t) {
                        if (ec) {
                            std::cerr << "Broadcast error: " << ec.message() << "\n";
                        }
                    });
            }
        }
    }

    void register_request(int request_id, std::shared_ptr<websocket::stream<tcp::socket>> client) {
        client_requests_[request_id] = client;
    }

    void handle_response(int request_id, const std::string& response) {
        auto it = client_requests_.find(request_id);
        if (it != client_requests_.end()) {
            auto client = it->second;
            if (client) {
                client->async_write(
                    asio::buffer(response),
                    [client](boost::system::error_code ec, std::size_t) {
                        if (ec) {
                            std::cerr << "Response send error: " << ec.message() << "\n";
                        }
                    });
            }
            client_requests_.erase(it); // Clean up after handling
        } else {
            // If it's not mapped to a client, assume it's public market data → Broadcast
            broadcast(response);
        }
    }

private:
    void accept_connection() {
        acceptor_.async_accept(
            socket_,
            [this](boost::system::error_code ec) {
                if (!ec) {
                    std::cout << "Client connected\n";
                    auto client = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket_));
                    clients_.insert(client);
                    start_session(client);
                }
                accept_connection(); // Keep accepting new clients
            });
    }

    void start_session(std::shared_ptr<websocket::stream<tcp::socket>> client) {
        client->async_accept(
            [this, client](boost::system::error_code ec) {
                if (!ec) {
                    read_from_client(client);
                } else {
                    std::cerr << "Handshake error: " << ec.message() << "\n";
                    clients_.erase(client);
                }
            });
    }

    void read_from_client(std::shared_ptr<websocket::stream<tcp::socket>> client) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        client->async_read(
            *buffer,
            [this, client, buffer](boost::system::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "Read error: " << ec.message() << "\n";
                    clients_.erase(client);
                    return;
                }

                std::string message = beast::buffers_to_string(buffer->data());
                handle_client_message(client, message);
                read_from_client(client); // Keep reading from client
            });
    }

    void handle_client_message(std::shared_ptr<websocket::stream<tcp::socket>> client, const std::string& message) {
        simdjson::dom::parser parser;
        try {
            auto json = parser.parse(message);
            int id = json["id"].get_int64();
            std::string method = json["method"].get_string();

            if (method == "place_order") {
                std::string instrument = json["params"]["instrument_name"].get_string();
                double price = json["params"]["price"].get_double();
                double amount = json["params"]["amount"].get_double();
                std::string order_type = json["params"]["type"].get_string();

                std::cout << "Received order request\n";
                
                // Register the client for the request
                register_request(id, client);

                // Forward to Deribit client
                client_->place_order(instrument, "buy", price, amount, order_type)
                    .then([this, id](std::string response) {
                        handle_response(id, response);
                    });

            } else if (method == "get_order_book") {
                std::string instrument = json["params"]["instrument_name"].get_string();
                int depth = json["params"]["depth"].get_int64();

                std::cout << "Fetching order book for " << instrument << "\n";
                
                client_->get_order_book(instrument, depth)
                    .then([this, id](std::string response) {
                        handle_response(id, response);
                    });

            } else if (method == "get_positions") {
                std::cout << "Fetching current positions\n";

                client_->get_positions()
                    .then([this, id](std::string response) {
                        handle_response(id, response);
                    });
            }

        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "JSON parse error: " << e.what() << "\n";
        }
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_;
    std::set<std::weak_ptr<websocket::stream<tcp::socket>>, std::owner_less<std::weak_ptr<websocket::stream<tcp::socket>>>> clients_;
    std::unordered_map<int, std::shared_ptr<websocket::stream<tcp::socket>>> client_requests_;

public:
    std::shared_ptr<WebSocketClient> client_;
};

// Main
int main() {
    try {
        const std::string private_key_path = "private.pem";

        asio::io_context ioc;
        auto server = std::make_shared<WebSocketServer>(ioc, tcp::endpoint(tcp::v4(), 9002));
        server->client_ = std::make_shared<WebSocketClient>(private_key_path, "CLIENT_ID");

        // Connect to Deribit
        server->client_->connect("test.deribit.com", "443");

        std::thread t([&]() { ioc.run(); });

        server->run(); // Start server

        t.join();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
}
