#pragma once

// ============================================================================
// OPUS TRADE BOT - Signal Filter
// ============================================================================
// Filters raw OBI signals to produce high-quality trade signals
// Reduces ~1666 raw signals to 3-4 qualified trades per session
// ============================================================================

#include <chrono>
#include <cmath>

namespace opus::strategy {

// ============================================================================
// Signal Filter Configuration
// ============================================================================

struct SignalFilterConfig {
    // Threshold Gates
    double imbalance_threshold = 0.6;       // Minimum |imb| to consider
    double high_conviction_threshold = 0.7; // Instant entry threshold
    
    // Confirmation Settings
    int confirmation_ticks = 3;             // Ticks required for normal threshold
    int high_conviction_ticks = 1;          // Ticks for high conviction (instant)
    
    // Cooldown Settings
    int cooldown_seconds = 30;              // Minimum time between trades
    
    // Spread Check
    double max_spread_pct = 0.05;           // Max acceptable spread %
};

// ============================================================================
// Signal Filter Class
// ============================================================================

class SignalFilter {
public:
    enum class SignalType {
        None = 0,
        Buy = 1,
        Sell = -1
    };
    
    struct FilteredSignal {
        SignalType type = SignalType::None;
        double imbalance = 0.0;
        double confidence = 0.0;  // 0.0 - 1.0
        bool is_high_conviction = false;
    };

    explicit SignalFilter(const SignalFilterConfig& config = {})
        : config_(config) {}
    
    // ========================================================================
    // Main Filter Method
    // ========================================================================
    
    FilteredSignal filter(double imbalance, double spread_pct, double bid, double ask) {
        FilteredSignal result;
        
        // Reset if imbalance direction changed
        SignalType current_direction = get_direction(imbalance);
        if (current_direction != last_direction_) {
            consecutive_ticks_ = 0;
            last_direction_ = current_direction;
        }
        
        // Filter 1: Spread Check
        if (spread_pct > config_.max_spread_pct) {
            reset_streak();
            return result;  // Reject - spread too wide
        }
        
        double abs_imb = std::abs(imbalance);
        
        // Filter 2: Threshold Gate
        if (abs_imb < config_.imbalance_threshold) {
            reset_streak();
            return result;  // Reject - below threshold
        }
        
        // Track consecutive ticks
        consecutive_ticks_++;
        
        // Filter 3: Dynamic Confirmation
        bool is_high_conviction = (abs_imb >= config_.high_conviction_threshold);
        int required_ticks = is_high_conviction 
            ? config_.high_conviction_ticks 
            : config_.confirmation_ticks;
        
        if (consecutive_ticks_ < required_ticks) {
            return result;  // Not enough confirmation yet
        }
        
        // Filter 4: Cooldown Check
        auto now = std::chrono::steady_clock::now();
        auto cooldown = std::chrono::seconds(config_.cooldown_seconds);
        
        if (current_direction == SignalType::Buy) {
            if (now - last_buy_time_ < cooldown) {
                return result;  // Buy cooldown active
            }
        } else if (current_direction == SignalType::Sell) {
            if (now - last_sell_time_ < cooldown) {
                return result;  // Sell cooldown active
            }
        }
        
        // All filters passed - generate qualified signal
        result.type = current_direction;
        result.imbalance = imbalance;
        result.is_high_conviction = is_high_conviction;
        result.confidence = calculate_confidence(abs_imb);
        
        // Update cooldown timers
        if (current_direction == SignalType::Buy) {
            last_buy_time_ = now;
        } else {
            last_sell_time_ = now;
        }
        
        // Reset streak after signal
        reset_streak();
        
        return result;
    }
    
    // ========================================================================
    // Statistics
    // ========================================================================
    
    struct Stats {
        uint64_t raw_signals = 0;
        uint64_t threshold_filtered = 0;
        uint64_t spread_filtered = 0;
        uint64_t confirmation_filtered = 0;
        uint64_t cooldown_filtered = 0;
        uint64_t qualified_signals = 0;
    };
    
    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }
    
private:
    SignalFilterConfig config_;
    
    // State tracking
    SignalType last_direction_ = SignalType::None;
    int consecutive_ticks_ = 0;
    
    // Cooldown timers
    std::chrono::steady_clock::time_point last_buy_time_{};
    std::chrono::steady_clock::time_point last_sell_time_{};
    
    // Statistics
    Stats stats_;
    
    SignalType get_direction(double imbalance) const {
        if (imbalance > 0) return SignalType::Buy;
        if (imbalance < 0) return SignalType::Sell;
        return SignalType::None;
    }
    
    void reset_streak() {
        consecutive_ticks_ = 0;
    }
    
    double calculate_confidence(double abs_imb) const {
        // Map imbalance to confidence: 0.6 -> 0.5, 1.0 -> 1.0
        double normalized = (abs_imb - config_.imbalance_threshold) / 
                           (1.0 - config_.imbalance_threshold);
        return std::min(1.0, std::max(0.5, 0.5 + normalized * 0.5));
    }
};

} // namespace opus::strategy
