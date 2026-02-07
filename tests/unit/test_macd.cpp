// Placeholder for MACD tests
#include "opus/strategy/indicators/macd.hpp"
#include <gtest/gtest.h>

using namespace opus::strategy;

TEST(MACDTest, InitiallyNotReady) {
    MACD_12_26_9 macd;
    EXPECT_FALSE(macd.is_ready());
}
