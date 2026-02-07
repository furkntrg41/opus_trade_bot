// ============================================================================
// OPUS TRADE BOT - Signal Generator Implementation
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/strategy/indicators/rsi.hpp"
#include "opus/strategy/indicators/macd.hpp"
#include "opus/strategy/indicators/bollinger.hpp"
#include "opus/strategy/order_book_imbalance.hpp"

#include <algorithm>
#include <vector>

namespace opus::strategy {

// ============================================================================
// Signal Aggregator
// ============================================================================

/// Combines signals from multiple indicators
class SignalAggregator {
public:
    struct WeightedSignal {
        SignalStrength signal;
        double weight;
    };

    /// Add a signal with weight
    void add_signal(SignalStrength signal, double weight) {
        signals_.push_back({signal, weight});
        total_weight_ += weight;
    }

    /// Clear all signals
    void clear() {
        signals_.clear();
        total_weight_ = 0.0;
    }

    /// Get weighted average signal
    [[nodiscard]] SignalStrength weighted_average() const {
        if (signals_.empty() || total_weight_ == 0.0) {
            return SignalStrength{0.0};
        }

        double weighted_sum = 0.0;
        for (const auto& ws : signals_) {
            weighted_sum += ws.signal.value() * ws.weight;
        }

        return SignalStrength{weighted_sum / total_weight_};
    }

    /// Get signal if ALL signals agree on direction
    [[nodiscard]] SignalStrength unanimous() const {
        if (signals_.empty()) return SignalStrength{0.0};

        bool all_bullish = true;
        bool all_bearish = true;
        double min_strength = 1.0;

        for (const auto& ws : signals_) {
            if (!ws.signal.is_bullish()) all_bullish = false;
            if (!ws.signal.is_bearish()) all_bearish = false;
            min_strength = std::min(min_strength, std::abs(ws.signal.value()));
        }

        if (all_bullish) return SignalStrength{min_strength};
        if (all_bearish) return SignalStrength{-min_strength};
        return SignalStrength{0.0};
    }

    /// Get signal if ANY signal is strong enough
    [[nodiscard]] SignalStrength strongest() const {
        if (signals_.empty()) return SignalStrength{0.0};

        SignalStrength best{0.0};
        for (const auto& ws : signals_) {
            if (std::abs(ws.signal.value()) > std::abs(best.value())) {
                best = ws.signal;
            }
        }
        return best;
    }

private:
    std::vector<WeightedSignal> signals_;
    double total_weight_ = 0.0;
};

}  // namespace opus::strategy
