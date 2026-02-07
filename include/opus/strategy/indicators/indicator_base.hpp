#pragma once
// ============================================================================
// OPUS TRADE BOT - Indicator Base Class
// ============================================================================
// CRTP pattern for zero-overhead polymorphism
// All indicators share common interface without virtual call overhead
// ============================================================================

#include "opus/core/types.hpp"

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

namespace opus::strategy {

// ============================================================================
// Indicator Concept
// ============================================================================

template <typename T>
concept Indicator = requires(T indicator, double value) {
    { indicator.update(value) } -> std::same_as<void>;
    { indicator.value() } -> std::convertible_to<double>;
    { indicator.is_ready() } -> std::convertible_to<bool>;
    { indicator.reset() } -> std::same_as<void>;
};

// ============================================================================
// CRTP Base Class
// ============================================================================

template <typename Derived>
class IndicatorBase {
public:
    /// Update indicator with new price data
    void update(double value) {
        static_cast<Derived*>(this)->update_impl(value);
    }

    /// Get current indicator value
    [[nodiscard]] double value() const {
        return static_cast<const Derived*>(this)->value_impl();
    }

    /// Check if indicator has enough data
    [[nodiscard]] bool is_ready() const {
        return static_cast<const Derived*>(this)->is_ready_impl();
    }

    /// Reset indicator state
    void reset() {
        static_cast<Derived*>(this)->reset_impl();
    }

    /// Get indicator period
    [[nodiscard]] size_t period() const {
        return static_cast<const Derived*>(this)->period_impl();
    }

protected:
    IndicatorBase() = default;
    ~IndicatorBase() = default;
};

// ============================================================================
// Rolling Window for Historical Data
// ============================================================================

template <size_t MaxSize>
class RollingWindow {
public:
    RollingWindow() : size_(0), index_(0) {}

    void push(double value) {
        buffer_[index_] = value;
        index_ = (index_ + 1) % MaxSize;
        if (size_ < MaxSize) {
            ++size_;
        }
    }

    [[nodiscard]] double operator[](size_t i) const {
        // i=0 is the most recent value
        if (i >= size_) return 0.0;
        return buffer_[(index_ - 1 - i + MaxSize) % MaxSize];
    }

    [[nodiscard]] double oldest() const {
        if (size_ < MaxSize) return buffer_[0];
        return buffer_[index_];
    }

    [[nodiscard]] double newest() const {
        return buffer_[(index_ - 1 + MaxSize) % MaxSize];
    }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool is_full() const { return size_ == MaxSize; }

    void reset() {
        size_ = 0;
        index_ = 0;
    }

    /// Calculate sum of all elements
    [[nodiscard]] double sum() const {
        double s = 0.0;
        for (size_t i = 0; i < size_; ++i) {
            s += buffer_[i];
        }
        return s;
    }

    /// Calculate mean
    [[nodiscard]] double mean() const {
        if (size_ == 0) return 0.0;
        return sum() / static_cast<double>(size_);
    }

    /// Calculate standard deviation
    [[nodiscard]] double std_dev() const {
        if (size_ < 2) return 0.0;
        const double m = mean();
        double variance = 0.0;
        for (size_t i = 0; i < size_; ++i) {
            const double diff = buffer_[i] - m;
            variance += diff * diff;
        }
        return std::sqrt(variance / static_cast<double>(size_));
    }

private:
    std::array<double, MaxSize> buffer_{};
    size_t size_;
    size_t index_;
};

}  // namespace opus::strategy
