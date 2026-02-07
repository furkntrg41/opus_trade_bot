// ============================================================================
// OPUS TRADE BOT - Binary Data Writer
// ============================================================================
// High-performance binary data storage using memory-mapped files
// ============================================================================

#include "opus/core/types.hpp"
#include "opus/market/order_book.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace opus::market {

// ============================================================================
// Binary Data Writer (simplified implementation)
// ============================================================================

class BinaryDataWriter {
public:
    explicit BinaryDataWriter(const std::string& file_path)
        : file_(file_path, std::ios::binary | std::ios::app) {}

    ~BinaryDataWriter() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    /// Write a market tick to file
    void write_tick(const MarketTick& tick) {
        if (!file_.is_open()) return;

        // Write entire tick at once (it's already packed to 40 bytes)
        file_.write(reinterpret_cast<const char*>(&tick), sizeof(MarketTick));
    }

    /// Write order book snapshot
    void write_order_book_snapshot(const OrderBookSnapshot& snapshot,
                                   const std::vector<PriceLevel>& bids,
                                   const std::vector<PriceLevel>& asks) {
        if (!file_.is_open()) return;

        // Write header
        file_.write(reinterpret_cast<const char*>(&snapshot), sizeof(snapshot));

        // Write bids
        for (const auto& level : bids) {
            int64_t price = level.price.raw();
            int64_t qty = level.quantity.raw();
            file_.write(reinterpret_cast<const char*>(&price), sizeof(price));
            file_.write(reinterpret_cast<const char*>(&qty), sizeof(qty));
        }

        // Write asks
        for (const auto& level : asks) {
            int64_t price = level.price.raw();
            int64_t qty = level.quantity.raw();
            file_.write(reinterpret_cast<const char*>(&price), sizeof(price));
            file_.write(reinterpret_cast<const char*>(&qty), sizeof(qty));
        }
    }

    void flush() {
        if (file_.is_open()) {
            file_.flush();
        }
    }

    [[nodiscard]] bool is_open() const { return file_.is_open(); }

private:
    std::ofstream file_;
};

}  // namespace opus::market
