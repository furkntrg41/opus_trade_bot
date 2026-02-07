// ============================================================================
// OPUS TRADE BOT - Position Tracker Implementation
// ============================================================================

#include "opus/order/position_tracker.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace opus::order {

PositionTracker::PositionTracker(exchange::binance::IBinanceClient& client)
    : client_(client) {}

bool PositionTracker::sync_with_exchange() {
    // 1. Fetch current open positions from API
    // client_.get_positions() calls /fapi/v2/positionRisk
    // and ALREADY filters out zero/dust positions (< 1e-7)
    auto api_positions = client_.get_positions();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 2. Identify closed positions
    // A position is closed if it WAS in open_positions_ but is NOT in api_positions
    bool any_closed = false;
    
    for (const auto& old_pos : open_positions_) {
        bool found = false;
        for (const auto& new_pos : api_positions) {
            if (old_pos.symbol == new_pos.symbol) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            std::cout << "[TRACKER] Detected closure for " << old_pos.symbol.view() << "\n";
            any_closed = true;
            // In a real system, we would calculate realized PnL here
            // using the last known unrealized PnL or trade history
        }
    }
    
    // 3. Update internal state
    open_positions_.clear();
    for (const auto& api_pos : api_positions) {
        Position pos;
        pos.symbol = api_pos.symbol;
        
        // Reconstruct signed quantity
        double qty_abs = api_pos.quantity.to_double();
        if (api_pos.side == PositionSide::Short) {
            pos.quantity = -qty_abs;
        } else {
            pos.quantity = qty_abs;
        }
        
        pos.entry_price = api_pos.entry_price.to_double();
        pos.unrealized_pnl = api_pos.unrealized_profit;
        
        open_positions_.push_back(pos);
    }
    
    return any_closed;
}

bool PositionTracker::has_open_position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !open_positions_.empty();
}

std::optional<PositionTracker::Position> PositionTracker::get_position(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pos : open_positions_) {
        // Simple string comparison for symbols
        if (pos.symbol == symbol) {
            return pos;
        }
    }
    return std::nullopt;
}

double PositionTracker::get_last_realized_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_realized_pnl_;
}

}  // namespace opus::order
