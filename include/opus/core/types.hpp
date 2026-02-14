#pragma once
// ============================================================================
// OPUS TRADE BOT - Core Types
// ============================================================================
// Fundamental type definitions for the trading system
// Using strong typing and fixed-point arithmetic for precision
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace opus {

// ============================================================================
// Time Types
// ============================================================================

/// Nanosecond precision timestamp
using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

/// Duration in nanoseconds
using Duration = std::chrono::nanoseconds;

/// Get current timestamp with nanosecond precision
[[nodiscard]] inline Timestamp now() noexcept {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
}

/// Convert timestamp to Unix epoch milliseconds (Binance format)
[[nodiscard]] inline int64_t to_epoch_ms(Timestamp ts) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch()).count();
}

/// Convert Unix epoch milliseconds to Timestamp
[[nodiscard]] inline Timestamp from_epoch_ms(int64_t epoch_ms) noexcept {
    return Timestamp{std::chrono::milliseconds{epoch_ms}};
}

// ============================================================================
// Price and Quantity Types (Fixed-Point Arithmetic)
// ============================================================================

/// Price with 8 decimal places precision (matches Binance)
/// Stored as int64 to avoid floating-point errors
/// 1 Price unit = 0.00000001 actual price
class Price {
public:
    static constexpr int64_t PRECISION = 100000000LL;  // 10^8
    static constexpr int DECIMAL_PLACES = 8;

    constexpr Price() noexcept : value_(0) {}
    constexpr explicit Price(int64_t raw_value) noexcept : value_(raw_value) {}

    /// Create from double (e.g., 42000.50 -> internal representation)
    [[nodiscard]] static Price from_double(double price) noexcept {
        return Price{static_cast<int64_t>(price * PRECISION)};
    }

    /// Convert to double for display/logging
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / PRECISION;
    }

    /// Raw internal value
    [[nodiscard]] constexpr int64_t raw() const noexcept { return value_; }

    /// Arithmetic operators
    constexpr Price operator+(Price other) const noexcept { return Price{value_ + other.value_}; }
    constexpr Price operator-(Price other) const noexcept { return Price{value_ - other.value_}; }
    constexpr Price& operator+=(Price other) noexcept { value_ += other.value_; return *this; }
    constexpr Price& operator-=(Price other) noexcept { value_ -= other.value_; return *this; }

    /// Comparison operators
    constexpr auto operator<=>(const Price&) const noexcept = default;

    /// Check if price is valid (non-zero, non-negative)
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ > 0; }

private:
    int64_t value_;
};

/// Quantity with 8 decimal places precision
class Quantity {
public:
    static constexpr int64_t PRECISION = 100000000LL;  // 10^8
    static constexpr int DECIMAL_PLACES = 8;

    constexpr Quantity() noexcept : value_(0) {}
    constexpr explicit Quantity(int64_t raw_value) noexcept : value_(raw_value) {}

    [[nodiscard]] static Quantity from_double(double qty) noexcept {
        if (!std::isfinite(qty)) return Quantity{0};
        // Clamp to prevent overflow
        if (qty > 9.2e10) return Quantity{std::numeric_limits<int64_t>::max()};
        if (qty < -9.2e10) return Quantity{std::numeric_limits<int64_t>::min() + 1}; // +1 to avoid collision with special "invalid" logic if any
        return Quantity{static_cast<int64_t>(qty * PRECISION)};
    }
    
    /// Create from USD value and Price (e.g. $100 at $50,000 = 0.002 BTC)
    [[nodiscard]] static Quantity from_usd_value(double usd_value, double price) noexcept {
        if (price <= 0) return Quantity{0};
        return from_double(usd_value / price);
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / PRECISION;
    }

    [[nodiscard]] constexpr int64_t raw() const noexcept { return value_; }

    constexpr Quantity operator+(Quantity other) const noexcept {
        return Quantity{value_ + other.value_};
    }
    constexpr Quantity operator-(Quantity other) const noexcept {
        return Quantity{value_ - other.value_};
    }
    constexpr Quantity& operator+=(Quantity other) noexcept { value_ += other.value_; return *this; }
    constexpr Quantity& operator-=(Quantity other) noexcept { value_ -= other.value_; return *this; }

