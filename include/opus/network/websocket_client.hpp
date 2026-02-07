#pragma once
// ============================================================================
// OPUS TRADE BOT - WebSocket Client
// ============================================================================
// High-performance async WebSocket client using Boost.Beast
// Features: Auto-reconnect, heartbeat, message queuing
// ============================================================================

#include "opus/core/ring_buffer.hpp"
#include "opus/core/types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

// Forward declarations for Boost (avoid header pollution)
namespace boost::asio {
class io_context;
namespace ssl {
class context;
}
}  // namespace boost::asio

namespace opus::network {

// ============================================================================
// WebSocket Message Types
// ============================================================================

struct WebSocketMessage {
    std::string data;
    Timestamp received_at;
    bool is_binary;
};

// ============================================================================
// WebSocket Client Configuration
// ============================================================================

struct WebSocketConfig {
    std::string host;
    std::string port = "443";
    std::string path = "/";

    // Connection settings
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};

    // Reconnection settings
    bool auto_reconnect = true;
    std::chrono::seconds reconnect_delay{5};
    size_t max_reconnect_attempts = 10;

    // Heartbeat/ping
    bool enable_ping = true;
    std::chrono::seconds ping_interval{30};

    // Buffer sizes
    size_t read_buffer_size = 65536;   // 64KB
    size_t write_buffer_size = 16384;  // 16KB
};

// ============================================================================
// WebSocket Client Interface
// ============================================================================

class IWebSocketClient {
public:
    using MessageCallback = std::function<void(std::string_view)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ConnectCallback = std::function<void()>;
    using DisconnectCallback = std::function<void()>;

    virtual ~IWebSocketClient() = default;

    /// Connect to WebSocket server
    virtual void connect() = 0;

    /// Disconnect from server
    virtual void disconnect() = 0;

    /// Check if connected
    [[nodiscard]] virtual bool is_connected() const = 0;

    /// Send a message
    virtual void send(std::string_view message) = 0;

    /// Set message callback
    virtual void on_message(MessageCallback callback) = 0;

    /// Set error callback
    virtual void on_error(ErrorCallback callback) = 0;

    /// Set connect callback
    virtual void on_connect(ConnectCallback callback) = 0;

    /// Set disconnect callback
    virtual void on_disconnect(DisconnectCallback callback) = 0;
};

// ============================================================================
// WebSocket Client Implementation (Header-only for templates)
// ============================================================================

class WebSocketClient : public IWebSocketClient {
public:
    explicit WebSocketClient(const WebSocketConfig& config);
    ~WebSocketClient() override;

    // Non-copyable, non-movable (due to std::atomic members)
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) = delete;
    WebSocketClient& operator=(WebSocketClient&&) = delete;

    void connect() override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const override;
    void send(std::string_view message) override;

    void on_message(MessageCallback callback) override { on_message_ = std::move(callback); }
    void on_error(ErrorCallback callback) override { on_error_ = std::move(callback); }
    void on_connect(ConnectCallback callback) override { on_connect_ = std::move(callback); }
    void on_disconnect(DisconnectCallback callback) override { on_disconnect_ = std::move(callback); }

    /// Start the I/O context (blocking)
    void run();

    /// Start in background thread
    void run_async();

    /// Stop the I/O context
    void stop();

private:
    // Implementation details (PIMPL idiom)
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Callbacks
    MessageCallback on_message_;
    ErrorCallback on_error_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread io_thread_;
};

// ============================================================================
// WebSocket Connection Pool (for multiple streams)
// ============================================================================

class WebSocketPool {
public:
    explicit WebSocketPool(size_t thread_count = 1);
    ~WebSocketPool();

    /// Add a new connection
    std::shared_ptr<IWebSocketClient> create_connection(const WebSocketConfig& config);

    /// Remove a connection
    void remove_connection(const std::shared_ptr<IWebSocketClient>& client);

    /// Start all connections
    void start();

    /// Stop all connections
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opus::network
