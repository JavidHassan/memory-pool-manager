// pool_allocator.hpp — the public MemoryPool facade plus the per-thread
// cache tier. Hot path (thread-local hit) touches no shared state at all:
// no locks, no atomics, no cache-line ping-pong.
#pragma once

#include <cstddef>
#include <new>

#include "central_cache.hpp"
#include "size_classes.hpp"

namespace mempool {

class MemoryPool;

// Per-thread free lists. On miss, refills a batch from the central cache;
// when a list grows past the watermark, returns a batch back. Flushes
// everything to central on thread exit (RAII via thread_local dtor).
class ThreadCache {
public:
    explicit ThreadCache(CentralCache& central) noexcept : central_(central) {}

    ~ThreadCache() { flush_all(); }

    [[nodiscard]] void* allocate(std::size_t cls) {
        FreeList& list = lists_[cls];
        if (void* p = list.head; p != nullptr) [[likely]] {
            list.head = CentralCache::next_of(p);
            --list.count;
            if constexpr (kStatsEnabled)
                central_.stats().thread_cache_hits.fetch_add(
                    1, std::memory_order_relaxed);
            return p;
        }
        return refill(cls);
    }

    void deallocate(std::size_t cls, void* p) noexcept {
        FreeList& list = lists_[cls];
        CentralCache::set_next(p, list.head);
        list.head = p;
        if (++list.count > kReturnWatermark) [[unlikely]] {
            return_batch(cls);
        }
    }

    void flush_all() noexcept {
        for (std::size_t cls = 0; cls < kNumClasses; ++cls) {
            FreeList& list = lists_[cls];
            if (list.head) {
                void* tail = list.head;
                while (CentralCache::next_of(tail)) tail = CentralCache::next_of(tail);
                central_.release_batch(cls, list.head, tail, list.count);
                list.head = nullptr;
                list.count = 0;
            }
        }
    }

    static constexpr std::size_t kBatchSize = 32;
    static constexpr std::size_t kReturnWatermark = 256;

private:
    struct FreeList {
        void* head = nullptr;
        std::size_t count = 0;
    };

    [[nodiscard]] void* refill(std::size_t cls) {
        if constexpr (kStatsEnabled)
            central_.stats().central_refills.fetch_add(1,
                                                       std::memory_order_relaxed);
        std::size_t got = 0;
        void* head = central_.fetch_batch(cls, kBatchSize, got);
        if (head == nullptr) throw std::bad_alloc{};

        FreeList& list = lists_[cls];
        list.head = CentralCache::next_of(head);  // keep first for caller
        list.count = got - 1;
        return head;
    }

    void return_batch(std::size_t cls) noexcept {
        FreeList& list = lists_[cls];
        // Detach half the list and hand it back to central.
        std::size_t keep = list.count / 2;
        void* tail = list.head;
        for (std::size_t i = 1; i < keep; ++i) tail = CentralCache::next_of(tail);

        void* give_head = CentralCache::next_of(tail);
        CentralCache::set_next(tail, nullptr);

        void* give_tail = give_head;
        std::size_t give_count = list.count - keep;
        while (CentralCache::next_of(give_tail))
            give_tail = CentralCache::next_of(give_tail);

        central_.release_batch(cls, give_head, give_tail, give_count);
        list.count = keep;
    }

    std::array<FreeList, kNumClasses> lists_{};
    CentralCache& central_;
};

// Facade. Sized API (deallocate takes the original size) means blocks
// carry zero metadata — 100% of pooled bytes are usable payload.
class MemoryPool {
public:
    static MemoryPool& instance() {
        static MemoryPool pool;
        return pool;
    }

    [[nodiscard]] void* allocate(std::size_t size) {
        if constexpr (kStatsEnabled)
            stats().allocations.fetch_add(1, std::memory_order_relaxed);
        if (!is_pooled(size)) [[unlikely]] {
            if constexpr (kStatsEnabled)
                stats().fallback_allocs.fetch_add(1, std::memory_order_relaxed);
            return ::operator new(size);
        }
        return local_cache().allocate(size_to_class(size));
    }

    void deallocate(void* p, std::size_t size) noexcept {
        if constexpr (kStatsEnabled)
            stats().deallocations.fetch_add(1, std::memory_order_relaxed);
        if (!is_pooled(size)) [[unlikely]] {
            ::operator delete(p);
            return;
        }
        local_cache().deallocate(size_to_class(size), p);
    }

    [[nodiscard]] PoolStats& stats() noexcept { return central_.stats(); }
    [[nodiscard]] CentralCache& central() noexcept { return central_; }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

private:
    MemoryPool() = default;

    [[nodiscard]] ThreadCache& local_cache() {
        thread_local ThreadCache cache{central_};
        return cache;
    }

    CentralCache central_;
};

}  // namespace mempool
