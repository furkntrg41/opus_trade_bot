#pragma once
// ============================================================================
// OPUS TRADE BOT - Order Book Manager
// ============================================================================
// In-memory L2 order book with efficient updates
// Optimized for cache locality and fast lookup
// ============================================================================

#include "opus/core/types.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace opus::market {

// ============================================================================
// Order Book Configuration
// ============================================================================

struct OrderBookConfig {
    size_t max_depth = 1000;        // Maximum levels to store
    bool maintain_checksum = true;  // For Binance depth stream validation
};

// ============================================================================
// Order Book Implementation
// ============================================================================

class OrderBook {
public:
    static constexpr size_t MAX_LEVELS = 1000;

    explicit OrderBook(const OrderBookConfig& config = {});

    // ========================================================================
    // Snapshot Operations
    // ========================================================================

    /// Initialize from snapshot
    void initialize(std::span<const PriceLevel> bids,
                    std::span<const PriceLevel> asks,
                    int64_t last_update_id);

    /// Clear the order book
    void clear();

    // ========================================================================
    // Incremental Updates
    // ========================================================================

    /// Update a single bid level (qty=0 means remove)
    void update_bid(Price price, Quantity qty);

    /// Update a single ask level
    void update_ask(Price price, Quantity qty);

    /// Batch update (for efficiency)
    void update_batch(std::span<const PriceLevel> bids, std::span<const PriceLevel> asks);

    /// Set last update ID (for synchronization)
    void set_last_update_id(int64_t id) { last_update_id_ = id; }

    // ========================================================================
    // Query Operations
    // ========================================================================

    /// Get best bid
    [[nodiscard]] const PriceLevel* best_bid() const;

    /// Get best ask
    [[nodiscard]] const PriceLevel* best_ask() const;

    /// Get mid price
    [[nodiscard]] Price mid_price() const;

    /// Get spread
    [[nodiscard]] Price spread() const;

    /// Get spread as percentage of mid price
    [[nodiscard]] double spread_pct() const;

    /// Get top N bid levels
    [[nodiscard]] std::span<const PriceLevel> bids(size_t n = MAX_LEVELS) const;

    /// Get top N ask levels
    [[nodiscard]] std::span<const PriceLevel> asks(size_t n = MAX_LEVELS) const;

    /// Get bid depth (total volume) at top N levels
    [[nodiscard]] Quantity bid_depth(size_t levels = 10) const;

    /// Get ask depth at top N levels
    [[nodiscard]] Quantity ask_depth(size_t levels = 10) const;

    /// Get last update ID
    [[nodiscard]] int64_t last_update_id() const { return last_update_id_; }

    /// Get timestamp of last update
    [[nodiscard]] Timestamp last_update_time() const { return last_update_time_; }

    /// Check if book is initialized
    [[nodiscard]] bool is_initialized() const { return initialized_; }

    /// Get number of bid levels
    [[nodiscard]] size_t bid_count() const { return bid_count_; }

    /// Get number of ask levels
    [[nodiscard]] size_t ask_count() const { return ask_count_; }

private:
    // Sorted arrays for cache-friendly access
    std::array<PriceLevel, MAX_LEVELS> bids_;
    std::array<PriceLevel, MAX_LEVELS> asks_;
    size_t bid_count_ = 0;
    size_t ask_count_ = 0;

    int64_t last_update_id_ = 0;
    Timestamp last_update_time_;
    bool initialized_ = false;

    OrderBookConfig config_;

    // Helper methods
    void insert_bid(Price price, Quantity qty);
    void insert_ask(Price price, Quantity qty);
    void remove_bid(Price price);
    void remove_ask(Price price);
};

// ============================================================================
// Order Book Snapshot (for binary serialization)
// ============================================================================

#pragma pack(push, 1)
struct OrderBookSnapshot {
    int64_t timestamp_ns;
    int64_t last_update_id;
    uint16_t bid_count;
    uint16_t ask_count;
    // Followed by bid_count PriceLevels for bids, then ask_count for asks
};
#pragma pack(pop)

}  // namespace opus::market
