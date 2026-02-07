#pragma once
// ============================================================================
// OPUS TRADE BOT - Bollinger Bands
// ============================================================================
// Volatility indicator using standard deviation bands
// Default: 20-period SMA with 2 standard deviation bands
// ============================================================================

#include "indicator_base.hpp"
#include "opus/core/types.hpp"
#include <cmath>

namespace opus::strategy {

/// Bollinger Bands Indicator
template <size_t Period = 20, size_t StdDevMultiplier = 2>
class BollingerBands : public IndicatorBase<BollingerBands<Period, StdDevMultiplier>> {
public:
    BollingerBands() : window_(), count_(0) {}

    void update_impl(double price) {
        window_.push(price);
        latest_price_ = price;
        ++count_;

        if (count_ >= Period) {
            middle_ = window_.mean();
            std_dev_ = window_.std_dev();
            upper_ = middle_ + StdDevMultiplier * std_dev_;
            lower_ = middle_ - StdDevMultiplier * std_dev_;
        }
    }

    /// Middle band (SMA)
    [[nodiscard]] double value_impl() const { return middle_; }

    /// Upper band
    [[nodiscard]] double upper_band() const { return upper_; }

    /// Lower band
    [[nodiscard]] double lower_band() const { return lower_; }

    /// Band width (volatility measure)
    [[nodiscard]] double band_width() const {
        if (middle_ == 0.0) return 0.0;
        return (upper_ - lower_) / middle_;
    }

    /// %B indicator: where price is relative to bands
    /// 0 = at lower band, 0.5 = at middle, 1 = at upper band
    [[nodiscard]] double percent_b() const {
        if (upper_ == lower_) return 0.5;
        return (latest_price_ - lower_) / (upper_ - lower_);
    }

    [[nodiscard]] bool is_ready_impl() const { return count_ >= Period; }

    void reset_impl() {
        window_.reset();
        count_ = 0;
        middle_ = 0.0;
        upper_ = 0.0;
        lower_ = 0.0;
        std_dev_ = 0.0;
        latest_price_ = 0.0;
    }

    [[nodiscard]] constexpr size_t period_impl() const { return Period; }

    // ========================================================================
    // Trading Signals
    // ========================================================================

    /// Price touching or breaking upper band (potential sell/overbought)
    [[nodiscard]] bool is_at_upper() const {
        return is_ready_impl() && latest_price_ >= upper_;
    }

    /// Price touching or breaking lower band (potential buy/oversold)
    [[nodiscard]] bool is_at_lower() const {
        return is_ready_impl() && latest_price_ <= lower_;
    }

    /// Squeeze detected (low volatility, potential breakout)
    [[nodiscard]] bool is_squeeze() const {
        // Squeeze when band width is in lower percentile
        return is_ready_impl() && band_width() < 0.02;  // Adjust threshold as needed
    }

    /// Expansion detected (high volatility)
    [[nodiscard]] bool is_expansion() const {
        return is_ready_impl() && band_width() > 0.05;  // Adjust threshold as needed
    }

    /// Generate trading signal based on mean reversion
    [[nodiscard]] SignalStrength signal() const {
        if (!is_ready_impl()) return SignalStrength{0.0};

        const double pct_b = percent_b();

        if (pct_b <= 0.0) {
            // At or below lower band - strong buy signal
            return SignalStrength{0.8};
        } else if (pct_b >= 1.0) {
            // At or above upper band - strong sell signal
            return SignalStrength{-0.8};
        } else if (pct_b < 0.2) {
            // Near lower band - moderate buy
            return SignalStrength{0.4};
        } else if (pct_b > 0.8) {
            // Near upper band - moderate sell
            return SignalStrength{-0.4};
        }

        return SignalStrength{0.0};  // In the middle, neutral
    }

    /// Generate breakout signal (opposite of mean reversion)
    [[nodiscard]] SignalStrength breakout_signal() const {
        if (!is_ready_impl()) return SignalStrength{0.0};

        // Only valid during squeeze/low volatility
        if (!is_squeeze()) return SignalStrength{0.0};

        if (latest_price_ > upper_) {
            return SignalStrength{0.9};  // Bullish breakout
        } else if (latest_price_ < lower_) {
            return SignalStrength{-0.9};  // Bearish breakout
        }

        return SignalStrength{0.0};
    }

private:
    RollingWindow<Period> window_;
    size_t count_;
    double middle_;
    double upper_;
    double lower_;
    double std_dev_;
    double latest_price_;
};

// Standard Bollinger Bands
using BB20_2 = BollingerBands<20, 2>;

// Alternative configurations
using BB10_2 = BollingerBands<10, 2>;  // Faster, more sensitive
using BB50_2 = BollingerBands<50, 2>;  // Slower, smoother
using BB20_3 = BollingerBands<20, 3>;  // Wider bands

}  // namespace opus::strategy
