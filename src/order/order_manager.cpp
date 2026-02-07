// ============================================================================
// OPUS TRADE BOT - Order Manager Implementation
// ============================================================================

#include "opus/order/order_manager.hpp"
#include <iostream>

namespace opus::order {

OrderManager::OrderManager(exchange::binance::IBinanceClient& client)
    : client_(client) {}

std::optional<exchange::binance::OrderInfo> OrderManager::place_market_order(
    const Symbol& symbol,
    Side side,
    Quantity quantity) {
    
    exchange::binance::OrderRequest request;
    request.symbol = symbol;
    request.side = side;
    request.type = OrderType::Market;
    request.quantity = quantity;
    request.client_order_id = generate_client_order_id();

    auto result = client_.place_order(request);
    if (result) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[result->order_id] = *result;
    }
    return result;
}

std::optional<exchange::binance::OrderInfo> OrderManager::place_limit_order(
    const Symbol& symbol,
    Side side,
    Quantity quantity,
    Price price,
    TimeInForce tif) {
    
    exchange::binance::OrderRequest request;
    request.symbol = symbol;
    request.side = side;
    request.type = OrderType::Limit;
    request.quantity = quantity;
    request.price = price;
    request.time_in_force = tif;
    request.client_order_id = generate_client_order_id();

    auto result = client_.place_order(request);
    if (result) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[result->order_id] = *result;
    }
    return result;
}

BracketOrderResult OrderManager::place_bracket_order(
    const Symbol& symbol,
    Side side,
    Quantity quantity,
    Price stop_loss_price,
    Price take_profit_price) {
    
    BracketOrderResult result;
    
    // 1. Place Entry Order (Market)
    std::cout << "[ORDER] Placing Entry Market Order...\n";
    result.entry_order = place_market_order(symbol, side, quantity);
    
    if (!result.entry_order) {
        std::cerr << "[ORDER] Entry failed! Aborting bracket.\n";
        return result;
    }

    std::cout << "[ORDER] Entry Filled: " << result.entry_order->price.to_double() << "\n";
    
    // Determine opposite side for SL/TP
    Side close_side = (side == Side::Buy) ? Side::Sell : Side::Buy;

    // 2. Place Stop Loss (STOP_MARKET)
    exchange::binance::OrderRequest sl_req;
    sl_req.symbol = symbol;
    sl_req.side = close_side;
    sl_req.type = OrderType::StopMarket;
    sl_req.quantity = quantity; // Close full position
    sl_req.stop_price = stop_loss_price;
    sl_req.reduce_only = true;
    sl_req.client_order_id = generate_client_order_id() + "_SL";
    
    std::cout << "[ORDER] Placing Stop Loss at " << stop_loss_price.to_double() << "...\n";
    result.stop_loss_order = client_.place_order(sl_req);
    
    if (result.stop_loss_order) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[result.stop_loss_order->order_id] = *result.stop_loss_order;
    } else {
         std::cerr << "[ORDER] Stop Loss Placement FAILED!\n";
    }

    // 3. Place Take Profit (TAKE_PROFIT_MARKET)
    exchange::binance::OrderRequest tp_req;
    tp_req.symbol = symbol;
    tp_req.side = close_side;
    tp_req.type = OrderType::TakeProfitMarket;
    tp_req.quantity = quantity; // Close full position
    tp_req.stop_price = take_profit_price;
    tp_req.reduce_only = true;
    tp_req.client_order_id = generate_client_order_id() + "_TP";

    std::cout << "[ORDER] Placing Take Profit at " << take_profit_price.to_double() << "...\n";
    result.take_profit_order = client_.place_order(tp_req);

    if (result.take_profit_order) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[result.take_profit_order->order_id] = *result.take_profit_order;
    } else {
        std::cerr << "[ORDER] Take Profit Placement FAILED!\n";
    }

    return result;
}

bool OrderManager::cancel_order(const Symbol& symbol, int64_t order_id) {
    bool success = client_.cancel_order(symbol, order_id);
    if (success) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_.erase(order_id);
    }
    return success;
}

bool OrderManager::cancel_all_orders(const Symbol& symbol) {
    bool success = client_.cancel_all_orders(symbol);
    if (success) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_.clear();
    }
    return success;
}

std::vector<exchange::binance::OrderInfo> OrderManager::get_pending_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<exchange::binance::OrderInfo> orders;
    orders.reserve(pending_orders_.size());
    for (const auto& [id, order] : pending_orders_) {
        orders.push_back(order);
    }
    return orders;
}

void OrderManager::sync_orders(const Symbol& symbol) {
    auto open_orders = client_.get_open_orders(symbol);
    
    std::lock_guard<std::mutex> lock(mutex_);
    pending_orders_.clear();
    for (const auto& order : open_orders) {
        pending_orders_[order.order_id] = order;
    }
}

std::string OrderManager::generate_client_order_id() {
    return "opus_" + std::to_string(++order_counter_);
}

}  // namespace opus::order
