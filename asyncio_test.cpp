#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <iostream>

namespace beast = boost::beast;
namespace http = beast::http;   
namespace websocket = beast::websocket;
namespace asio = boost::asio;         
using tcp = asio::ip::tcp;         

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(tcp::socket&& socket) : ws_(std::move(socket)) {}

    void run() {
        ws_.async_accept(
            beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        if (ec) {
            std::cerr << "accept error: " << ec.message() << std::endl;
            return;
        }

        do_read();
    }

    void do_read() {
        
        ws_.async_read(buffer_, [this, self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec == websocket::error::closed) {
                std::cout << "WebSocket closed" << std::endl;
                return;
            }
            
            if (ec) {
                std::cerr << "read error: " << ec.message() << std::endl;
                return;
            }

            std::string message = beast::buffers_to_string(buffer_.data());
            std::cout << "Received: " << message << std::endl;

            buffer_.consume(buffer_.size());


            self->do_write(message);
        });

    }

    void do_write(std::string message) {
        ws_.async_write(
            asio::buffer(message),
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
                if (ec) {
                    std::cerr << "write error: " << ec.message() << std::endl;
                    return;
                }

                self->do_read();
            });
    }

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
};

class WebSocketServer {
public:
    WebSocketServer(asio::io_context& ioc, tcp::endpoint endpoint)
        : acceptor_(ioc, endpoint) {
        do_accept();
    }

    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (ec) {
                    std::cerr << "accept error: " << ec.message() << std::endl;
                    return;
                }

                std::make_shared<WebSocketSession>(std::move(socket))->run();
                do_accept();
            });
    }

private:
    tcp::acceptor acceptor_;
};

int main() {
    try {
        asio::io_context ioc{1};
        WebSocketServer server{ioc, {asio::ip::make_address("127.0.0.1"), 8080}};

        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
