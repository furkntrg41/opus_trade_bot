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
            
            // Calculate Realized PnL by fetching recent trades
            try {
                auto trades = client_.get_account_trades(old_pos.symbol, 5);
                double total_pnl = 0.0;
                double total_commission = 0.0;
                
                // Filter trades that likely belong to this closure (last 10 seconds)
                // Note: ideally we would match order IDs, but time-based approximation works for MVP
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                
                for (const auto& trade : trades) {
                    // Check if trade happened in last 30 seconds
                    int64_t trade_time_ms = opus::to_epoch_ms(trade.time);
                    if (now_ms - trade_time_ms < 30000) { 
                        total_pnl += trade.realized_pnl;
                        total_commission += trade.commission; // Commission usually negative in PnL or separate?
                                                              // Binance PnL usually is net? No, userTrades has separate commission.
                                                              // RealizedPnl from Binance is usually Gross PnL.
                    }
                }
                
                // Net PnL = Realized PnL - Commission (approximate if assets differ)
                // Note: Commission is often in BNB or Quote asset. Assuming Quote (USDT) for simplicity or ignoring for MVP.
                // Let's stick to realizedPnl from Binance which is the trading profit.
                last_realized_pnl_ = total_pnl; 
                
                std::cout << "[TRACKER] Calculated PnL: " << last_realized_pnl_ << "\n";
                
            } catch (const std::exception& e) {
                std::cerr << "[TRACKER] Failed to calculate PnL: " << e.what() << "\n";
                last_realized_pnl_ = 0.0;
            }
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
