// ============================================================================
// OPUS TRADE BOT - Ring Buffer Unit Tests
// ============================================================================

#include "opus/core/memory_pool.hpp"
#include "opus/core/ring_buffer.hpp"

#include <gtest/gtest.h>
#include <thread>

using namespace opus;

// Test data structure
struct TestMessage {
    int id;
    double value;
};

// ============================================================================
// SPSC Ring Buffer Tests
// ============================================================================

class SPSCRingBufferTest : public ::testing::Test {
protected:
    static constexpr size_t CAPACITY = 64;
    SPSCRingBuffer<TestMessage, CAPACITY> buffer;
};

TEST_F(SPSCRingBufferTest, InitiallyEmpty) {
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.full());
    EXPECT_EQ(buffer.size(), 0);
}

TEST_F(SPSCRingBufferTest, PushAndPop) {
    TestMessage msg{1, 3.14};
    
    EXPECT_TRUE(buffer.try_push(msg));
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 1);

    auto result = buffer.try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, 1);
    EXPECT_DOUBLE_EQ(result->value, 3.14);
    EXPECT_TRUE(buffer.empty());
}

TEST_F(SPSCRingBufferTest, FIFO) {
    for (int i = 0; i < 10; ++i) {
        buffer.try_push({i, static_cast<double>(i)});
    }

    for (int i = 0; i < 10; ++i) {
        auto result = buffer.try_pop();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->id, i);
    }
}

TEST_F(SPSCRingBufferTest, Full) {
    // Fill the buffer (capacity - 1 elements)
    for (size_t i = 0; i < CAPACITY - 1; ++i) {
        EXPECT_TRUE(buffer.try_push({static_cast<int>(i), 0.0}));
    }

    EXPECT_TRUE(buffer.full());
    EXPECT_FALSE(buffer.try_push({999, 0.0}));  // Should fail
}

TEST_F(SPSCRingBufferTest, Wrap) {
    // Push and pop to make the buffer wrap
    for (int round = 0; round < 3; ++round) {
        for (size_t i = 0; i < CAPACITY / 2; ++i) {
            buffer.try_push({static_cast<int>(i), 0.0});
        }
        for (size_t i = 0; i < CAPACITY / 2; ++i) {
            buffer.try_pop();
        }
    }

    EXPECT_TRUE(buffer.empty());
}

TEST_F(SPSCRingBufferTest, Clear) {
    for (int i = 0; i < 10; ++i) {
        buffer.try_push({i, 0.0});
    }
    
    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0);
}

// ============================================================================
// Memory Pool Tests
// ============================================================================

TEST(MemoryPoolTest, PoolAllocation) {
    PoolAllocator<TestMessage, 100> pool;

    EXPECT_EQ(pool.capacity(), 100);
    EXPECT_EQ(pool.allocated(), 0);

    TestMessage* ptr = pool.allocate();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(pool.allocated(), 1);

    pool.deallocate(ptr);
    EXPECT_EQ(pool.allocated(), 0);
}

TEST(MemoryPoolTest, PoolExhaustion) {
    // Use int64_t because PoolAllocator requires sizeof(T) >= sizeof(void*)
    PoolAllocator<int64_t, 3> pool;

    int64_t* p1 = pool.allocate();
    int64_t* p2 = pool.allocate();
    int64_t* p3 = pool.allocate();

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p3, nullptr);

    int64_t* p4 = pool.allocate();
    EXPECT_EQ(p4, nullptr);  // Pool exhausted

    pool.deallocate(p1);
    p4 = pool.allocate();
    EXPECT_NE(p4, nullptr);  // Now available
}

TEST(MemoryPoolTest, CreateDestroy) {
    PoolAllocator<std::string, 10> pool;

    std::string* s = pool.create("Hello, World!");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "Hello, World!");

    pool.destroy(s);
    EXPECT_EQ(pool.allocated(), 0);
}

TEST(ArenaAllocatorTest, BasicAllocation) {
    ArenaAllocator arena(1024);

    int* p1 = arena.allocate<int>(10);
    ASSERT_NE(p1, nullptr);

    double* p2 = arena.allocate<double>(5);
    ASSERT_NE(p2, nullptr);

    EXPECT_GT(arena.used(), 0);
    EXPECT_LT(arena.remaining(), 1024);
}

TEST(ArenaAllocatorTest, Reset) {
    ArenaAllocator arena(1024);

    arena.allocate<int>(100);
    EXPECT_GT(arena.used(), 0);

    arena.reset();
    EXPECT_EQ(arena.used(), 0);
    EXPECT_EQ(arena.remaining(), 1024);
}
