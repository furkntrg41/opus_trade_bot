#pragma once
// ============================================================================
// OPUS TRADE BOT - Binance Futures Client
// ============================================================================
// High-level interface to Binance Futures API
// Supports both Testnet and Mainnet environments
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/network/rest_client.hpp"
#include "opus/network/websocket_client.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opus::exchange::binance {

// ============================================================================
// Binance-specific Types
// ============================================================================

struct AccountInfo {
    double total_wallet_balance;
    double available_balance;
    double total_unrealized_profit;
    double total_margin_balance;
    std::vector<struct PositionInfo> positions;
};

struct PositionInfo {
    Symbol symbol;
    PositionSide side;
    Quantity quantity;
    Price entry_price;
    double unrealized_profit;
    double leverage;
    Price liquidation_price;
};

struct OrderInfo {
    int64_t order_id;
    std::string client_order_id;
    Symbol symbol;
    Side side;
    PositionSide position_side;
    OrderType type;
    OrderStatus status;
    Price price;
    Quantity quantity;
    Quantity executed_qty;
    Timestamp create_time;
    Timestamp update_time;
};

struct Balance {
    std::string asset;
    double wallet_balance;
    double available_balance;
    double unrealized_profit;
};

// ============================================================================
// Order Request
// ============================================================================

struct OrderRequest {
    Symbol symbol;
    Side side;
    PositionSide position_side = PositionSide::Both;
    OrderType type = OrderType::Market;
    TimeInForce time_in_force = TimeInForce::GTC;
    Quantity quantity;
    Price price;               // For limit orders
    Price stop_price;          // For stop orders
    std::string client_order_id;
    bool reduce_only = false;
    bool close_position = false;
};

// ============================================================================
// Market Data Callbacks
// ============================================================================

struct DepthUpdate {
    Symbol symbol;
    int64_t last_update_id;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    Timestamp event_time;
};

struct TradeUpdate {
    Symbol symbol;
    int64_t trade_id;
    Price price;
    Quantity quantity;
    Side side;  // Market taker side
    Timestamp trade_time;
};

struct KlineUpdate {
    Symbol symbol;
    Kline kline;
    bool is_final;  // True when candle is closed
};

// ============================================================================
// Binance Client Configuration
// ============================================================================

struct BinanceConfig {
    std::string api_key;
    std::string secret_key;

    // Environment
    bool testnet = true;  // Start with testnet for safety

    // Testnet URLs
    std::string testnet_rest_url = "https://testnet.binancefuture.com";
    std::string testnet_ws_url = "wss://stream.binancefuture.com";

    // Mainnet URLs
    std::string mainnet_rest_url = "https://fapi.binance.com";
    std::string mainnet_ws_url = "wss://fstream.binance.com";

    // Trading settings
    int default_leverage = 10;
    bool hedge_mode = false;  // false = one-way mode
};

// ============================================================================
// Binance Client Interface
// ============================================================================

