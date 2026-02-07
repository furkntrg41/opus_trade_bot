#pragma once
// ============================================================================
// OPUS TRADE BOT - EMA (Exponential Moving Average)
// ============================================================================
// Foundation indicator for MACD and other trend-following strategies
// ============================================================================

#include "indicator_base.hpp"

namespace opus::strategy {

/// EMA Indicator with configurable period
template <size_t Period>
class EMA : public IndicatorBase<EMA<Period>> {
public:
    static_assert(Period > 0, "Period must be positive");

    // Smoothing multiplier: 2 / (Period + 1)
    static constexpr double MULTIPLIER = 2.0 / (Period + 1);

    EMA() { reset_impl(); }

    void update_impl(double price) {
        if (count_ == 0) {
            // First value is just the price
            ema_ = price;
            sum_ = price;
        } else if (count_ < Period) {
            // Still building initial SMA
            sum_ += price;
            ema_ = sum_ / static_cast<double>(count_ + 1);
        } else {
            // EMA calculation
            ema_ = (price - ema_) * MULTIPLIER + ema_;
        }
        ++count_;
    }

    [[nodiscard]] double value_impl() const { return ema_; }

    [[nodiscard]] bool is_ready_impl() const { return count_ >= Period; }

    void reset_impl() {
        count_ = 0;
        ema_ = 0.0;
        sum_ = 0.0;
    }

    [[nodiscard]] constexpr size_t period_impl() const { return Period; }

private:
    size_t count_;
    double ema_;
    double sum_;  // For initial SMA calculation
};

/// SMA Indicator (Simple Moving Average)
template <size_t Period>
class SMA : public IndicatorBase<SMA<Period>> {
public:
    static_assert(Period > 0, "Period must be positive");

    SMA() : window_(), count_(0) {}

    void update_impl(double price) {
        window_.push(price);
        ++count_;
    }

    [[nodiscard]] double value_impl() const {
        return window_.mean();
    }

    [[nodiscard]] bool is_ready_impl() const { return count_ >= Period; }

    void reset_impl() {
        window_.reset();
        count_ = 0;
    }

    [[nodiscard]] constexpr size_t period_impl() const { return Period; }

private:
    RollingWindow<Period> window_;
    size_t count_;
};

// Common EMA configurations
using EMA9 = EMA<9>;
using EMA12 = EMA<12>;
using EMA20 = EMA<20>;
using EMA26 = EMA<26>;
using EMA50 = EMA<50>;
using EMA200 = EMA<200>;

// Common SMA configurations
using SMA20 = SMA<20>;
using SMA50 = SMA<50>;
using SMA200 = SMA<200>;

}  // namespace opus::strategy
