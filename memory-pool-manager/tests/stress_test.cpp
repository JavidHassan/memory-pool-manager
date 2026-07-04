// stress_test.cpp — hammer the pool from many threads simultaneously,
// including the hard case: blocks allocated on one thread and freed on
// another (exercises the central-cache path under contention).
// Every block is stamped with a checksum and verified before free, so
// any cross-thread corruption is caught immediately.
// Run under TSan to prove absence of data races, ASan/Valgrind for leaks.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "mempool/pool_allocator.hpp"

using namespace mempool;

namespace {

struct Block {
    void* ptr;
    std::uint32_t size;
    std::uint32_t stamp;
};

void stamp(void* p, std::uint32_t size, std::uint32_t seed) {
    auto* b = static_cast<std::uint8_t*>(p);
    for (std::uint32_t i = 0; i < size; ++i)
        b[i] = static_cast<std::uint8_t>((seed + i * 31) & 0xFF);
}

bool verify(const void* p, std::uint32_t size, std::uint32_t seed) {
    auto* b = static_cast<const std::uint8_t*>(p);
    for (std::uint32_t i = 0; i < size; ++i)
        if (b[i] != static_cast<std::uint8_t>((seed + i * 31) & 0xFF))
            return false;
    return true;
}

std::atomic<std::uint64_t> g_ops{0};
std::atomic<std::uint64_t> g_corruptions{0};

// Mailboxes for cross-thread frees: thread i posts blocks to (i+1)%N.
struct Mailbox {
    std::mutex m;
    std::vector<Block> blocks;
};

void worker(int id, int n_threads, int iterations,
            std::vector<Mailbox>& mail) {
    std::mt19937 rng(static_cast<unsigned>(id * 7919 + 13));
    std::uniform_int_distribution<std::uint32_t> size_dist(1, 1024);
    std::uniform_int_distribution<int> action(0, 99);

    auto& pool = MemoryPool::instance();
    std::vector<Block> local;
    local.reserve(512);

    for (int it = 0; it < iterations; ++it) {
        const int a = action(rng);

        if (a < 55 || local.empty()) {  // allocate
            const std::uint32_t size = size_dist(rng);
            void* p = pool.allocate(size);
            const auto seed = static_cast<std::uint32_t>(rng());
            stamp(p, size, seed);
            local.push_back({p, size, seed});
        } else if (a < 85) {  // free one of ours (random order)
            std::uniform_int_distribution<std::size_t> pick(0, local.size() - 1);
            std::size_t i = pick(rng);
            if (!verify(local[i].ptr, local[i].size, local[i].stamp))
                g_corruptions.fetch_add(1);
            pool.deallocate(local[i].ptr, local[i].size);
            local[i] = local.back();
            local.pop_back();
        } else if (a < 95) {  // post a block to the next thread to free
            std::uniform_int_distribution<std::size_t> pick(0, local.size() - 1);
            std::size_t i = pick(rng);
            Mailbox& mb = mail[(id + 1) % n_threads];
            {
                std::lock_guard g(mb.m);
                mb.blocks.push_back(local[i]);
            }
            local[i] = local.back();
            local.pop_back();
        } else {  // drain our mailbox: free blocks another thread allocated
            Mailbox& mb = mail[id];
            std::vector<Block> incoming;
            {
                std::lock_guard g(mb.m);
                incoming.swap(mb.blocks);
            }
            for (const Block& blk : incoming) {
                if (!verify(blk.ptr, blk.size, blk.stamp))
                    g_corruptions.fetch_add(1);
                pool.deallocate(blk.ptr, blk.size);
            }
        }
        g_ops.fetch_add(1, std::memory_order_relaxed);
    }

    // Cleanup: free everything still held locally and in our mailbox.
    for (const Block& blk : local) {
        if (!verify(blk.ptr, blk.size, blk.stamp)) g_corruptions.fetch_add(1);
        pool.deallocate(blk.ptr, blk.size);
    }
    Mailbox& mb = mail[id];
    std::lock_guard g(mb.m);
    for (const Block& blk : mb.blocks) {
        if (!verify(blk.ptr, blk.size, blk.stamp)) g_corruptions.fetch_add(1);
        pool.deallocate(blk.ptr, blk.size);
    }
    mb.blocks.clear();
}

}  // namespace

int main(int argc, char** argv) {
    const int n_threads = argc > 1 ? std::atoi(argv[1])
                                   : static_cast<int>(
                                         std::thread::hardware_concurrency());
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 200'000;

    std::vector<Mailbox> mail(static_cast<std::size_t>(n_threads));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n_threads));

    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i, n_threads, iterations, std::ref(mail));
    for (auto& t : threads) t.join();

    // Final sweep: a worker may post to a neighbour's mailbox after that
    // neighbour has already drained and exited — collect the stragglers.
    auto& pool = MemoryPool::instance();
    for (auto& mb : mail) {
        std::lock_guard g(mb.m);
        for (const Block& blk : mb.blocks) {
            if (!verify(blk.ptr, blk.size, blk.stamp)) g_corruptions.fetch_add(1);
            pool.deallocate(blk.ptr, blk.size);
        }
        mb.blocks.clear();
    }

    auto& st = MemoryPool::instance().stats();
    const bool balanced = st.allocations.load() == st.deallocations.load();
    const bool clean = g_corruptions.load() == 0;

    std::cout << "threads:        " << n_threads << "\n"
              << "total ops:      " << g_ops.load() << "\n"
              << "allocations:    " << st.allocations.load() << "\n"
              << "deallocations:  " << st.deallocations.load() << "\n"
              << "corruptions:    " << g_corruptions.load() << "\n"
              << "slabs created:  " << st.slabs_created.load() << "\n"
              << "tcache hit rate: "
              << (100.0 * static_cast<double>(st.thread_cache_hits.load()) /
                  static_cast<double>(
                      std::max<std::uint64_t>(1, st.allocations.load())))
              << "%\n"
              << (balanced && clean ? "STRESS PASS" : "STRESS FAIL") << "\n";

    return balanced && clean ? 0 : 1;
}
