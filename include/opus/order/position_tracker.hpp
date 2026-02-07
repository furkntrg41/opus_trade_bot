#pragma once
// ============================================================================
// OPUS TRADE BOT - Position Tracker
// ============================================================================
// Tracks open positions in real-time to prevent RiskManager deadlock
// Supports Hedge Mode and Multi-Asset Mode
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/exchange/binance/client.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace opus::order {

class PositionTracker {
public:
    struct Position {
        Symbol symbol;
        double quantity = 0.0;      // Signed: +Long, -Short
        double entry_price = 0.0;
        double unrealized_pnl = 0.0;
    };

    explicit PositionTracker(exchange::binance::IBinanceClient& client);

    /// Sync positions from exchange (Smart Polling)
    /// Returns true if a position was JUST closed
    bool sync_with_exchange();

    /// Check if ANY position is open
    bool has_open_position() const;

    /// Get current position details (Thread-safe)
    std::optional<Position> get_position(const Symbol& symbol) const;
    
    /// Get the last realized PnL (approximate)
    double get_last_realized_pnl() const;

private:
    exchange::binance::IBinanceClient& client_;
    mutable std::mutex mutex_;
    
    // Tracked positions by symbol
    std::vector<Position> open_positions_; 
    
    // State
    double last_realized_pnl_ = 0.0;
};

}  // namespace opus::order
