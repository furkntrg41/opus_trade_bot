// Placeholder for Order Book tests
#include "opus/market/order_book.hpp"
#include <gtest/gtest.h>

using namespace opus::market;

TEST(OrderBookTest, Initialize) {
    OrderBook book;
    EXPECT_FALSE(book.is_initialized());
}
