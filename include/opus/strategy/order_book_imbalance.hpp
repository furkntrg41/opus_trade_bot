#pragma once
// ============================================================================
// OPUS TRADE BOT - Order Book Imbalance Strategy
// ============================================================================
// HFT strategy based on bid/ask volume ratio
// Detects short-term price pressure from order book asymmetry
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/strategy/indicators/indicator_base.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>

namespace opus::strategy {

// ============================================================================
// Order Book Imbalance Calculator
// ============================================================================

class OrderBookImbalance {
public:
    /// Calculate imbalance from top N levels
    /// Returns value between -1 (heavy sells) and +1 (heavy buys)
    [[nodiscard]] static double calculate(std::span<const PriceLevel> bids,
                                          std::span<const PriceLevel> asks,
                                          size_t levels = 5) {
        if (bids.empty() || asks.empty()) return 0.0;

        const size_t n = std::min({levels, bids.size(), asks.size()});

        double bid_volume = 0.0;
        double ask_volume = 0.0;

        for (size_t i = 0; i < n; ++i) {
            bid_volume += bids[i].quantity.to_double();
            ask_volume += asks[i].quantity.to_double();
        }

        const double total = bid_volume + ask_volume;
        if (total == 0.0) return 0.0;

        // Imbalance ratio: (bid - ask) / (bid + ask)
        return (bid_volume - ask_volume) / total;
    }

    /// Weighted imbalance (closer levels have more weight)
    [[nodiscard]] static double calculate_weighted(std::span<const PriceLevel> bids,
                                                    std::span<const PriceLevel> asks,
                                                    size_t levels = 10) {
        if (bids.empty() || asks.empty()) return 0.0;

        const size_t n = std::min({levels, bids.size(), asks.size()});

        double bid_volume = 0.0;
        double ask_volume = 0.0;

        for (size_t i = 0; i < n; ++i) {
            // Weight decreases with depth: 1.0, 0.9, 0.8, ...
            const double weight = 1.0 - (static_cast<double>(i) / levels);
            bid_volume += bids[i].quantity.to_double() * weight;
            ask_volume += asks[i].quantity.to_double() * weight;
        }

        const double total = bid_volume + ask_volume;
        if (total == 0.0) return 0.0;

        return (bid_volume - ask_volume) / total;
    }

    /// Calculate micro-price (volume-weighted mid price)
    [[nodiscard]] static double micro_price(const PriceLevel& best_bid,
                                             const PriceLevel& best_ask) {
        const double bid_qty = best_bid.quantity.to_double();
        const double ask_qty = best_ask.quantity.to_double();
        const double total = bid_qty + ask_qty;

        if (total == 0.0) {
            return (best_bid.price.to_double() + best_ask.price.to_double()) / 2.0;
        }

        // Weighted average: more weight to the side with less volume
        // (price tends to move toward the thinner side)
        const double bid_price = best_bid.price.to_double();
        const double ask_price = best_ask.price.to_double();

        return (bid_price * ask_qty + ask_price * bid_qty) / total;
    }
};

// ============================================================================
// Imbalance Signal Generator
// ============================================================================

class ImbalanceSignalGenerator {
public:
    struct Config {
        double threshold = 0.3;           // Minimum imbalance for signal
        double strong_threshold = 0.5;    // Strong signal threshold
        size_t depth_levels = 5;          // Number of book levels to analyze
        size_t smoothing_period = 10;     // EMA smoothing for noise reduction
    };

    explicit ImbalanceSignalGenerator(const Config& config = {})
        : config_(config), smoothed_imbalance_(0.0), sample_count_(0) {}

    /// Update with new order book data
    void update(std::span<const PriceLevel> bids, std::span<const PriceLevel> asks) {
        const double raw_imbalance =
            OrderBookImbalance::calculate(bids, asks, config_.depth_levels);

        // EMA smoothing
        if (sample_count_ == 0) {
            smoothed_imbalance_ = raw_imbalance;
        } else {
            const double alpha = 2.0 / (config_.smoothing_period + 1);
            smoothed_imbalance_ = alpha * raw_imbalance + (1.0 - alpha) * smoothed_imbalance_;
        }

        ++sample_count_;
        raw_imbalance_ = raw_imbalance;
    }

    /// Get current signal strength
    [[nodiscard]] SignalStrength signal() const {
        const double imb = std::abs(smoothed_imbalance_);

        if (imb < config_.threshold) {
            return SignalStrength{0.0};  // No signal
        }

        // Scale signal strength based on imbalance magnitude
        double strength = (imb - config_.threshold) / (1.0 - config_.threshold);
        strength = std::clamp(strength, 0.0, 1.0);

        // Direction based on imbalance sign
        if (smoothed_imbalance_ > 0) {
            return SignalStrength{strength};   // Bullish (more bids)
        } else {
            return SignalStrength{-strength};  // Bearish (more asks)
        }
    }

    /// Get raw (unsmoothed) imbalance
    [[nodiscard]] double raw_imbalance() const { return raw_imbalance_; }

    /// Get smoothed imbalance
    [[nodiscard]] double smoothed_imbalance() const { return smoothed_imbalance_; }

    /// Check if we have enough samples
    [[nodiscard]] bool is_ready() const { return sample_count_ >= config_.smoothing_period; }

    void reset() {
        smoothed_imbalance_ = 0.0;
        raw_imbalance_ = 0.0;
        sample_count_ = 0;
    }

private:
    Config config_;
    double smoothed_imbalance_;
    double raw_imbalance_;
    size_t sample_count_;
};

// ============================================================================
// Queue Position Estimator
// ============================================================================

/// Estimate queue position for order priority
class QueuePositionEstimator {
public:
    /// Estimate how many orders are ahead of us at a price level
    [[nodiscard]] static size_t estimate_queue_position(const PriceLevel& level,
                                                         Quantity our_size,
                                                         double avg_order_size = 0.1) {
        const double total_qty = level.quantity.to_double();
        const double our_qty = our_size.to_double();

        if (total_qty <= our_qty) {
            return 0;  // We're at the front
        }

        // Estimate number of orders ahead based on average order size
        const double qty_ahead = total_qty - our_qty;
        return static_cast<size_t>(qty_ahead / avg_order_size);
    }

    /// Estimate time to fill based on recent trade velocity
    [[nodiscard]] static std::chrono::milliseconds estimate_time_to_fill(
        size_t queue_position,
        double trades_per_second) {
        if (trades_per_second <= 0) {
            return std::chrono::milliseconds::max();
        }

        const double seconds = static_cast<double>(queue_position) / trades_per_second;
        return std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    }
};

}  // namespace opus::strategy