    constexpr auto operator<=>(const Quantity&) const noexcept = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ > 0; }

private:
    int64_t value_;
};

// ============================================================================
// Trading Types
// ============================================================================

/// Order side
enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

/// Order type
enum class OrderType : uint8_t {
    Market = 0,
    Limit = 1,
    StopMarket = 2,
    StopLimit = 3,
    TakeProfit = 4,
    TakeProfitMarket = 5
};

/// Position side for futures
enum class PositionSide : uint8_t {
    Both = 0,   // One-way mode
    Long = 1,   // Hedge mode long
    Short = 2   // Hedge mode short
};

/// Order status
enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Canceled = 3,
    Rejected = 4,
    Expired = 5
};

/// Time in force
enum class TimeInForce : uint8_t {
    GTC = 0,  // Good Till Cancel
    IOC = 1,  // Immediate Or Cancel
    FOK = 2,  // Fill Or Kill
    GTX = 3   // Good Till Crossing (Post Only)
};

// ============================================================================
// Symbol Type
// ============================================================================

/// Trading symbol (e.g., "BTCUSDT")
/// Uses small string optimization for cache efficiency
class Symbol {
public:
    static constexpr size_t MAX_LENGTH = 15;

    Symbol() noexcept : length_(0) { data_[0] = '\0'; }

    explicit Symbol(std::string_view symbol) noexcept {
        length_ = static_cast<uint8_t>(std::min(symbol.size(), MAX_LENGTH));
        std::copy_n(symbol.data(), length_, data_);
        data_[length_] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {data_, length_};
    }

    [[nodiscard]] const char* c_str() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return length_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }

    bool operator==(const Symbol& other) const noexcept {
        return view() == other.view();
    }

    bool operator!=(const Symbol& other) const noexcept {
        return !(*this == other);
    }

    bool operator<(const Symbol& other) const noexcept {
        return view() < other.view();
    }

private:
    char data_[MAX_LENGTH + 1];
    uint8_t length_;
};

// ============================================================================
// Market Data Structures (Cache-line aligned for performance)
// ============================================================================

/// Single price level in order book
struct alignas(32) PriceLevel {
    Price price;
    Quantity quantity;
    uint32_t order_count;
    uint32_t padding;
};
static_assert(sizeof(PriceLevel) == 32, "PriceLevel must be 32 bytes");

/// Trade/Tick data
#pragma pack(push, 1)
struct MarketTick {
    uint64_t timestamp_ns;
    double bid_price;
    double ask_price;
    double bid_qty;
    double ask_qty;
};
#pragma pack(pop)
static_assert(sizeof(MarketTick) == 40, "MarketTick must be 40 bytes for binary storage");

/// Kline/Candlestick data
struct Kline {
    Timestamp open_time;
    Timestamp close_time;
    Price open;
    Price high;
    Price low;
    Price close;
    Quantity volume;
    Quantity quote_volume;
    uint32_t trade_count;
};

// ============================================================================
// Signal Types
// ============================================================================

/// Trading signal strength (-1.0 to +1.0)
/// Positive = bullish, Negative = bearish
class SignalStrength {
public:
    constexpr SignalStrength() noexcept : value_(0.0) {}
    constexpr explicit SignalStrength(double value) noexcept
        : value_(std::clamp(value, -1.0, 1.0)) {}

    [[nodiscard]] constexpr double value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_bullish() const noexcept { return value_ > 0.0; }
    [[nodiscard]] constexpr bool is_bearish() const noexcept { return value_ < 0.0; }
    [[nodiscard]] constexpr bool is_neutral() const noexcept { return value_ == 0.0; }

private:
    double value_;

    static constexpr double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

/// Signal from a strategy
struct TradingSignal {
    Symbol symbol;
    Side side;
    SignalStrength strength;
    Timestamp timestamp;
    Price suggested_entry;
    Price stop_loss;
    Price take_profit;
};

}  // namespace opus

// ============================================================================
// Hash specializations for use with containers
// ============================================================================
template <>
struct std::hash<opus::Symbol> {
    size_t operator()(const opus::Symbol& s) const noexcept {
        return std::hash<std::string_view>{}(s.view());
    }
};
