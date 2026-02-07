# Tests CMake Configuration
# ==========================

enable_testing()

# Find Google Test
find_package(GTest REQUIRED)

# ============================================================================
# Unit Tests
# ============================================================================

# Core module tests
add_executable(test_core
    unit/test_types.cpp
    unit/test_memory_pool.cpp
    unit/test_ring_buffer.cpp
)
target_link_libraries(test_core PRIVATE
    opus_core
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(test_core PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_core COMMAND test_core)

# Strategy module tests  
add_executable(test_strategy
    unit/test_rsi.cpp
    unit/test_macd.cpp
    unit/test_bollinger.cpp
    unit/test_order_book_imbalance.cpp
)
target_link_libraries(test_strategy PRIVATE
    opus_strategy
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(test_strategy PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_strategy COMMAND test_strategy)

# Market module tests
add_executable(test_market
    unit/test_order_book.cpp
)
target_link_libraries(test_market PRIVATE
    opus_market
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(test_market PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_market COMMAND test_market)

# ============================================================================
# Integration Tests
# ============================================================================

add_executable(test_integration
    integration/test_trading_flow.cpp
)
target_link_libraries(test_integration PRIVATE
    opus_core
    opus_strategy
    opus_market
    opus_order
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(test_integration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_integration COMMAND test_integration)
