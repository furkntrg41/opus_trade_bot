// ============================================================================
// OPUS TRADE BOT - Position Tracker
// ============================================================================
// Tracks open positions and calculates PnL
// ============================================================================

#include "opus/order/risk_manager.hpp"
#include "opus/exchange/binance/client.hpp"
#include "opus/core/types.hpp"

#include <mutex>
#include <unordered_map>

namespace opus::order {

// ============================================================================
// Position Tracker
// ============================================================================

class PositionTracker {
public:
    struct Position {
        Symbol symbol;
        PositionSide side;
        Quantity quantity;
        Price entry_price;
        Price current_price;
        double unrealized_pnl;
        double realized_pnl;
        Timestamp open_time;
    };

    explicit PositionTracker(exchange::binance::IBinanceClient& client)
        : client_(client) {}

    /// Sync positions from exchange
    void sync_positions() {
        auto exchange_positions = client_.get_positions();

        std::lock_guard<std::mutex> lock(mutex_);
        positions_.clear();

        for (const auto& pos : exchange_positions) {
            Position tracked_pos;
            tracked_pos.symbol = pos.symbol;
            tracked_pos.side = pos.side;
            tracked_pos.quantity = pos.quantity;
            tracked_pos.entry_price = pos.entry_price;
            tracked_pos.unrealized_pnl = pos.unrealized_profit;
            tracked_pos.realized_pnl = 0.0;

            positions_[std::string(pos.symbol.view())] = tracked_pos;
        }
    }

    /// Update price for PnL calculation
    void update_price(const Symbol& symbol, Price current_price) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = positions_.find(std::string(symbol.view()));
        if (it == positions_.end()) return;

        it->second.current_price = current_price;

        // Calculate unrealized PnL
        double entry = it->second.entry_price.to_double();
        double current = current_price.to_double();
        double qty = it->second.quantity.to_double();

        if (it->second.side == PositionSide::Long) {
            it->second.unrealized_pnl = (current - entry) * qty;
        } else {
            it->second.unrealized_pnl = (entry - current) * qty;
        }
    }

    /// Get position for a symbol
    [[nodiscard]] std::optional<Position> get_position(const Symbol& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = positions_.find(std::string(symbol.view()));
        if (it == positions_.end()) return std::nullopt;

        return it->second;
    }

    /// Get all positions
    [[nodiscard]] std::vector<Position> get_all_positions() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<Position> result;
        result.reserve(positions_.size());
        for (const auto& [_, pos] : positions_) {
            result.push_back(pos);
        }
        return result;
    }

    /// Calculate total unrealized PnL
    [[nodiscard]] double total_unrealized_pnl() const {
        std::lock_guard<std::mutex> lock(mutex_);

        double total = 0.0;
        for (const auto& [_, pos] : positions_) {
            total += pos.unrealized_pnl;
        }
        return total;
    }

    /// Calculate total exposure
    [[nodiscard]] double total_exposure() const {
        std::lock_guard<std::mutex> lock(mutex_);

        double total = 0.0;
        for (const auto& [_, pos] : positions_) {
            total += pos.quantity.to_double() * pos.current_price.to_double();
        }
        return total;
    }

    /// Check if we have a position in the given symbol
    [[nodiscard]] bool has_position(const Symbol& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return positions_.find(std::string(symbol.view())) != positions_.end();
    }

    /// Get position count
    [[nodiscard]] size_t position_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return positions_.size();
    }

private:
    exchange::binance::IBinanceClient& client_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Position> positions_;
};

}  // namespace opus::order
