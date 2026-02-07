#pragma once
// ============================================================================
// OPUS TRADE BOT - MACD (Moving Average Convergence Divergence)
// ============================================================================
// Trend-following momentum indicator
// Standard settings: 12/26/9 EMAs
// ============================================================================

#include "ema.hpp"
#include "indicator_base.hpp"
#include "opus/core/types.hpp"

#include <cmath>

namespace opus::strategy {

/// MACD Indicator with configurable periods
template <size_t FastPeriod = 12, size_t SlowPeriod = 26, size_t SignalPeriod = 9>
class MACD : public IndicatorBase<MACD<FastPeriod, SlowPeriod, SignalPeriod>> {
public:
    static_assert(FastPeriod < SlowPeriod, "Fast period must be less than slow period");

    MACD() { reset_impl(); }

    void update_impl(double price) {
        fast_ema_.update(price);
        slow_ema_.update(price);
        ++count_;

        if (count_ >= SlowPeriod) {
            // Calculate MACD line
            macd_line_ = fast_ema_.value() - slow_ema_.value();

            // Update signal line (EMA of MACD)
            signal_ema_.update(macd_line_);

            // Store previous for crossover detection
            prev_histogram_ = histogram_;
            histogram_ = macd_line_ - signal_ema_.value();
        }
    }

    /// MACD line (fast EMA - slow EMA)
    [[nodiscard]] double value_impl() const { return macd_line_; }

    /// Signal line (EMA of MACD)
    [[nodiscard]] double signal_line() const { return signal_ema_.value(); }

    /// Histogram (MACD - Signal)
    [[nodiscard]] double histogram() const { return histogram_; }

    [[nodiscard]] bool is_ready_impl() const {
        return count_ >= SlowPeriod + SignalPeriod;
    }

    void reset_impl() {
        count_ = 0;
        macd_line_ = 0.0;
        histogram_ = 0.0;
        prev_histogram_ = 0.0;
        fast_ema_.reset();
        slow_ema_.reset();
        signal_ema_.reset();
    }

    [[nodiscard]] constexpr size_t period_impl() const { return SlowPeriod + SignalPeriod; }

    // ========================================================================
    // Trading Signals
    // ========================================================================

    /// Bullish crossover: MACD crosses above signal line
    [[nodiscard]] bool is_bullish_crossover() const {
        return is_ready_impl() && prev_histogram_ <= 0.0 && histogram_ > 0.0;
    }

    /// Bearish crossover: MACD crosses below signal line
    [[nodiscard]] bool is_bearish_crossover() const {
        return is_ready_impl() && prev_histogram_ >= 0.0 && histogram_ < 0.0;
    }

    /// MACD above zero (bullish territory)
    [[nodiscard]] bool is_bullish() const {
        return is_ready_impl() && macd_line_ > 0.0;
    }

    /// MACD below zero (bearish territory)
    [[nodiscard]] bool is_bearish() const {
        return is_ready_impl() && macd_line_ < 0.0;
    }

    /// Histogram expanding (momentum increasing)
    [[nodiscard]] bool is_momentum_increasing() const {
        return is_ready_impl() && std::abs(histogram_) > std::abs(prev_histogram_);
    }

    /// Generate trading signal
    [[nodiscard]] SignalStrength signal() const {
        if (!is_ready_impl()) return SignalStrength{0.0};

        // Strong signal on crossover
        if (is_bullish_crossover()) {
            return SignalStrength{0.8};  // Strong buy
        }
        if (is_bearish_crossover()) {
            return SignalStrength{-0.8};  // Strong sell
        }

        // Medium signal based on histogram direction
        if (histogram_ > 0.0 && is_momentum_increasing()) {
            return SignalStrength{0.4};  // Moderate buy
        }
        if (histogram_ < 0.0 && is_momentum_increasing()) {
            return SignalStrength{-0.4};  // Moderate sell
        }

        return SignalStrength{0.0};  // Neutral
    }

private:
    EMA<FastPeriod> fast_ema_;
    EMA<SlowPeriod> slow_ema_;
    EMA<SignalPeriod> signal_ema_;

    size_t count_;
    double macd_line_;
    double histogram_;
    double prev_histogram_;
};

// Standard MACD configuration
using MACD_12_26_9 = MACD<12, 26, 9>;

// Alternative configurations
using MACD_8_17_9 = MACD<8, 17, 9>;    // Faster
using MACD_19_39_9 = MACD<19, 39, 9>;  // Slower

}  // namespace opus::strategy
