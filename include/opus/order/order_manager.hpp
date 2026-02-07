#pragma once
// ============================================================================
// OPUS TRADE BOT - Order Manager
// ============================================================================
// Manages order lifecycle: creation, modification, cancellation
// Supports Bracket Orders (Market Entry + SL + TP)
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/exchange/binance/client.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opus::order {

struct BracketOrderResult {
    std::optional<exchange::binance::OrderInfo> entry_order;
    std::optional<exchange::binance::OrderInfo> stop_loss_order;
    std::optional<exchange::binance::OrderInfo> take_profit_order;
};

class OrderManager {
public:
    using OrderCallback = std::function<void(const exchange::binance::OrderInfo&)>;

    explicit OrderManager(exchange::binance::IBinanceClient& client);

    /// Place a market order
    std::optional<exchange::binance::OrderInfo> place_market_order(
        const Symbol& symbol,
        Side side,
        Quantity quantity);

    /// Place a limit order
    std::optional<exchange::binance::OrderInfo> place_limit_order(
        const Symbol& symbol,
        Side side,
        Quantity quantity,
        Price price,
        TimeInForce tif = TimeInForce::GTC);

    /// Place a bracket order (Entry + SL + TP)
    BracketOrderResult place_bracket_order(
        const Symbol& symbol,
        Side side,
        Quantity quantity,
        Price stop_loss_price,
        Price take_profit_price);

    /// Cancel an order
    bool cancel_order(const Symbol& symbol, int64_t order_id);

    /// Cancel all orders for a symbol
    bool cancel_all_orders(const Symbol& symbol);

    /// Get all pending orders
    [[nodiscard]] std::vector<exchange::binance::OrderInfo> get_pending_orders() const;

    /// Sync pending orders with exchange
    void sync_orders(const Symbol& symbol);

private:
    std::string generate_client_order_id();

    exchange::binance::IBinanceClient& client_;
    mutable std::mutex mutex_;
    std::unordered_map<int64_t, exchange::binance::OrderInfo> pending_orders_;
    std::atomic<uint64_t> order_counter_{0};
};

}  // namespace opus::order
