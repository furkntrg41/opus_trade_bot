// ============================================================================
// OPUS TRADE BOT - Core Types Unit Tests
// ============================================================================

#include "opus/core/types.hpp"

#include <gtest/gtest.h>

using namespace opus;

// ============================================================================
// Price Tests
// ============================================================================

TEST(PriceTest, DefaultConstruction) {
    Price p;
    EXPECT_EQ(p.raw(), 0);
}

TEST(PriceTest, FromDouble) {
    Price p = Price::from_double(42000.50);
    EXPECT_DOUBLE_EQ(p.to_double(), 42000.50);
}

TEST(PriceTest, Arithmetic) {
    Price p1 = Price::from_double(100.0);
    Price p2 = Price::from_double(50.0);

    Price sum = p1 + p2;
    EXPECT_DOUBLE_EQ(sum.to_double(), 150.0);

    Price diff = p1 - p2;
    EXPECT_DOUBLE_EQ(diff.to_double(), 50.0);
}

TEST(PriceTest, Comparison) {
    Price p1 = Price::from_double(100.0);
    Price p2 = Price::from_double(50.0);
    Price p3 = Price::from_double(100.0);

    EXPECT_GT(p1, p2);
    EXPECT_LT(p2, p1);
    EXPECT_EQ(p1, p3);
}

TEST(PriceTest, IsValid) {
    Price valid = Price::from_double(100.0);
    Price invalid = Price{0};
    Price negative = Price{-1};

    EXPECT_TRUE(valid.is_valid());
    EXPECT_FALSE(invalid.is_valid());
    EXPECT_FALSE(negative.is_valid());
}

// ============================================================================
// Quantity Tests
// ============================================================================

TEST(QuantityTest, FromDouble) {
    Quantity q = Quantity::from_double(1.5);
    EXPECT_DOUBLE_EQ(q.to_double(), 1.5);
}

TEST(QuantityTest, Arithmetic) {
    Quantity q1 = Quantity::from_double(10.0);
    Quantity q2 = Quantity::from_double(3.0);

    Quantity sum = q1 + q2;
    EXPECT_DOUBLE_EQ(sum.to_double(), 13.0);

    Quantity diff = q1 - q2;
    EXPECT_DOUBLE_EQ(diff.to_double(), 7.0);
}

// ============================================================================
// Symbol Tests
// ============================================================================

TEST(SymbolTest, Construction) {
    Symbol s("BTCUSDT");
    EXPECT_EQ(s.view(), "BTCUSDT");
    EXPECT_EQ(s.size(), 7);
}

TEST(SymbolTest, Truncation) {
    // Symbols longer than MAX_LENGTH should be truncated
    Symbol s("VERYLONGSYMBOLNAME");
    EXPECT_LE(s.size(), Symbol::MAX_LENGTH);
}

TEST(SymbolTest, Equality) {
    Symbol s1("BTCUSDT");
    Symbol s2("BTCUSDT");
    Symbol s3("ETHUSDT");

    EXPECT_EQ(s1, s2);
    EXPECT_NE(s1, s3);
}

TEST(SymbolTest, Hashing) {
    Symbol s("BTCUSDT");
    std::hash<Symbol> hasher;
    size_t hash = hasher(s);
    EXPECT_NE(hash, 0);
}

// ============================================================================
// SignalStrength Tests
// ============================================================================

TEST(SignalStrengthTest, Clamping) {
    SignalStrength strong_bullish{2.0};
    EXPECT_DOUBLE_EQ(strong_bullish.value(), 1.0);  // Clamped to max

    SignalStrength strong_bearish{-2.0};
    EXPECT_DOUBLE_EQ(strong_bearish.value(), -1.0);  // Clamped to min
}

TEST(SignalStrengthTest, Direction) {
    SignalStrength bullish{0.5};
    SignalStrength bearish{-0.5};
    SignalStrength neutral{0.0};

    EXPECT_TRUE(bullish.is_bullish());
    EXPECT_FALSE(bullish.is_bearish());

    EXPECT_TRUE(bearish.is_bearish());
    EXPECT_FALSE(bearish.is_bullish());

    EXPECT_TRUE(neutral.is_neutral());
}

// ============================================================================
// Timestamp Tests
// ============================================================================

TEST(TimestampTest, Now) {
    Timestamp t1 = now();
    Timestamp t2 = now();
    EXPECT_LE(t1, t2);
}

TEST(TimestampTest, EpochConversion) {
    int64_t epoch_ms = 1700000000000;  // Some timestamp
    Timestamp ts = from_epoch_ms(epoch_ms);
    int64_t back = to_epoch_ms(ts);
    EXPECT_EQ(epoch_ms, back);
}
