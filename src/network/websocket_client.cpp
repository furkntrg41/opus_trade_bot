// ============================================================================
// OPUS TRADE BOT - WebSocket Client Implementation
// ============================================================================
// Boost.Beast SSL WebSocket client with auto-reconnect
// ============================================================================

#include "opus/network/websocket_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <mutex>
#include <queue>
#include <iostream>

namespace opus::network {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ============================================================================
// WebSocket Client Implementation
// ============================================================================

struct WebSocketClient::Impl {
    using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    explicit Impl(const WebSocketConfig& config)
        : config_(config)
        , io_context_(1)
        , ssl_context_(ssl::context::tlsv12_client)
        , resolver_(net::make_strand(io_context_))
        , reconnect_attempts_(0) {

        // Configure SSL context
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_none);  // TODO: Enable in production
    }

    ~Impl() {
        stop();
    }

    void connect(WebSocketClient* self) {
        std::cerr << "[WS_DEBUG] Starting connect to " << config_.host << ":" << config_.port << config_.path << std::endl;
        std::cerr.flush();
        
        // Create new WebSocket stream
        ws_ = std::make_unique<ws_stream>(net::make_strand(io_context_), ssl_context_);

        // Start async resolve
        resolver_.async_resolve(
            config_.host,
            config_.port,
            [this, self](beast::error_code ec, tcp::resolver::results_type results) {
                on_resolve(ec, results, self);
            });
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results, WebSocketClient* self) {
        if (ec) {
            handle_error("Resolve failed: " + ec.message(), self);
            return;
        }

        std::cerr << "[WS_DEBUG] DNS resolved, connecting..." << std::endl;
        std::cerr.flush();
        
        // Set timeout for connect
        beast::get_lowest_layer(*ws_).expires_after(config_.connect_timeout);

        // Async connect
        beast::get_lowest_layer(*ws_).async_connect(
            results,
            [this, self](beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
                on_connect(ec, ep, self);
            });
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type, WebSocketClient* self) {
        if (ec) {
            handle_error("Connect failed: " + ec.message(), self);
            maybe_reconnect(self);
            return;
        }
        
        std::cerr << "[WS_DEBUG] TCP connected, starting SSL handshake..." << std::endl;

        // Set SNI hostname for SSL
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), config_.host.c_str())) {
            ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
            handle_error("SSL hostname failed: " + ec.message(), self);
            return;
        }

        // SSL handshake
        beast::get_lowest_layer(*ws_).expires_after(config_.connect_timeout);
        ws_->next_layer().async_handshake(
            ssl::stream_base::client,
            [this, self](beast::error_code ec) {
                on_ssl_handshake(ec, self);
            });
    }

    void on_ssl_handshake(beast::error_code ec, WebSocketClient* self) {
        if (ec) {
            handle_error("SSL handshake failed: " + ec.message(), self);
            maybe_reconnect(self);
            return;
        }
        
        std::cerr << "[WS_DEBUG] SSL handshake done, starting WS handshake..." << std::endl;

        // Set timeout FOR WebSocket handshake (not expires_never!)
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(10));

        // Set WebSocket options
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "OpusTrade/1.0");
            }));

        // WebSocket handshake
        ws_->async_handshake(
            config_.host,
            config_.path,
            [this, self](beast::error_code ec) {
                on_ws_handshake(ec, self);
            });
    }

    void on_ws_handshake(beast::error_code ec, WebSocketClient* self) {
        std::cerr << "[WS_DEBUG] WS handshake callback, ec=" << ec.message() << std::endl;
        
        if (ec) {
            handle_error("WebSocket handshake failed: " + ec.message(), self);
            maybe_reconnect(self);
            return;
        }
        
        std::cerr << "[WS_DEBUG] WebSocket CONNECTED!" << std::endl;

        // Connected!
        self->connected_ = true;
        reconnect_attempts_ = 0;

        if (self->on_connect_) {
            self->on_connect_();
        }

        // Disable timeout for persistent reads - WebSocket handles its own ping/pong
        beast::get_lowest_layer(*ws_).expires_never();
        
        // Start reading
        do_read(self);

        // Start ping timer if enabled
        if (config_.enable_ping) {
            start_ping_timer(self);
        }
    }

    void do_read(WebSocketClient* self) {
        ws_->async_read(
            read_buffer_,
            [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                on_read(ec, bytes_transferred, self);
            });
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred, WebSocketClient* self) {
        if (ec) {
            if (ec == websocket::error::closed) {
                // Normal close
                self->connected_ = false;
                if (self->on_disconnect_) {
                    self->on_disconnect_();
                }
                maybe_reconnect(self);
                return;
            }

            handle_error("Read error: " + ec.message(), self);
            self->connected_ = false;
            maybe_reconnect(self);
            return;
        }

        // Process message
        if (self->on_message_) {
            std::string message = beast::buffers_to_string(read_buffer_.data());
            self->on_message_(message);
        }

        read_buffer_.consume(bytes_transferred);

        // Continue reading
        do_read(self);
    }

    void send(std::string_view message, WebSocketClient* self) {
        bool write_in_progress = false;

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_in_progress = !write_queue_.empty();
            write_queue_.push(std::string(message));
        }

        if (!write_in_progress) {
            do_write(self);
        }
    }

    void do_write(WebSocketClient* self) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (write_queue_.empty()) return;
            message = std::move(write_queue_.front());
        }

        ws_->async_write(
            net::buffer(message),
            [this, self](beast::error_code ec, std::size_t) {
                on_write(ec, self);
            });
    }

    void on_write(beast::error_code ec, WebSocketClient* self) {
        if (ec) {
            handle_error("Write error: " + ec.message(), self);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_queue_.pop();
        }

        do_write(self);
    }

    void start_ping_timer(WebSocketClient* self) {
        ping_timer_ = std::make_unique<net::steady_timer>(io_context_);
        ping_timer_->expires_after(config_.ping_interval);
        ping_timer_->async_wait([this, self](beast::error_code ec) {
            if (!ec && self->connected_) {
                ws_->async_ping({}, [this, self](beast::error_code) {
                    start_ping_timer(self);
                });
            }
        });
    }

    void disconnect() {
        if (ws_) {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
        }
    }

    void maybe_reconnect(WebSocketClient* self) {
        if (!config_.auto_reconnect || !self->running_) return;
        if (reconnect_attempts_ >= config_.max_reconnect_attempts) {
            handle_error("Max reconnect attempts reached", self);
            return;
        }

        ++reconnect_attempts_;

        reconnect_timer_ = std::make_unique<net::steady_timer>(io_context_);
        reconnect_timer_->expires_after(config_.reconnect_delay);
        reconnect_timer_->async_wait([this, self](beast::error_code ec) {
            if (!ec && self->running_) {
                connect(self);
            }
        });
    }

    void handle_error(const std::string& error, WebSocketClient* self) {
        std::cerr << "[WS_ERROR] " << error << std::endl;
        if (self->on_error_) {
            self->on_error_(error);
        }
    }

    void run() {
        io_context_.run();
    }

    void stop() {
        io_context_.stop();
    }

    // Members
    WebSocketConfig config_;
    net::io_context io_context_;
    ssl::context ssl_context_;
    tcp::resolver resolver_;

    std::unique_ptr<ws_stream> ws_;
    beast::flat_buffer read_buffer_;

    std::mutex write_mutex_;
    std::queue<std::string> write_queue_;

    std::unique_ptr<net::steady_timer> ping_timer_;
    std::unique_ptr<net::steady_timer> reconnect_timer_;
    size_t reconnect_attempts_;
};

