#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <iostream>
#include <string>
#include <chrono>
#include <random>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// Generate current timestamp in milliseconds
long long get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Generate a random nonce
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

// Sign data using Ed25519 private key
std::string sign_data(const std::string& data_to_sign, const std::string& private_key_path) {
    FILE* private_key_file = fopen(private_key_path.c_str(), "r");
    if (!private_key_file) {
        throw std::runtime_error("Failed to open private key file");
    }

    EVP_PKEY* private_key = PEM_read_PrivateKey(private_key_file, nullptr, nullptr, nullptr);
    fclose(private_key_file);

    if (!private_key || EVP_PKEY_id(private_key) != EVP_PKEY_ED25519) {
        throw std::runtime_error("Invalid or unsupported private key type. Expected Ed25519.");
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(private_key);
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, private_key) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(private_key);
        throw std::runtime_error("Failed to initialize signing");
    }

    size_t signature_len = 0;
    if (EVP_DigestSign(md_ctx, nullptr, &signature_len,
                       reinterpret_cast<const unsigned char*>(data_to_sign.c_str()), data_to_sign.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(private_key);
        throw std::runtime_error("Failed to calculate signature length");
    }

    std::vector<unsigned char> signature(signature_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &signature_len,
                       reinterpret_cast<const unsigned char*>(data_to_sign.c_str()), data_to_sign.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(private_key);
        throw std::runtime_error("Failed to generate signature");
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(private_key);

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

    std::replace(base64_string.begin(), base64_string.end(), '+', '-');
    std::replace(base64_string.begin(), base64_string.end(), '/', '_');
    base64_string.erase(std::remove(base64_string.begin(), base64_string.end(), '='), base64_string.end());
    return base64_string;
}

// Enable TCP_NODELAY for reduced latency
void enable_tcp_nodelay(tcp::socket& socket) {
    boost::asio::ip::tcp::no_delay option(true);
    socket.set_option(option);
}


int main() {
    try {
        const std::string client_id = "zavRq-oY";
        const std::string private_key_path = "private.pem";
        long long timestamp = get_current_timestamp();
        std::string nonce = generate_nonce();
        std::string data_to_sign = std::to_string(timestamp) + "\n" + nonce + "\n";
        auto signature = sign_data(data_to_sign, private_key_path);

        json auth_request = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "public/auth"},
            {"params", {
                {"grant_type", "client_signature"},
                {"client_id", client_id},
                {"timestamp", timestamp},
                {"signature", signature},
                {"nonce", nonce},
                {"data", ""}
            }}
        };

        asio::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        ssl_ctx.set_default_verify_paths();
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve("test.deribit.com", "443");

        websocket::stream<ssl::stream<tcp::socket>> ws(ioc, ssl_ctx);
        asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());
        enable_tcp_nodelay(ws.next_layer().next_layer());
        
        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake("test.deribit.com", "/ws/api/v2");

        ws.write(asio::buffer(auth_request.dump()));
        beast::flat_buffer buffer;
        ws.read(buffer);
        std::cout << "Response: " << beast::make_printable(buffer.data()) << std::endl;
        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}
