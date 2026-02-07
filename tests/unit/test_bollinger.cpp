// Placeholder for Bollinger Bands tests
#include "opus/strategy/indicators/bollinger.hpp"
#include <gtest/gtest.h>

using namespace opus::strategy;

TEST(BollingerTest, InitiallyNotReady) {
    BB20_2 bb;
    EXPECT_FALSE(bb.is_ready());
}
