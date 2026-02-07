// ============================================================================
// OPUS TRADE BOT - RSI Unit Tests
// ============================================================================

#include "opus/strategy/indicators/rsi.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace opus::strategy;

class RSITest : public ::testing::Test {
protected:
    RSI14 rsi;
};

TEST_F(RSITest, InitiallyNotReady) {
    EXPECT_FALSE(rsi.is_ready());
}

TEST_F(RSITest, ReadyAfterEnoughData) {
    // RSI14 needs 15 data points to be ready
    for (int i = 0; i < 15; ++i) {
        rsi.update(100.0 + i);
    }
    EXPECT_TRUE(rsi.is_ready());
}

TEST_F(RSITest, ValueInValidRange) {
    // Add some price data
    std::vector<double> prices = {
        44.0, 44.34, 44.09, 43.61, 44.33,
        44.83, 45.10, 45.42, 45.84, 46.08,
        45.89, 46.03, 45.61, 46.28, 46.28,
        46.00, 46.03, 46.41, 46.22, 45.64
    };

    for (double price : prices) {
        rsi.update(price);
    }

    ASSERT_TRUE(rsi.is_ready());
    
    double rsi_value = rsi.value();
    EXPECT_GE(rsi_value, 0.0);
    EXPECT_LE(rsi_value, 100.0);
}

TEST_F(RSITest, OverboughtDetection) {
    // Simulate strong uptrend
    double price = 100.0;
    for (int i = 0; i < 20; ++i) {
        price += 2.0;  // Consistent gains
        rsi.update(price);
    }

    ASSERT_TRUE(rsi.is_ready());
    EXPECT_GT(rsi.value(), 70.0);
    EXPECT_TRUE(rsi.is_overbought());
}

TEST_F(RSITest, OversoldDetection) {
    // Simulate strong downtrend
    double price = 100.0;
    for (int i = 0; i < 20; ++i) {
        price -= 2.0;  // Consistent losses
        rsi.update(price);
    }

    ASSERT_TRUE(rsi.is_ready());
    EXPECT_LT(rsi.value(), 30.0);
    EXPECT_TRUE(rsi.is_oversold());
}

TEST_F(RSITest, Reset) {
    for (int i = 0; i < 20; ++i) {
        rsi.update(100.0 + i);
    }
    EXPECT_TRUE(rsi.is_ready());

    rsi.reset();
    EXPECT_FALSE(rsi.is_ready());
}

TEST_F(RSITest, SignalGeneration) {
    // Oversold condition should generate bullish signal
    double price = 100.0;
    for (int i = 0; i < 20; ++i) {
        price -= 2.0;
        rsi.update(price);
    }

    auto signal = rsi.signal();
    EXPECT_TRUE(signal.is_bullish());
}