// ============================================================================
// WebSocketClient Public Interface
// ============================================================================

WebSocketClient::WebSocketClient(const WebSocketConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

WebSocketClient::~WebSocketClient() {
    stop();
}

void WebSocketClient::connect() {
    impl_->connect(this);
}

void WebSocketClient::disconnect() {
    connected_ = false;
    impl_->disconnect();
}

bool WebSocketClient::is_connected() const {
    return connected_;
}

void WebSocketClient::send(std::string_view message) {
    if (connected_) {
        impl_->send(message, this);
    }
}

void WebSocketClient::run() {
    running_ = true;
    impl_->run();
}

void WebSocketClient::run_async() {
    running_ = true;
    io_thread_ = std::thread([this]() {
        impl_->run();
    });
}

void WebSocketClient::stop() {
    running_ = false;
    impl_->stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

// ============================================================================
// WebSocketPool (Placeholder)
// ============================================================================

struct WebSocketPool::Impl {
    net::io_context io_context_;
    std::vector<std::shared_ptr<WebSocketClient>> connections_;
    std::vector<std::thread> threads_;
    size_t thread_count_;

    explicit Impl(size_t thread_count) : thread_count_(thread_count) {}
};

WebSocketPool::WebSocketPool(size_t thread_count)
    : impl_(std::make_unique<Impl>(thread_count)) {}

WebSocketPool::~WebSocketPool() {
    stop();
}

std::shared_ptr<IWebSocketClient> WebSocketPool::create_connection(const WebSocketConfig& config) {
    auto client = std::make_shared<WebSocketClient>(config);
    impl_->connections_.push_back(client);
    return client;
}

void WebSocketPool::remove_connection(const std::shared_ptr<IWebSocketClient>& client) {
    auto& conns = impl_->connections_;
    auto it = std::find_if(conns.begin(), conns.end(),
        [&client](const std::shared_ptr<WebSocketClient>& c) {
            return c.get() == client.get();
        });
    if (it != conns.end()) {
        (*it)->stop();
        conns.erase(it);
    }
}

void WebSocketPool::start() {
    for (auto& conn : impl_->connections_) {
        conn->connect();
        conn->run_async();
    }
}

void WebSocketPool::stop() {
    for (auto& conn : impl_->connections_) {
        conn->stop();
    }
    impl_->connections_.clear();
}

}  // namespace opus::network
