// central_cache.hpp — the shared middle tier. One free list per size
// class, each guarded by its own spinlock and padded to a cache line to
// prevent false sharing between classes. Thread caches move blocks in
// and out in batches, so contention here is rare by design.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "size_classes.hpp"
#include "slab.hpp"

namespace mempool {

// 64 bytes covers x86-64 and most AArch64 parts. We pin the value rather
// than use std::hardware_destructive_interference_size because that
// constant is ABI-unstable across -mtune targets (GCC -Winterference-size).
inline constexpr std::size_t kCacheLine = 64;

// Minimal test-and-test-and-set spinlock with exponential backoff.
// Central-list critical sections are a few pointer writes, so a spinlock
// beats a mutex here (no syscall, no futex wait on the common path).
class Spinlock {
public:
    void lock() noexcept {
        int spins = 0;
        while (flag_.exchange(true, std::memory_order_acquire)) {
            while (flag_.load(std::memory_order_relaxed)) {
                if (++spins < 64) {
#if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
#endif
                } else {
                    flag_.wait(true, std::memory_order_relaxed);  // C++20
                    break;
                }
            }
        }
    }
    void unlock() noexcept {
        flag_.store(false, std::memory_order_release);
        flag_.notify_one();
    }

private:
    std::atomic<bool> flag_{false};
};

#ifdef MEMPOOL_STATS
inline constexpr bool kStatsEnabled = true;
#else
inline constexpr bool kStatsEnabled = false;
#endif

struct PoolStats {
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> deallocations{0};
    std::atomic<std::uint64_t> thread_cache_hits{0};
    std::atomic<std::uint64_t> central_refills{0};
    std::atomic<std::uint64_t> slabs_created{0};
    std::atomic<std::uint64_t> bytes_reserved{0};
    std::atomic<std::uint64_t> fallback_allocs{0};  // > kMaxPooledSize
};

class CentralCache {
public:
    // Pop up to `want` blocks for a size class into an intrusive list.
    // Returns head (linked through first word of each block) and writes
    // the count obtained. Grows by carving a new slab when empty.
    [[nodiscard]] void* fetch_batch(std::size_t cls, std::size_t want,
                                    std::size_t& got) {
        auto& shard = shards_[cls];
        std::lock_guard lock(shard.lock);

        if (shard.free_head == nullptr) {
            grow(cls);  // carve a fresh slab into shard.free_head
        }

        void* head = shard.free_head;
        void* tail = head;
        got = head ? 1 : 0;
        while (got < want && tail && next_of(tail)) {
            tail = next_of(tail);
            ++got;
        }
        if (tail) {
            shard.free_head = next_of(tail);
            set_next(tail, nullptr);
        }
        shard.free_count -= got;
        return head;
    }

    // Return a batch of blocks (intrusive list, `count` long) to a class.
    void release_batch(std::size_t cls, void* head, void* tail,
                       std::size_t count) noexcept {
        auto& shard = shards_[cls];
        std::lock_guard lock(shard.lock);
        set_next(tail, shard.free_head);
        shard.free_head = head;
        shard.free_count += count;
    }

    [[nodiscard]] PoolStats& stats() noexcept { return stats_; }

    [[nodiscard]] std::size_t free_blocks(std::size_t cls) const noexcept {
        return shards_[cls].free_count;
    }

    static void* next_of(void* p) noexcept {
        return *reinterpret_cast<void**>(p);
    }
    static void set_next(void* p, void* n) noexcept {
        *reinterpret_cast<void**>(p) = n;
    }

private:
    // Per-class shard, padded so neighbouring classes never share a line.
    struct alignas(kCacheLine) Shard {
        Spinlock lock;
        void* free_head = nullptr;
        std::size_t free_count = 0;
    };

    void grow(std::size_t cls) {
        const std::size_t block = class_to_size(cls);
        auto slab = std::make_unique<Slab>(block);
        shards_[cls].free_head = slab->carve();
        shards_[cls].free_count = slab->capacity();

        stats_.slabs_created.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_reserved.fetch_add(kSlabSize, std::memory_order_relaxed);

        std::lock_guard g(slabs_lock_);
        slabs_.push_back(std::move(slab));  // RAII ownership until pool dies
    }

    std::array<Shard, kNumClasses> shards_{};
    Spinlock slabs_lock_;
    std::vector<std::unique_ptr<Slab>> slabs_;
    PoolStats stats_;
};

}  // namespace mempool
