#pragma once
// ============================================================================
// OPUS TRADE BOT - Logger
// ============================================================================
// Async logging wrapper with minimal latency on hot path
// Uses spdlog for high-performance logging
// ============================================================================

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace opus::utils {

// ============================================================================
// Log Levels
// ============================================================================

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

// ============================================================================
// Logger Configuration
// ============================================================================

struct LogConfig {
    LogLevel level = LogLevel::Info;
    std::string log_file = "opus_trade_bot.log";
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v";

    // Performance settings
    bool async = true;              // Async logging for low latency
    size_t queue_size = 8192;       // Async queue size
    size_t flush_interval_ms = 100; // Auto-flush interval

    // File settings
    size_t max_file_size_mb = 100;
    size_t max_files = 10;
    bool rotate_on_open = false;
};

// ============================================================================
// Logger Interface
// ============================================================================

class Logger {
public:
    /// Initialize the global logger
    static void initialize(const LogConfig& config = LogConfig{});

    /// Shutdown the logger (flush and close)
    static void shutdown();

    /// Get the global logger instance
    static Logger& instance();

    /// Set log level
    void set_level(LogLevel level);

    /// Log methods
    template <typename... Args>
    void trace(std::string_view fmt, Args&&... args);

    template <typename... Args>
    void debug(std::string_view fmt, Args&&... args);

    template <typename... Args>
    void info(std::string_view fmt, Args&&... args);

    template <typename... Args>
    void warn(std::string_view fmt, Args&&... args);

    template <typename... Args>
    void error(std::string_view fmt, Args&&... args);

    template <typename... Args>
    void critical(std::string_view fmt, Args&&... args);

    /// Flush all pending logs
    void flush();

private:
    Logger() = default;
    ~Logger() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Convenience Macros
// ============================================================================

#define LOG_TRACE(...) ::opus::utils::Logger::instance().trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::opus::utils::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...) ::opus::utils::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...) ::opus::utils::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::opus::utils::Logger::instance().error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::opus::utils::Logger::instance().critical(__VA_ARGS__)

// ============================================================================
// Scoped Timer for Performance Measurement
// ============================================================================

class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name, LogLevel level = LogLevel::Debug)
        : name_(name), level_(level), start_(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        log_duration(duration);
    }

    // Non-copyable
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    void log_duration(int64_t microseconds) const;

    std::string_view name_;
    LogLevel level_;
    std::chrono::high_resolution_clock::time_point start_;
};

#define SCOPED_TIMER(name) ::opus::utils::ScopedTimer _timer_##__LINE__(name)

}  // namespace opus::utils
