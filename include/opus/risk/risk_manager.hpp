#pragma once

// ============================================================================
// OPUS TRADE BOT - Risk Manager
// ============================================================================
// Hardcoded risk limits that CANNOT be disabled
// Prevents catastrophic losses from bugs or market conditions
// ============================================================================

#include <chrono>
#include <cmath>
#include <algorithm>
#include <string>

namespace opus::risk {

// ============================================================================
// Risk Configuration
// ============================================================================

struct RiskConfig {
    // Position Limits
    double max_position_usd = 100.0;    // Max $100 per trade
    int max_open_positions = 1;          // Only 1 position at a time
    
    // Rate Limits
    int max_orders_per_minute = 2;
    int min_order_interval_ms = 30000;   // 30 second cooldown
    
    // Loss Limits (FEE-AWARE: 0.10% round-trip fees)
    double stop_loss_pct = 0.25;         // 0.25% SL (covers fees + buffer)
    double take_profit_pct = 0.50;       // 0.50% TP (2:1 R:R)
    double max_daily_loss_usd = 50.0;    // Hard stop at $50 daily loss
    
    // Commission Rates (Binance Futures VIP0)
    double maker_fee_pct = 0.02;         // 0.02%
    double taker_fee_pct = 0.05;         // 0.05%
};

// ============================================================================
// HARDCODED MINIMUMS - Cannot be overridden
// ============================================================================

namespace limits {
    constexpr double MIN_STOP_LOSS_PCT = 0.20;       // Never less than 0.20%
    constexpr double MAX_POSITION_USD = 500.0;       // Never more than $500
    constexpr double MIN_ORDER_INTERVAL_MS = 10000;  // Never less than 10s
    constexpr int MAX_DAILY_TRADES = 20;             // Never more than 20/day
}

// ============================================================================
// Risk Manager Class
// ============================================================================

class RiskManager {
public:
    enum class TradeDecision {
        Approved,
        RejectedPositionLimit,
        RejectedRateLimit,
        RejectedDailyLoss,
        RejectedCooldown,
        RejectedMaxTrades
    };
    
    struct TradeResult {
        TradeDecision decision;
        double position_size_usd = 0.0;
        double stop_loss_price = 0.0;
        double take_profit_price = 0.0;
        std::string reason;
    };

    explicit RiskManager(const RiskConfig& config = RiskConfig{})
        : config_(apply_hardcoded_limits(config)) {
        reset_daily_stats();
    }
    
    // ========================================================================
    // Pre-Trade Check
    // ========================================================================
    
    TradeResult can_trade(double entry_price, bool is_long) {
        TradeResult result;
        
        // Check 1: Daily loss limit
        if (daily_pnl_ <= -config_.max_daily_loss_usd) {
            result.decision = TradeDecision::RejectedDailyLoss;
            result.reason = "Daily loss limit reached: $" + std::to_string(-daily_pnl_);
            return result;
        }
        
        // Check 2: Max daily trades
        if (daily_trades_ >= limits::MAX_DAILY_TRADES) {
            result.decision = TradeDecision::RejectedMaxTrades;
            result.reason = "Max daily trades reached: " + std::to_string(daily_trades_);
            return result;
        }
        
        // Check 3: Open position limit
        if (open_positions_ >= config_.max_open_positions) {
            result.decision = TradeDecision::RejectedPositionLimit;
            result.reason = "Max open positions: " + std::to_string(open_positions_);
            return result;
        }
        
        // Check 4: Cooldown
        auto now = std::chrono::steady_clock::now();
        auto cooldown = std::chrono::milliseconds(config_.min_order_interval_ms);
        if (now - last_order_time_ < cooldown) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                cooldown - (now - last_order_time_)).count();
            result.decision = TradeDecision::RejectedCooldown;
            result.reason = "Cooldown active: " + std::to_string(remaining) + "s remaining";
            return result;
        }
        
        // All checks passed - calculate trade parameters
        result.decision = TradeDecision::Approved;
        result.position_size_usd = config_.max_position_usd;
        
        // Calculate SL/TP prices
        double sl_offset = entry_price * (config_.stop_loss_pct / 100.0);
        double tp_offset = entry_price * (config_.take_profit_pct / 100.0);
        
        if (is_long) {
            result.stop_loss_price = entry_price - sl_offset;
            result.take_profit_price = entry_price + tp_offset;
        } else {
            result.stop_loss_price = entry_price + sl_offset;
            result.take_profit_price = entry_price - tp_offset;
        }
        
        return result;
    }
    
    // ========================================================================
    // Trade Execution Callbacks
    // ========================================================================
    
    void on_order_placed() {
        last_order_time_ = std::chrono::steady_clock::now();
        open_positions_++;
        daily_trades_++;
    }
    
    void on_position_closed(double pnl) {
        open_positions_ = std::max(0, open_positions_ - 1);
        daily_pnl_ += pnl;
    }
    
    // ========================================================================
    // Daily Reset
    // ========================================================================
    
    void reset_daily_stats() {
        daily_pnl_ = 0.0;
        daily_trades_ = 0;
    }
    
    // ========================================================================
    // Getters
    // ========================================================================
    
    double daily_pnl() const { return daily_pnl_; }
    int daily_trades() const { return daily_trades_; }
    int open_positions() const { return open_positions_; }
    const RiskConfig& config() const { return config_; }
    
    // Calculate expected fees for a trade
    double calculate_fees(double position_usd, bool is_taker = true) const {
        double fee_rate = is_taker ? config_.taker_fee_pct : config_.maker_fee_pct;
        return position_usd * (fee_rate / 100.0) * 2;  // Entry + Exit
    }
    
private:
    RiskConfig config_;
    
    // State
    int open_positions_ = 0;
    int daily_trades_ = 0;
    double daily_pnl_ = 0.0;
    std::chrono::steady_clock::time_point last_order_time_{};
    
    // Apply hardcoded limits that cannot be overridden
    static RiskConfig apply_hardcoded_limits(RiskConfig config) {
        config.stop_loss_pct = std::max(config.stop_loss_pct, limits::MIN_STOP_LOSS_PCT);
        config.max_position_usd = std::min(config.max_position_usd, limits::MAX_POSITION_USD);
        config.min_order_interval_ms = std::max(config.min_order_interval_ms, 
                                                 static_cast<int>(limits::MIN_ORDER_INTERVAL_MS));
        return config;
    }
};

} // namespace opus::risk
