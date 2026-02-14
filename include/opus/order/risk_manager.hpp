#pragma once
// ============================================================================
// OPUS TRADE BOT - Risk Manager
// ============================================================================
// Position sizing, stop-loss, profit-taking, and exposure limits
// ============================================================================

#include "opus/core/types.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace opus::order {

// ============================================================================
// Risk Parameters
// ============================================================================

struct RiskConfig {
    // Position sizing
    double max_position_pct = 0.1;       // Max 10% of account per position
    double max_total_exposure_pct = 0.5; // Max 50% total exposure
    double risk_per_trade_pct = 0.02;    // 2% risk per trade

    // Stop-loss / Take-profit
    double default_stop_loss_pct = 0.02;    // 2% stop-loss
    double default_take_profit_pct = 0.04;  // 4% take-profit (2:1 R:R)
    bool use_trailing_stop = true;
    double trailing_stop_pct = 0.015;       // 1.5% trailing

    // Drawdown protection
    double max_daily_drawdown_pct = 0.05;   // 5% max daily loss
    double max_total_drawdown_pct = 0.15;   // 15% max total drawdown

    // Order limits
    int max_open_positions = 5;
    int max_orders_per_minute = 60;

    // Leverage
    int max_leverage = 10;
};

// ============================================================================
// Position Sizing Calculators
// ============================================================================

class PositionSizer {
public:
    /// Fixed fractional position sizing
    /// risk_amount = account_balance * risk_per_trade_pct
    /// position_size = risk_amount / (entry_price * stop_loss_pct)
    [[nodiscard]] static Quantity fixed_fractional(double account_balance,
                                                    Price entry_price,
                                                    double stop_loss_pct,
                                                    double risk_per_trade_pct) {
        if (stop_loss_pct <= 0 || entry_price.to_double() <= 0) {
            return Quantity{0};
        }

        const double risk_amount = account_balance * risk_per_trade_pct;
        const double stop_distance = entry_price.to_double() * stop_loss_pct;
        const double size = risk_amount / stop_distance;

        return Quantity::from_double(size);
    }

    /// Kelly Criterion position sizing
    /// f* = (bp - q) / b
    /// where: b = win/loss ratio, p = win probability, q = 1 - p
    [[nodiscard]] static double kelly_fraction(double win_probability,
                                                double avg_win,
                                                double avg_loss) {
        if (avg_loss <= 0) return 0.0;

        const double b = avg_win / avg_loss;  // Win/loss ratio
        const double p = win_probability;
        const double q = 1.0 - p;

        const double kelly = (b * p - q) / b;

        // Half-Kelly for safety
        return std::clamp(kelly * 0.5, 0.0, 0.25);
    }

    /// ATR-based position sizing
    [[nodiscard]] static Quantity atr_based(double account_balance,
                                             Price entry_price,
                                             double atr,
                                             double atr_multiplier = 2.0,
                                             double risk_per_trade_pct = 0.02) {
        if (atr <= 0 || entry_price.to_double() <= 0) {
            return Quantity{0};
        }

        const double risk_amount = account_balance * risk_per_trade_pct;
        const double stop_distance = atr * atr_multiplier;
        const double size = risk_amount / stop_distance;

        return Quantity::from_double(size);
    }
};

