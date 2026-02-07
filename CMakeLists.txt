cmake_minimum_required(VERSION 3.20)

# ============================================================================
# OPUS TRADE BOT - Professional C++20 Algorithmic Trading System
# ============================================================================
# Wall Street-grade trading bot for Binance Futures
# Following SOLID principles and HFT best practices
# ============================================================================

project(opus_trade_bot
    VERSION 1.0.0
    DESCRIPTION "Professional C++20 Algorithmic Trading Bot for Binance Futures"
    LANGUAGES CXX
)

# ============================================================================
# C++ Standard and Compiler Settings
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type configuration
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# ============================================================================
# Compiler-specific flags for maximum performance
# ============================================================================
if(MSVC)
    # MSVC optimizations
    add_compile_options(
        /W4                     # High warning level
        /permissive-            # Strict conformance
        /Zc:__cplusplus         # Correct __cplusplus macro
        /utf-8                  # UTF-8 source and execution charset
        $<$<CONFIG:Release>:/O2>        # Maximum optimization
        $<$<CONFIG:Release>:/GL>        # Whole program optimization
        $<$<CONFIG:Release>:/DNDEBUG>   # No debug
    )
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>      # Link-time code generation
    )
else()
    # GCC/Clang optimizations
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>  # CPU-specific optimizations
        $<$<CONFIG:Release>:-flto>          # Link-time optimization
        $<$<CONFIG:Release>:-DNDEBUG>
    )
    add_link_options(
        $<$<CONFIG:Release>:-flto>
    )
endif()

# ============================================================================
# vcpkg Integration (if available)
# ============================================================================
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

# ============================================================================
# Find Dependencies
# ============================================================================

# Boost (Asio, Beast, System)
find_package(Boost 1.80 COMPONENTS system QUIET)
if(Boost_FOUND)
    message(STATUS "Found Boost: ${Boost_VERSION}")
else()
    message(WARNING "Boost not found - networking features will be disabled")
endif()

# simdjson (High-performance JSON parser)
find_package(simdjson QUIET)
if(simdjson_FOUND)
    message(STATUS "Found simdjson")
else()
    message(WARNING "simdjson not found - using fallback JSON parser")
endif()

# spdlog (Fast async logging)
find_package(spdlog QUIET)
if(spdlog_FOUND)
    message(STATUS "Found spdlog")
else()
    message(WARNING "spdlog not found - using basic logging")
endif()

# Abseil (FlatHashMap, other utilities)
find_package(absl QUIET)
if(absl_FOUND)
    message(STATUS "Found Abseil")
else()
    message(WARNING "Abseil not found - using std::unordered_map fallback")
endif()

# OpenSSL (for HMAC-SHA256 authentication)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
else()
    message(WARNING "OpenSSL not found - exchange authentication disabled")
endif()

# yaml-cpp (for configuration file parsing)
find_package(yaml-cpp CONFIG QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "Found yaml-cpp")
else()
    message(WARNING "yaml-cpp not found - config loading disabled")
endif()

# ============================================================================
# Library Definitions
# ============================================================================

# Core library (Memory, Ring Buffer, Event Loop)
add_library(opus_core STATIC
    src/core/memory_pool.cpp
    src/core/ring_buffer.cpp
    src/core/event_loop.cpp
)
target_include_directories(opus_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(Boost_FOUND)
    target_link_libraries(opus_core PUBLIC Boost::system)
endif()
if(spdlog_FOUND)
    target_link_libraries(opus_core PUBLIC spdlog::spdlog)
endif()

# Network library (WebSocket, REST client)
add_library(opus_network STATIC
    src/network/websocket_client.cpp
    src/network/rest_client.cpp
    src/network/rate_limiter.cpp
)
target_link_libraries(opus_network PUBLIC opus_core)
if(Boost_FOUND)
    target_link_libraries(opus_network PUBLIC Boost::system)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(opus_network PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Exchange library (Binance integration)
add_library(opus_exchange STATIC
    src/exchange/binance/client.cpp
    src/exchange/binance/auth.cpp
    src/exchange/binance/market_data.cpp
)
target_link_libraries(opus_exchange PUBLIC opus_network)
if(simdjson_FOUND)
    target_link_libraries(opus_exchange PUBLIC simdjson::simdjson)
endif()

# Market data library
add_library(opus_market STATIC
    src/market/order_book.cpp
    src/market/kline.cpp
    src/market/ticker.cpp
    src/market/binary_writer.cpp
)
target_link_libraries(opus_market PUBLIC opus_core)

# Strategy library (Indicators, Signals)
add_library(opus_strategy STATIC
    src/strategy/indicators/rsi.cpp
    src/strategy/indicators/macd.cpp
    src/strategy/indicators/bollinger.cpp
    src/strategy/indicators/ema.cpp
    src/strategy/order_book_imbalance.cpp
    src/strategy/signal_generator.cpp
)
target_link_libraries(opus_strategy PUBLIC opus_market)

# Order management library
add_library(opus_order STATIC
    src/order/order_manager.cpp
    src/order/position_tracker.cpp
    src/order/risk_manager.cpp
)
target_link_libraries(opus_order PUBLIC opus_strategy opus_exchange)

# ============================================================================
# Main Executable
# ============================================================================
add_executable(opus_bot src/main.cpp)
target_link_libraries(opus_bot PRIVATE
    opus_core
    opus_network
    opus_exchange
    opus_market
    opus_strategy
    opus_order
)
if(yaml-cpp_FOUND)
    target_link_libraries(opus_bot PRIVATE yaml-cpp::yaml-cpp)
endif()

# ============================================================================
# Testing (Google Test)
# ============================================================================
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "Found Google Test - tests enabled")
        add_subdirectory(tests)
    else()
        message(WARNING "Google Test not found - tests disabled")
    endif()
endif()

# ============================================================================
# Install Configuration
# ============================================================================
install(TARGETS opus_bot
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/opus
    DESTINATION include
)
install(DIRECTORY config/
    DESTINATION etc/opus
)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== OPUS TRADE BOT Configuration ===")
message(STATUS "Version:        ${PROJECT_VERSION}")
message(STATUS "Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "Boost:          ${Boost_FOUND}")
message(STATUS "simdjson:       ${simdjson_FOUND}")
message(STATUS "spdlog:         ${spdlog_FOUND}")
message(STATUS "Abseil:         ${absl_FOUND}")
message(STATUS "OpenSSL:        ${OpenSSL_FOUND}")
message(STATUS "Tests:          ${BUILD_TESTS}")
message(STATUS "=====================================")
