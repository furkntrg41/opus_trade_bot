#pragma once
// ============================================================================
// OPUS TRADE BOT - REST Client
// ============================================================================
// Async HTTP client for Binance REST API
// Features: Connection pooling, rate limiting, request signing
// ============================================================================

#include "opus/core/types.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace opus::network {

// ============================================================================
// HTTP Types
// ============================================================================

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DEL  // Note: Using DEL instead of DELETE to avoid Windows macro conflict
};

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
    std::string path;
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> headers;
    std::string body;

    // For signed requests
    bool sign = false;
    int64_t recv_window = 5000;  // milliseconds
};

struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    Timestamp received_at;

    // Rate limit info from headers
    int rate_limit_used = 0;
    int rate_limit_limit = 0;

    [[nodiscard]] bool is_success() const { return status_code >= 200 && status_code < 300; }
    [[nodiscard]] bool is_rate_limited() const { return status_code == 429; }
};

// ============================================================================
// REST Client Configuration
// ============================================================================

struct RestClientConfig {
    std::string base_url = "https://fapi.binance.com";
    std::string api_key;
    std::string secret_key;

    // Connection settings
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds request_timeout{30};

    // Connection pooling
    size_t max_connections = 10;
    bool keep_alive = true;

    // Rate limiting
    int requests_per_minute = 1200;  // Binance limit
    bool auto_retry_rate_limit = true;

    // Testnet
    bool use_testnet = false;
};

// ============================================================================
// REST Client Interface
// ============================================================================

class IRestClient {
public:
    using ResponseCallback = std::function<void(const HttpResponse&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~IRestClient() = default;

    /// Execute request asynchronously
    virtual void request_async(const HttpRequest& request, ResponseCallback callback) = 0;

    /// Execute request synchronously (blocking)
    [[nodiscard]] virtual HttpResponse request(const HttpRequest& request) = 0;

    /// Convenience methods
    [[nodiscard]] virtual HttpResponse get(std::string_view path,
                                            const std::map<std::string, std::string>& params = {}) = 0;

    [[nodiscard]] virtual HttpResponse post(std::string_view path,
                                             const std::map<std::string, std::string>& params = {},
                                             std::string_view body = "") = 0;

    virtual void get_async(std::string_view path,
                           const std::map<std::string, std::string>& params,
                           ResponseCallback callback) = 0;

    virtual void post_async(std::string_view path,
                            const std::map<std::string, std::string>& params,
                            std::string_view body,
                            ResponseCallback callback) = 0;

    /// Set error callback
    virtual void on_error(ErrorCallback callback) = 0;

    /// Get rate limit status
    [[nodiscard]] virtual int get_rate_limit_remaining() const = 0;
};

// ============================================================================
// REST Client Implementation
// ============================================================================

class RestClient : public IRestClient {
public:
    explicit RestClient(const RestClientConfig& config);
    ~RestClient() override;

    // Non-copyable
    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    void request_async(const HttpRequest& request, ResponseCallback callback) override;
    [[nodiscard]] HttpResponse request(const HttpRequest& request) override;

    [[nodiscard]] HttpResponse get(std::string_view path,
                                    const std::map<std::string, std::string>& params = {}) override;

    [[nodiscard]] HttpResponse post(std::string_view path,
                                     const std::map<std::string, std::string>& params = {},
                                     std::string_view body = "") override;

    void get_async(std::string_view path,
                   const std::map<std::string, std::string>& params,
                   ResponseCallback callback) override;

    void post_async(std::string_view path,
                    const std::map<std::string, std::string>& params,
                    std::string_view body,
                    ResponseCallback callback) override;

    void on_error(ErrorCallback callback) override { on_error_ = std::move(callback); }

    [[nodiscard]] int get_rate_limit_remaining() const override;

    /// Start the async I/O
    void run();
    void run_async();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    ErrorCallback on_error_;
};

// ============================================================================
// Rate Limiter
// ============================================================================

class RateLimiter {
public:
    RateLimiter(int max_requests, std::chrono::seconds window);

    /// Try to acquire a permit. Returns true if allowed.
    [[nodiscard]] bool try_acquire();

    /// Wait until a permit is available, then acquire.
    void acquire();

    /// Get remaining permits in current window
    [[nodiscard]] int remaining() const;

    /// Get time until next window reset
    [[nodiscard]] std::chrono::milliseconds time_until_reset() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opus::network
