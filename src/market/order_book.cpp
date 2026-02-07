// ============================================================================
// OPUS TRADE BOT - Order Book Implementation
// ============================================================================

#include "opus/market/order_book.hpp"

#include <algorithm>

namespace opus::market {

OrderBook::OrderBook(const OrderBookConfig& config) : config_(config) {
    clear();
}

void OrderBook::initialize(std::span<const PriceLevel> bids,
                           std::span<const PriceLevel> asks,
                           int64_t last_update_id) {
    clear();

    // Copy bids (already sorted in descending order)
    bid_count_ = std::min(bids.size(), MAX_LEVELS);
    std::copy_n(bids.begin(), bid_count_, bids_.begin());

    // Copy asks (already sorted in ascending order)
    ask_count_ = std::min(asks.size(), MAX_LEVELS);
    std::copy_n(asks.begin(), ask_count_, asks_.begin());

    last_update_id_ = last_update_id;
    last_update_time_ = now();
    initialized_ = true;
}

void OrderBook::clear() {
    bid_count_ = 0;
    ask_count_ = 0;
    last_update_id_ = 0;
    initialized_ = false;
}

void OrderBook::update_bid(Price price, Quantity qty) {
    last_update_time_ = now();

    if (qty.raw() == 0) {
        remove_bid(price);
    } else {
        insert_bid(price, qty);
    }
}

void OrderBook::update_ask(Price price, Quantity qty) {
    last_update_time_ = now();

    if (qty.raw() == 0) {
        remove_ask(price);
    } else {
        insert_ask(price, qty);
    }
}

void OrderBook::update_batch(std::span<const PriceLevel> bids,
                              std::span<const PriceLevel> asks) {
    for (const auto& level : bids) {
        update_bid(level.price, level.quantity);
    }
    for (const auto& level : asks) {
        update_ask(level.price, level.quantity);
    }
}

const PriceLevel* OrderBook::best_bid() const {
    return bid_count_ > 0 ? &bids_[0] : nullptr;
}

const PriceLevel* OrderBook::best_ask() const {
    return ask_count_ > 0 ? &asks_[0] : nullptr;
}

Price OrderBook::mid_price() const {
    const auto* bid = best_bid();
    const auto* ask = best_ask();

    if (!bid || !ask) return Price{0};

    return Price{(bid->price.raw() + ask->price.raw()) / 2};
}

Price OrderBook::spread() const {
    const auto* bid = best_bid();
    const auto* ask = best_ask();

    if (!bid || !ask) return Price{0};

    return Price{ask->price.raw() - bid->price.raw()};
}

double OrderBook::spread_pct() const {
    const auto mid = mid_price();
    const auto sprd = spread();

    if (mid.raw() == 0) return 0.0;

    return sprd.to_double() / mid.to_double() * 100.0;
}

std::span<const PriceLevel> OrderBook::bids(size_t n) const {
    return {bids_.data(), std::min(n, bid_count_)};
}

std::span<const PriceLevel> OrderBook::asks(size_t n) const {
    return {asks_.data(), std::min(n, ask_count_)};
}

Quantity OrderBook::bid_depth(size_t levels) const {
    Quantity total{0};
    const size_t n = std::min(levels, bid_count_);
    for (size_t i = 0; i < n; ++i) {
        total += bids_[i].quantity;
    }
    return total;
}

Quantity OrderBook::ask_depth(size_t levels) const {
    Quantity total{0};
    const size_t n = std::min(levels, ask_count_);
    for (size_t i = 0; i < n; ++i) {
        total += asks_[i].quantity;
    }
    return total;
}

void OrderBook::insert_bid(Price price, Quantity qty) {
    // Find insertion point (bids sorted descending by price)
    auto it = std::lower_bound(bids_.begin(), bids_.begin() + bid_count_, price,
                                [](const PriceLevel& level, Price p) {
                                    return level.price > p;  // Descending
                                });

    const size_t idx = std::distance(bids_.begin(), it);

    // Check if updating existing level
    if (idx < bid_count_ && bids_[idx].price == price) {
        bids_[idx].quantity = qty;
        return;
    }

    // Insert new level
    if (bid_count_ < MAX_LEVELS) {
        // Shift elements to make room
        std::move_backward(it, bids_.begin() + bid_count_, bids_.begin() + bid_count_ + 1);
        bids_[idx] = {price, qty, 0, 0};
        ++bid_count_;
    } else if (idx < MAX_LEVELS) {
        // Replace the worst level
        std::move_backward(it, bids_.begin() + MAX_LEVELS - 1, bids_.begin() + MAX_LEVELS);
        bids_[idx] = {price, qty, 0, 0};
    }
}

void OrderBook::insert_ask(Price price, Quantity qty) {
    // Find insertion point (asks sorted ascending by price)
    auto it = std::lower_bound(asks_.begin(), asks_.begin() + ask_count_, price,
                                [](const PriceLevel& level, Price p) {
                                    return level.price < p;  // Ascending
                                });

    const size_t idx = std::distance(asks_.begin(), it);

    // Check if updating existing level
    if (idx < ask_count_ && asks_[idx].price == price) {
        asks_[idx].quantity = qty;
        return;
    }

    // Insert new level
    if (ask_count_ < MAX_LEVELS) {
        std::move_backward(it, asks_.begin() + ask_count_, asks_.begin() + ask_count_ + 1);
        asks_[idx] = {price, qty, 0, 0};
        ++ask_count_;
    } else if (idx < MAX_LEVELS) {
        std::move_backward(it, asks_.begin() + MAX_LEVELS - 1, asks_.begin() + MAX_LEVELS);
        asks_[idx] = {price, qty, 0, 0};
    }
}

void OrderBook::remove_bid(Price price) {
    auto it = std::find_if(bids_.begin(), bids_.begin() + bid_count_,
                           [price](const PriceLevel& level) {
                               return level.price == price;
                           });

    if (it != bids_.begin() + bid_count_) {
        std::move(it + 1, bids_.begin() + bid_count_, it);
        --bid_count_;
    }
}

void OrderBook::remove_ask(Price price) {
    auto it = std::find_if(asks_.begin(), asks_.begin() + ask_count_,
                           [price](const PriceLevel& level) {
                               return level.price == price;
                           });

    if (it != asks_.begin() + ask_count_) {
        std::move(it + 1, asks_.begin() + ask_count_, it);
        --ask_count_;
    }
}

}  // namespace opus::market
