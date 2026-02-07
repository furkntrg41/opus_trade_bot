#pragma once
// ============================================================================
// OPUS TRADE BOT - Lock-Free Ring Buffer (LMAX Disruptor Pattern)
// ============================================================================
// High-performance inter-thread messaging without locks
// Single-Producer Single-Consumer (SPSC) for maximum throughput
// ============================================================================

#include <atomic>
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

// ============================================================================
// Platform-specific CPU hints (must be before usage)
// ============================================================================

#if defined(_MSC_VER)
    #include <intrin.h>
    // _mm_pause() is already defined by intrin.h on MSVC
#elif defined(__x86_64__) || defined(__i386__)
    #include <immintrin.h>
#else
    // Fallback for non-x86
    inline void _mm_pause() noexcept {}
#endif

namespace opus {

// ============================================================================
// Cache Line Constants
// ============================================================================

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    inline constexpr size_t CACHE_LINE_SIZE = 64;  // Common x86-64 cache line size
#endif

// ============================================================================
// Concept for Ring Buffer Elements
// ============================================================================

template <typename T>
concept RingBufferElement = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// ============================================================================
// SPSC Ring Buffer (Single Producer, Single Consumer)
// ============================================================================
// Lock-free, wait-free ring buffer optimized for:
// - Cache line separation between producer and consumer
// - Power-of-2 size for fast modulo (bitwise AND)
// - Memory barriers for correct ordering

template <RingBufferElement T, size_t Capacity>
class SPSCRingBuffer {
public:
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

    static constexpr size_t MASK = Capacity - 1;

    SPSCRingBuffer() : head_(0), tail_(0) {
        // Zero-initialize buffer
        for (auto& slot : buffer_) {
            slot = T{};
        }
    }

    // Non-copyable, non-movable (atomic members)
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /// Try to push an element (producer thread only)
    /// Returns true if successful, false if buffer is full
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & MASK;

        // Check if buffer is full
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }

        buffer_[current_head] = item;

        // Publish the write
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /// Push an element, spinning if buffer is full
    void push(const T& item) noexcept {
        while (!try_push(item)) {
            // Spin-wait (could add backoff strategy)
            _mm_pause();  // x86 hint to CPU
        }
    }

    /// Try to pop an element (consumer thread only)
    /// Returns the element if available, nullopt if buffer is empty
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if buffer is empty
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Buffer empty
        }

        T item = buffer_[current_tail];

        // Advance tail
        tail_.store((current_tail + 1) & MASK, std::memory_order_release);
        return item;
    }

    /// Pop an element, spinning if buffer is empty
    [[nodiscard]] T pop() noexcept {
        std::optional<T> result;
        while (!(result = try_pop())) {
            _mm_pause();
        }
        return *result;
    }

    /// Peek at the front element without removing it
    [[nodiscard]] std::optional<T> peek() const noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        return buffer_[current_tail];
    }

    /// Check if buffer is empty
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// Check if buffer is full
    [[nodiscard]] bool full() const noexcept {
        const size_t next_head = (head_.load(std::memory_order_acquire) + 1) & MASK;
        return next_head == tail_.load(std::memory_order_acquire);
    }

    /// Get current size (approximate, may be stale)
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & MASK;
    }

    /// Get capacity
    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity - 1; }

    /// Clear all elements (NOT thread-safe, use only when single-threaded)
    void clear() noexcept {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    // Ensure head and tail are on different cache lines to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;

    // Buffer storage
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_;
};

// ============================================================================
// MPSC Ring Buffer (Multiple Producer, Single Consumer)
// ============================================================================
// Uses CAS for thread-safe multi-producer push

template <RingBufferElement T, size_t Capacity>
class MPSCRingBuffer {
public:
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of 2");

    static constexpr size_t MASK = Capacity - 1;

    MPSCRingBuffer() : head_(0), tail_(0) {}

    // Non-copyable
    MPSCRingBuffer(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer& operator=(const MPSCRingBuffer&) = delete;

    /// Try to push (multiple producers allowed)
    [[nodiscard]] bool try_push(const T& item) noexcept {
        size_t current_head = head_.load(std::memory_order_relaxed);

        while (true) {
            const size_t next_head = (current_head + 1) & MASK;

            // Check if full
            if (next_head == tail_.load(std::memory_order_acquire)) {
                return false;
            }

            // Try to claim the slot
            if (head_.compare_exchange_weak(current_head, next_head,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                buffer_[current_head] = item;
                return true;
            }
            // CAS failed, current_head was updated, retry
        }
    }

    /// Try to pop (single consumer only)
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        T item = buffer_[current_tail];
        tail_.store((current_tail + 1) & MASK, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & MASK;
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity - 1; }

private:
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_;
};

// ============================================================================
// Sequence Barrier (LMAX Disruptor Pattern)
// ============================================================================
// For complex multi-stage pipelines

class SequenceBarrier {
public:
    SequenceBarrier() : sequence_(0) {}

    /// Publish a sequence number
    void publish(int64_t sequence) noexcept {
        sequence_.store(sequence, std::memory_order_release);
    }

    /// Get the current published sequence
    [[nodiscard]] int64_t get() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

    /// Wait until a sequence is published (busy-wait)
    void wait_for(int64_t sequence) const noexcept {
        while (get() < sequence) {
            _mm_pause();
        }
    }

private:
    alignas(CACHE_LINE_SIZE) std::atomic<int64_t> sequence_;
};

}  // namespace opus

