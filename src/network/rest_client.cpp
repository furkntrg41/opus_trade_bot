// ============================================================================
// OPUS TRADE BOT - REST Client Implementation
// ============================================================================
// Boost.Beast HTTP client with connection pooling and request signing
// ============================================================================

#include "opus/network/rest_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace opus::network {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ============================================================================
// HMAC-SHA256 Helper
// ============================================================================

static std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         digest, &digest_len);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(digest[i]);
    }
    return ss.str();
}

static std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;

    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
        }
    }

    return escaped.str();
}

static std::string build_query_string(const std::map<std::string, std::string>& params) {
    std::ostringstream ss;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) ss << '&';
        ss << url_encode(key) << '=' << url_encode(value);
        first = false;
    }
    return ss.str();
}

// ============================================================================
// REST Client Implementation
// ============================================================================

struct RestClient::Impl {
    using http_stream = beast::ssl_stream<beast::tcp_stream>;

    explicit Impl(const RestClientConfig& config)
        : config_(config)
        , io_context_(1)
        , ssl_context_(ssl::context::tlsv12_client)
        , resolver_(net::make_strand(io_context_))
        , rate_limit_remaining_(config.requests_per_minute) {

        // Configure SSL
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_none);  // TODO: Enable in production

        // Set base URL based on testnet flag
        if (config_.use_testnet) {
            base_url_ = "testnet.binancefuture.com";
        } else {
            // Parse host from base_url
            std::string url = config_.base_url;
            if (url.find("https://") == 0) {
                url = url.substr(8);
            }
            auto pos = url.find('/');
            if (pos != std::string::npos) {
                url = url.substr(0, pos);
            }
            base_url_ = url;
        }
    }

    HttpResponse request(const HttpRequest& req) {
        try {
            // Create stream
            http_stream stream(io_context_, ssl_context_);

            // Resolve
            auto results = resolver_.resolve(base_url_, "443");

            // Connect
            beast::get_lowest_layer(stream).connect(results);

            // SSL handshake
            if (!SSL_set_tlsext_host_name(stream.native_handle(), base_url_.c_str())) {
                throw std::runtime_error("Failed to set SNI hostname");
            }
            stream.handshake(ssl::stream_base::client);

            // Build request
            http::request<http::string_body> http_req;
            http_req.version(11);

            std::string target = req.path;
            auto params = req.query_params;

            // Add signature if needed
            if (req.sign && !config_.secret_key.empty()) {
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                params["timestamp"] = std::to_string(now);
                params["recvWindow"] = std::to_string(req.recv_window);

                std::string query = build_query_string(params);
                std::string signature = hmac_sha256(config_.secret_key, query);
                params["signature"] = signature;
            }

            // Build query string
            if (!params.empty()) {
                target += "?" + build_query_string(params);
            }

            http_req.target(target);

            switch (req.method) {
                case HttpMethod::GET:    http_req.method(http::verb::get); break;
                case HttpMethod::POST:   http_req.method(http::verb::post); break;
                case HttpMethod::PUT:    http_req.method(http::verb::put); break;
                case HttpMethod::DEL:    http_req.method(http::verb::delete_); break;
            }

            http_req.set(http::field::host, base_url_);
            http_req.set(http::field::user_agent, "OpusTrade/1.0");
            http_req.set(http::field::content_type, "application/json");

            if (!config_.api_key.empty()) {
                http_req.set("X-MBX-APIKEY", config_.api_key);
            }

            for (const auto& [name, value] : req.headers) {
                http_req.set(name, value);
            }

            if (!req.body.empty()) {
                http_req.body() = req.body;
                http_req.prepare_payload();
            }

            // Send request
            http::write(stream, http_req);

            // Read response
            beast::flat_buffer buffer;
            http::response<http::string_body> http_res;
            http::read(stream, buffer, http_res);

            // Parse response
            HttpResponse response;
            response.status_code = http_res.result_int();
            response.body = http_res.body();
            response.received_at = now();

            // Parse headers
            for (const auto& header : http_res) {
                response.headers[std::string(header.name_string())] = std::string(header.value());
            }

            // Parse rate limit headers
            auto used_it = response.headers.find("X-MBX-USED-WEIGHT-1M");
            if (used_it != response.headers.end()) {
                response.rate_limit_used = std::stoi(used_it->second);
                rate_limit_remaining_ = config_.requests_per_minute - response.rate_limit_used;
            }

            // Graceful shutdown
            beast::error_code ec;
            stream.shutdown(ec);

            return response;

        } catch (const std::exception& e) {
            HttpResponse error_response;
            error_response.status_code = -1;
            error_response.body = e.what();
            return error_response;
        }
    }

    void request_async(const HttpRequest& req, IRestClient::ResponseCallback callback) {
        // Post to io_context
        net::post(io_context_, [this, req, callback]() {
            auto response = request(req);
            if (callback) {
                callback(response);
            }
        });
    }

    void run() {
        io_context_.run();
    }

    void stop() {
        io_context_.stop();
    }

    // Members
    RestClientConfig config_;
    net::io_context io_context_;
    ssl::context ssl_context_;
    tcp::resolver resolver_;
    std::string base_url_;
    std::atomic<int> rate_limit_remaining_;
};

