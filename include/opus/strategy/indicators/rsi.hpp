#pragma once
// ============================================================================
// OPUS TRADE BOT - RSI (Relative Strength Index)
// ============================================================================
// Momentum oscillator measuring speed and magnitude of price changes
// Range: 0-100, Overbought > 70, Oversold < 30
// ============================================================================

#include "indicator_base.hpp"
#include <algorithm>
#include <cmath>

namespace opus::strategy {

/// RSI Indicator with Wilder's smoothing
template <size_t Period = 14>
class RSI : public IndicatorBase<RSI<Period>> {
public:
    static constexpr double OVERBOUGHT = 70.0;
    static constexpr double OVERSOLD = 30.0;

    RSI() { reset_impl(); }

    void update_impl(double price) {
        if (count_ == 0) {
            prev_price_ = price;
            ++count_;
            return;
        }

        const double change = price - prev_price_;
        prev_price_ = price;
        ++count_;

        const double gain = std::max(change, 0.0);
        const double loss = std::max(-change, 0.0);

        if (count_ <= Period) {
            // Initial SMA period
            gain_sum_ += gain;
            loss_sum_ += loss;

            if (count_ == Period) {
                avg_gain_ = gain_sum_ / Period;
                avg_loss_ = loss_sum_ / Period;
            }
        } else {
            // Wilder's smoothing (exponential moving average)
            avg_gain_ = (avg_gain_ * (Period - 1) + gain) / Period;
            avg_loss_ = (avg_loss_ * (Period - 1) + loss) / Period;
        }
    }

    [[nodiscard]] double value_impl() const {
        if (!is_ready_impl()) return 50.0;  // Neutral value if not ready

        if (avg_loss_ == 0.0) return 100.0;  // All gains, max RSI

        const double rs = avg_gain_ / avg_loss_;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    [[nodiscard]] bool is_ready_impl() const {
        return count_ > Period;
    }

    void reset_impl() {
        count_ = 0;
        prev_price_ = 0.0;
        gain_sum_ = 0.0;
        loss_sum_ = 0.0;
        avg_gain_ = 0.0;
        avg_loss_ = 0.0;
    }

    [[nodiscard]] constexpr size_t period_impl() const { return Period; }

    // Convenience methods
    [[nodiscard]] bool is_overbought() const { return value_impl() > OVERBOUGHT; }
    [[nodiscard]] bool is_oversold() const { return value_impl() < OVERSOLD; }

    [[nodiscard]] SignalStrength signal() const {
        if (!is_ready_impl()) return SignalStrength{0.0};

        const double rsi = value_impl();

        if (rsi > OVERBOUGHT) {
            // Overbought - bearish signal
            return SignalStrength{-(rsi - OVERBOUGHT) / (100.0 - OVERBOUGHT)};
        } else if (rsi < OVERSOLD) {
            // Oversold - bullish signal
            return SignalStrength{(OVERSOLD - rsi) / OVERSOLD};
        }
        return SignalStrength{0.0};  // Neutral
    }

private:
    size_t count_;
    double prev_price_;
    double gain_sum_;
    double loss_sum_;
    double avg_gain_;
    double avg_loss_;
};

// Common RSI configurations
using RSI14 = RSI<14>;
using RSI7 = RSI<7>;
using RSI21 = RSI<21>;

}  // namespace opus::strategy