// ============================================================================
// Risk Manager
// ============================================================================

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config = RiskConfig{}) : config_(config) {}

    // ========================================================================
    // Pre-Trade Checks
    // ========================================================================

    /// Check if a new position is allowed
    [[nodiscard]] bool can_open_position(double account_balance,
                                          double current_exposure,
                                          Quantity new_position_value) const {
        // Check max positions
        if (open_positions_ >= config_.max_open_positions) {
            return false;
        }

        // Check position size limit
        const double position_pct = new_position_value.to_double() / account_balance;
        if (position_pct > config_.max_position_pct) {
            return false;
        }

        // Check total exposure
        const double new_total = current_exposure + new_position_value.to_double();
        const double exposure_pct = new_total / account_balance;
        if (exposure_pct > config_.max_total_exposure_pct) {
            return false;
        }

        // Check drawdown limits
        if (daily_pnl_ < -account_balance * config_.max_daily_drawdown_pct) {
            return false;  // Daily loss limit hit
        }

        return true;
    }

    /// Calculate maximum allowed position size
    [[nodiscard]] Quantity max_position_size(double account_balance,
                                              Price entry_price,
                                              int leverage = 1) const {
        const double max_value = account_balance * config_.max_position_pct * leverage;
        const double max_qty = max_value / entry_price.to_double();
        return Quantity::from_double(max_qty);
    }

    // ========================================================================
    // Stop-Loss / Take-Profit Calculation
    // ========================================================================

    /// Calculate stop-loss price
    [[nodiscard]] Price calculate_stop_loss(Price entry_price, Side side) const {
        const double pct = config_.default_stop_loss_pct;
        const double entry = entry_price.to_double();

        if (side == Side::Buy) {
            return Price::from_double(entry * (1.0 - pct));
        } else {
            return Price::from_double(entry * (1.0 + pct));
        }
    }

    /// Calculate take-profit price
    [[nodiscard]] Price calculate_take_profit(Price entry_price, Side side) const {
        const double pct = config_.default_take_profit_pct;
        const double entry = entry_price.to_double();

        if (side == Side::Buy) {
            return Price::from_double(entry * (1.0 + pct));
        } else {
            return Price::from_double(entry * (1.0 - pct));
        }
    }

    /// Calculate trailing stop price
    [[nodiscard]] Price calculate_trailing_stop(Price highest_price,
                                                 Price current_stop,
                                                 Side side) const {
        if (!config_.use_trailing_stop) {
            return current_stop;
        }

        const double pct = config_.trailing_stop_pct;
        const double high = highest_price.to_double();

        if (side == Side::Buy) {
            const Price new_stop = Price::from_double(high * (1.0 - pct));
            return new_stop > current_stop ? new_stop : current_stop;
        } else {
            const Price new_stop = Price::from_double(high * (1.0 + pct));
            return new_stop < current_stop ? new_stop : current_stop;
        }
    }

    // ========================================================================
    // PnL Tracking
    // ========================================================================

    void record_trade_pnl(double pnl) {
        daily_pnl_ += pnl;
        total_pnl_ += pnl;

        if (pnl > 0) {
            ++winning_trades_;
            total_wins_ += pnl;
        } else if (pnl < 0) {
            ++losing_trades_;
            total_losses_ += std::abs(pnl);
        }
    }

    void position_opened() { ++open_positions_; }
    void position_closed() { if (open_positions_ > 0) --open_positions_; }

    /// Reset daily stats (call at start of each day)
    void reset_daily() { daily_pnl_ = 0.0; }

    // ========================================================================
    // Statistics
    // ========================================================================

    [[nodiscard]] double win_rate() const {
        const int total = winning_trades_ + losing_trades_;
        return total > 0 ? static_cast<double>(winning_trades_) / total : 0.0;
    }

    [[nodiscard]] double profit_factor() const {
        return total_losses_ > 0 ? total_wins_ / total_losses_ : 0.0;
    }

    [[nodiscard]] double daily_pnl() const { return daily_pnl_; }
    [[nodiscard]] double total_pnl() const { return total_pnl_; }
    [[nodiscard]] int open_positions() const { return open_positions_; }

    [[nodiscard]] const RiskConfig& config() const { return config_; }

private:
    RiskConfig config_;

    // Runtime state
    int open_positions_ = 0;
    double daily_pnl_ = 0.0;
    double total_pnl_ = 0.0;

    // Statistics
    int winning_trades_ = 0;
    int losing_trades_ = 0;
    double total_wins_ = 0.0;
    double total_losses_ = 0.0;
};

}  // namespace opus::order