// ============================================================================
// RestClient Public Interface
// ============================================================================

RestClient::RestClient(const RestClientConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

RestClient::~RestClient() = default;

void RestClient::request_async(const HttpRequest& request, ResponseCallback callback) {
    impl_->request_async(request, std::move(callback));
}

HttpResponse RestClient::request(const HttpRequest& request) {
    return impl_->request(request);
}

HttpResponse RestClient::get(std::string_view path, const std::map<std::string, std::string>& params) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.path = std::string(path);
    req.query_params = params;
    return impl_->request(req);
}

HttpResponse RestClient::post(std::string_view path,
                               const std::map<std::string, std::string>& params,
                               std::string_view body) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = std::string(path);
    req.query_params = params;
    req.body = std::string(body);
    req.sign = true;  // POST requests typically need signing
    return impl_->request(req);
}

void RestClient::get_async(std::string_view path,
                           const std::map<std::string, std::string>& params,
                           ResponseCallback callback) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.path = std::string(path);
    req.query_params = params;
    impl_->request_async(req, std::move(callback));
}

void RestClient::post_async(std::string_view path,
                            const std::map<std::string, std::string>& params,
                            std::string_view body,
                            ResponseCallback callback) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = std::string(path);
    req.query_params = params;
    req.body = std::string(body);
    req.sign = true;
    impl_->request_async(req, std::move(callback));
}

int RestClient::get_rate_limit_remaining() const {
    return impl_->rate_limit_remaining_.load();
}

void RestClient::run() {
    impl_->run();
}

void RestClient::run_async() {
    // Note: In production, would use a thread pool
    std::thread([this]() { impl_->run(); }).detach();
}

void RestClient::stop() {
    impl_->stop();
}

// ============================================================================
// Rate Limiter Implementation
// ============================================================================

struct RateLimiter::Impl {
    int max_requests_;
    std::chrono::seconds window_;
    std::deque<std::chrono::steady_clock::time_point> requests_;
    std::mutex mutex_;

    Impl(int max_requests, std::chrono::seconds window)
        : max_requests_(max_requests), window_(window) {}

    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_old_requests();

        if (static_cast<int>(requests_.size()) >= max_requests_) {
            return false;
        }

        requests_.push_back(std::chrono::steady_clock::now());
        return true;
    }

    void acquire() {
        while (!try_acquire()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    int remaining() const {
        // Note: This is approximate due to race conditions
        return max_requests_ - static_cast<int>(requests_.size());
    }

    std::chrono::milliseconds time_until_reset() const {
        if (requests_.empty()) {
            return std::chrono::milliseconds(0);
        }

        auto oldest = requests_.front();
        auto reset_time = oldest + window_;
        auto now = std::chrono::steady_clock::now();

        if (reset_time <= now) {
            return std::chrono::milliseconds(0);
        }

        return std::chrono::duration_cast<std::chrono::milliseconds>(reset_time - now);
    }

private:
    void cleanup_old_requests() {
        auto cutoff = std::chrono::steady_clock::now() - window_;
        while (!requests_.empty() && requests_.front() < cutoff) {
            requests_.pop_front();
        }
    }
};

RateLimiter::RateLimiter(int max_requests, std::chrono::seconds window)
    : impl_(std::make_unique<Impl>(max_requests, window)) {}

bool RateLimiter::try_acquire() {
    return impl_->try_acquire();
}

void RateLimiter::acquire() {
    impl_->acquire();
}

int RateLimiter::remaining() const {
    return impl_->remaining();
}

std::chrono::milliseconds RateLimiter::time_until_reset() const {
    return impl_->time_until_reset();
}

}  // namespace opus::network
