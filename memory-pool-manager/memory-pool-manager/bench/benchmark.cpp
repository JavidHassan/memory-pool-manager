// benchmark.cpp — pool vs malloc/operator new, honest methodology:
// warmup pass, N repetitions, report the MEDIAN ns/op (robust to noise).
// Scenarios:
//   1. fixed-size hot loop (the pool's best case)
//   2. mixed random sizes 8..1024 B
//   3. LIFO burst (alloc K, free K)
//   4. multithreaded contention (all threads allocating simultaneously)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "mempool/pool_allocator.hpp"

using namespace mempool;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kReps = 9;  // odd → clean median

template <typename F>
double median_ns_per_op(F&& body, std::uint64_t ops_per_rep) {
    std::vector<double> samples;
    samples.reserve(kReps);
    body();  // warmup (populates caches / page-faults slabs in)
    for (int r = 0; r < kReps; ++r) {
        const auto t0 = Clock::now();
        body();
        const auto t1 = Clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns / static_cast<double>(ops_per_rep));
    }
    std::nth_element(samples.begin(), samples.begin() + kReps / 2,
                     samples.end());
    return samples[kReps / 2];
}

// Prevent the optimizer from deleting alloc/free pairs.
inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }

// ── scenario 1: fixed 64-byte hot loop ────────────────────────────
template <bool UsePool>
void fixed_size_loop(int n) {
    auto& pool = MemoryPool::instance();
    for (int i = 0; i < n; ++i) {
        void* p = UsePool ? pool.allocate(64) : std::malloc(64);
        escape(p);
        if constexpr (UsePool) pool.deallocate(p, 64); else std::free(p);
    }
}

// ── scenario 2: mixed random sizes ────────────────────────────────
template <bool UsePool>
void mixed_size_loop(int n) {
    auto& pool = MemoryPool::instance();
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> d(8, 1024);
    for (int i = 0; i < n; ++i) {
        const std::size_t s = d(rng);
        void* p = UsePool ? pool.allocate(s) : std::malloc(s);
        escape(p);
        if constexpr (UsePool) pool.deallocate(p, s); else std::free(p);
    }
}

// ── scenario 3: LIFO burst — alloc K then free K ──────────────────
template <bool UsePool>
void burst_loop(int bursts, int k) {
    auto& pool = MemoryPool::instance();
    std::vector<void*> held(static_cast<std::size_t>(k));
    for (int b = 0; b < bursts; ++b) {
        for (int i = 0; i < k; ++i)
            held[static_cast<std::size_t>(i)] =
                UsePool ? pool.allocate(128) : std::malloc(128);
        for (int i = k - 1; i >= 0; --i) {
            if constexpr (UsePool)
                pool.deallocate(held[static_cast<std::size_t>(i)], 128);
            else
                std::free(held[static_cast<std::size_t>(i)]);
        }
    }
}

// ── scenario 4: multithreaded fixed-size ──────────────────────────
template <bool UsePool>
void mt_loop(int n_threads, int per_thread) {
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(n_threads));
    for (int t = 0; t < n_threads; ++t)
        ts.emplace_back(fixed_size_loop<UsePool>, per_thread);
    for (auto& t : ts) t.join();
}

void report(const char* name, double malloc_ns, double pool_ns) {
    std::printf("%-34s  malloc: %7.1f ns/op   pool: %7.1f ns/op   speedup: %.2fx\n",
                name, malloc_ns, pool_ns, malloc_ns / pool_ns);
}

}  // namespace

int main() {
    constexpr int N = 1'000'000;
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int threads = std::min(hw > 0 ? hw : 4, 8);

    std::printf("== Memory Pool Benchmark (median of %d reps) ==\n\n", kReps);

    {
        const double m = median_ns_per_op([&] { fixed_size_loop<false>(N); }, N);
        const double p = median_ns_per_op([&] { fixed_size_loop<true>(N); }, N);
        report("fixed 64B alloc/free", m, p);
    }
    {
        const double m = median_ns_per_op([&] { mixed_size_loop<false>(N); }, N);
        const double p = median_ns_per_op([&] { mixed_size_loop<true>(N); }, N);
        report("mixed 8-1024B alloc/free", m, p);
    }
    {
        constexpr int kBursts = 2'000, kK = 500;
        constexpr std::uint64_t ops = 2ULL * kBursts * kK;
        const double m =
            median_ns_per_op([&] { burst_loop<false>(kBursts, kK); }, ops);
        const double p =
            median_ns_per_op([&] { burst_loop<true>(kBursts, kK); }, ops);
        report("LIFO burst 500 x 128B", m, p);
    }
    {
        const int per = N / threads;
        const std::uint64_t ops = static_cast<std::uint64_t>(per) * threads;
        const double m =
            median_ns_per_op([&] { mt_loop<false>(threads, per); }, ops);
        const double p =
            median_ns_per_op([&] { mt_loop<true>(threads, per); }, ops);
        char label[64];
        std::snprintf(label, sizeof(label), "%d-thread fixed 64B", threads);
        report(label, m, p);
    }

    const auto& st = MemoryPool::instance().stats();
    std::printf("\nslabs created: %llu   memory reserved: %llu KiB\n",
                static_cast<unsigned long long>(st.slabs_created.load()),
                static_cast<unsigned long long>(st.bytes_reserved.load() / 1024));
    if constexpr (kStatsEnabled) {
        std::printf("thread-cache hit rate: %.2f%%\n",
                    100.0 * st.thread_cache_hits.load() /
                        std::max<std::uint64_t>(1, st.allocations.load()));
    }
    return 0;
}
