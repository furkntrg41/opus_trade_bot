#pragma once
// ============================================================================
// OPUS TRADE BOT - Memory Pool (Arena Allocator)
// ============================================================================
// Zero runtime allocation memory management
// Pre-allocate all memory at startup, no malloc/new during trading
// ============================================================================

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace opus {

// ============================================================================
// Arena Allocator
// ============================================================================
// Simple bump allocator for fast, sequential allocations
// Memory is freed all at once when arena is reset/destroyed

class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t capacity)
        : buffer_(std::make_unique<std::byte[]>(capacity))
        , capacity_(capacity)
        , offset_(0) {}

    // Non-copyable
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    // Movable
    ArenaAllocator(ArenaAllocator&&) noexcept = default;
    ArenaAllocator& operator=(ArenaAllocator&&) noexcept = default;

    /// Allocate memory with alignment
    template <typename T = std::byte>
    [[nodiscard]] T* allocate(size_t count = 1) {
        constexpr size_t alignment = alignof(T);
        const size_t size = sizeof(T) * count;

        // Align offset
        const size_t aligned_offset = align_up(offset_, alignment);
        const size_t new_offset = aligned_offset + size;

        if (new_offset > capacity_) {
            return nullptr;  // Out of memory
        }

        T* ptr = reinterpret_cast<T*>(buffer_.get() + aligned_offset);
        offset_ = new_offset;
        return ptr;
    }

    /// Allocate and construct an object
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        T* ptr = allocate<T>(1);
        if (ptr) {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    /// Reset arena (invalidates all allocations)
    void reset() noexcept { offset_ = 0; }

    /// Get remaining capacity
    [[nodiscard]] size_t remaining() const noexcept { return capacity_ - offset_; }

    /// Get total capacity
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    /// Get used memory
    [[nodiscard]] size_t used() const noexcept { return offset_; }

private:
    static constexpr size_t align_up(size_t value, size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::unique_ptr<std::byte[]> buffer_;
    size_t capacity_;
    size_t offset_;
};

// ============================================================================
// Pool Allocator
// ============================================================================
// Fixed-size block allocator with O(1) alloc/free
// Uses a freelist for efficient memory reuse

template <typename T, size_t BlockCount>
class PoolAllocator {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    PoolAllocator() : free_list_(nullptr), allocated_count_(0) {
        // Initialize freelist
        for (size_t i = 0; i < BlockCount; ++i) {
            auto* block = reinterpret_cast<FreeBlock*>(&storage_[i]);
            block->next = free_list_;
            free_list_ = block;
        }
    }

    // Non-copyable, non-movable (pointers into storage_)
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    ~PoolAllocator() {
        // Destructor doesn't call destructors on allocated objects
        // User must ensure all objects are deallocated
    }

    /// Allocate a single object
    [[nodiscard]] T* allocate() {
        if (!free_list_) {
            return nullptr;  // Pool exhausted
        }
        FreeBlock* block = free_list_;
        free_list_ = block->next;
        ++allocated_count_;
        return reinterpret_cast<T*>(block);
    }

    /// Deallocate an object (does NOT call destructor)
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        auto* block = reinterpret_cast<FreeBlock*>(ptr);
        block->next = free_list_;
        free_list_ = block;
        --allocated_count_;
    }

    /// Allocate and construct
    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    /// Destruct and deallocate
    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    /// Get number of allocated objects
    [[nodiscard]] size_t allocated() const noexcept { return allocated_count_; }

    /// Get maximum capacity
    [[nodiscard]] constexpr size_t capacity() const noexcept { return BlockCount; }

    /// Get remaining capacity
    [[nodiscard]] size_t remaining() const noexcept { return BlockCount - allocated_count_; }

private:
    struct FreeBlock {
        FreeBlock* next;
    };

    // Aligned storage for objects
    alignas(T) std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, BlockCount> storage_;
    FreeBlock* free_list_;
    size_t allocated_count_;
};

// ============================================================================
// Thread-Local Pool
// ============================================================================
// Each thread gets its own pool to avoid locking

template <typename T, size_t BlockCount>
class ThreadLocalPool {
public:
    /// Get the thread-local pool instance
    static PoolAllocator<T, BlockCount>& instance() {
        thread_local PoolAllocator<T, BlockCount> pool;
        return pool;
    }

    /// Convenience methods
    [[nodiscard]] static T* allocate() { return instance().allocate(); }
    static void deallocate(T* ptr) noexcept { instance().deallocate(ptr); }

    template <typename... Args>
    [[nodiscard]] static T* create(Args&&... args) {
        return instance().create(std::forward<Args>(args)...);
    }

    static void destroy(T* ptr) noexcept { instance().destroy(ptr); }
};

// ============================================================================
// Lock-Free Pool Allocator
// ============================================================================
// Thread-safe pool using atomic operations (CAS)

template <typename T, size_t BlockCount>
class LockFreePoolAllocator {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    LockFreePoolAllocator() : free_head_(nullptr), allocated_count_(0) {
        // Initialize freelist
        for (size_t i = 0; i < BlockCount; ++i) {
            auto* block = reinterpret_cast<Node*>(&storage_[i]);
            push_free(block);
        }
    }

    // Non-copyable, non-movable
    LockFreePoolAllocator(const LockFreePoolAllocator&) = delete;
    LockFreePoolAllocator& operator=(const LockFreePoolAllocator&) = delete;

    [[nodiscard]] T* allocate() {
        Node* old_head = free_head_.load(std::memory_order_acquire);

        while (old_head) {
            Node* new_head = old_head->next;
            if (free_head_.compare_exchange_weak(old_head, new_head,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire)) {
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                return reinterpret_cast<T*>(old_head);
            }
            // CAS failed, old_head is updated, retry
        }
        return nullptr;  // Pool exhausted
    }

    void deallocate(T* ptr) noexcept {
        if (!ptr) return;
        auto* node = reinterpret_cast<Node*>(ptr);
        push_free(node);
        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    [[nodiscard]] size_t allocated() const noexcept {
        return allocated_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept { return BlockCount; }

private:
    struct Node {
        Node* next;
    };

    void push_free(Node* node) noexcept {
        Node* old_head = free_head_.load(std::memory_order_acquire);
        do {
            node->next = old_head;
        } while (!free_head_.compare_exchange_weak(old_head, node,
                                                    std::memory_order_release,
                                                    std::memory_order_acquire));
    }

    alignas(T) std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, BlockCount> storage_;
    std::atomic<Node*> free_head_;
    std::atomic<size_t> allocated_count_;
};

}  // namespace opus