class IBinanceClient {
public:
    using DepthCallback = std::function<void(const DepthUpdate&)>;
    using TradeCallback = std::function<void(const TradeUpdate&)>;
    using KlineCallback = std::function<void(const KlineUpdate&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~IBinanceClient() = default;

    // ========================================================================
    // Account & Trading
    // ========================================================================

    /// Get account information
    [[nodiscard]] virtual std::optional<AccountInfo> get_account_info() = 0;

    /// Get all open positions
    [[nodiscard]] virtual std::vector<PositionInfo> get_positions() = 0;

    /// Get position for specific symbol
    [[nodiscard]] virtual std::optional<PositionInfo> get_position(const Symbol& symbol) = 0;

    /// Get all open orders
    [[nodiscard]] virtual std::vector<OrderInfo> get_open_orders(const Symbol& symbol = {}) = 0;

    /// Place a new order
    [[nodiscard]] virtual std::optional<OrderInfo> place_order(const OrderRequest& request) = 0;

    /// Cancel an order
    virtual bool cancel_order(const Symbol& symbol, int64_t order_id) = 0;

    /// Cancel all orders for a symbol
    virtual bool cancel_all_orders(const Symbol& symbol) = 0;

    /// Set leverage for a symbol
    virtual bool set_leverage(const Symbol& symbol, int leverage) = 0;

    // ========================================================================
    // Market Data (REST)
    // ========================================================================

    /// Get current price
    [[nodiscard]] virtual std::optional<Price> get_price(const Symbol& symbol) = 0;

    /// Get order book snapshot
    [[nodiscard]] virtual std::optional<DepthUpdate> get_depth(const Symbol& symbol,
                                                                int limit = 500) = 0;

    /// Get historical klines
    [[nodiscard]] virtual std::vector<Kline> get_klines(const Symbol& symbol,
                                                         std::string_view interval,
                                                         int limit = 500) = 0;

    // ========================================================================
    // WebSocket Streams
    // ========================================================================

    /// Subscribe to depth updates
    virtual void subscribe_depth(const Symbol& symbol, DepthCallback callback) = 0;

    /// Subscribe to trade updates
    virtual void subscribe_trades(const Symbol& symbol, TradeCallback callback) = 0;

    /// Subscribe to kline updates
    virtual void subscribe_klines(const Symbol& symbol,
                                   std::string_view interval,
                                   KlineCallback callback) = 0;

    /// Unsubscribe from all streams for a symbol
    virtual void unsubscribe(const Symbol& symbol) = 0;

    /// Set error callback
    virtual void on_error(ErrorCallback callback) = 0;

    // ========================================================================
    // Connection Management
    // ========================================================================

    /// Start the client (connect WebSocket streams)
    virtual void start() = 0;

    /// Stop the client
    virtual void stop() = 0;

    /// Check if connected
    [[nodiscard]] virtual bool is_connected() const = 0;
};

// ============================================================================
// Binance Client Implementation
// ============================================================================

class BinanceClient : public IBinanceClient {
public:
    explicit BinanceClient(const BinanceConfig& config);
    ~BinanceClient() override;

    // Non-copyable
    BinanceClient(const BinanceClient&) = delete;
    BinanceClient& operator=(const BinanceClient&) = delete;

    // Account & Trading
    [[nodiscard]] std::optional<AccountInfo> get_account_info() override;
    [[nodiscard]] std::vector<PositionInfo> get_positions() override;
    [[nodiscard]] std::optional<PositionInfo> get_position(const Symbol& symbol) override;
    [[nodiscard]] std::vector<OrderInfo> get_open_orders(const Symbol& symbol = {}) override;
    [[nodiscard]] std::optional<OrderInfo> place_order(const OrderRequest& request) override;
    bool cancel_order(const Symbol& symbol, int64_t order_id) override;
    bool cancel_all_orders(const Symbol& symbol) override;
    bool set_leverage(const Symbol& symbol, int leverage) override;

    // Market Data (REST)
    [[nodiscard]] std::optional<Price> get_price(const Symbol& symbol) override;
    [[nodiscard]] std::optional<DepthUpdate> get_depth(const Symbol& symbol, int limit = 500) override;
    [[nodiscard]] std::vector<Kline> get_klines(const Symbol& symbol,
                                                 std::string_view interval,
                                                 int limit = 500) override;

    // WebSocket Streams
    void subscribe_depth(const Symbol& symbol, DepthCallback callback) override;
    void subscribe_trades(const Symbol& symbol, TradeCallback callback) override;
    void subscribe_klines(const Symbol& symbol,
                          std::string_view interval,
                          KlineCallback callback) override;
    void unsubscribe(const Symbol& symbol) override;
    void on_error(ErrorCallback callback) override { on_error_ = std::move(callback); }

    // Connection Management
    void start() override;
    void stop() override;
    [[nodiscard]] bool is_connected() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    ErrorCallback on_error_;
};

// ============================================================================
// HMAC-SHA256 Signing Utility
// ============================================================================

/// Sign a message using HMAC-SHA256
[[nodiscard]] std::string hmac_sha256(std::string_view key, std::string_view message);

/// Generate signature for Binance API request
[[nodiscard]] std::string generate_signature(std::string_view secret_key,
                                              std::string_view query_string);

}  // namespace opus::exchange::binance
