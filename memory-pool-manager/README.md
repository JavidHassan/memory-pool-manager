# High-Performance Memory Pool Manager

![CI](https://github.com/JavidHassan/memory-pool-manager/actions/workflows/ci.yml/badge.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-MIT-green)

A tcmalloc-inspired slab allocator in modern C++20: three-tier design with
lock-free thread-local fast paths, spinlock-sharded central caches, and
RAII-owned slab backing. **4.5× faster than glibc malloc** on fixed-size
workloads, verified leak-free under Valgrind and race-free under
ThreadSanitizer.

## Measured Results

Median of 9 repetitions, 1M ops per rep, GCC 13.3 `-O2`, glibc 2.39:

| Scenario | malloc | this pool | Speedup |
|----------|-------:|----------:|--------:|
| Fixed 64 B alloc/free | 14.4 ns/op | **3.2 ns/op** | **4.5×** |
| Mixed 8–1024 B alloc/free | 20.6 ns/op | **10.2 ns/op** | **2.0×** |
| LIFO burst (500 × 128 B) | 20.6 ns/op | **4.4 ns/op** | **4.7×** |

Steady-state thread-cache hit rate: **~99%** (measured in stress runs).

### Verification

| Gate | Result |
|------|--------|
| Unit tests | 199,059 checks, 0 failures |
| 8-thread stress (1.6M ops, cross-thread frees, checksummed blocks) | 0 corruptions, alloc/free balanced |
| ThreadSanitizer | clean |
| AddressSanitizer + UBSan | clean |
| Valgrind (`--leak-check=full`) | **0 bytes in use at exit, 0 errors** |

## Architecture

```
 allocate(size) ──► ThreadCache (thread_local free lists)   ← no locks/atomics
                        │ miss: batch of 32
                        ▼
                    CentralCache (per-class spinlock shards,
                        │          cache-line padded)
                        ▼ empty: carve
                    Slab backend (64 KiB aligned, RAII-owned)
```

- **Zero per-block metadata** — sized deallocation (the `std::pmr` /
  `std::allocator` contract) means every pooled byte is payload.
- **Intrusive free lists** — free blocks link through their own bytes.
- **Batched refills** amortize one spinlock acquisition over 32 allocations.
- **Compile-time instrumentation** — stats are `if constexpr`-gated;
  release builds pay zero cost.

Full rationale and trade-offs: [docs/DESIGN.md](docs/DESIGN.md).

## Modern C++20 Usage

```cpp
#include "mempool/adapters.hpp"

// 1. Drop-in std::allocator replacement
std::vector<int, mempool::PoolAllocator<int>> v;

// 2. Polymorphic memory resource
mempool::PoolResource res;
std::pmr::vector<double> pv{&res};
std::pmr::string s{&res};

// 3. Raw sized API
auto& pool = mempool::MemoryPool::instance();
void* p = pool.allocate(256);
pool.deallocate(p, 256);
```

C++20 features used: concepts (`PoolLike`), `consteval` size-class table
with `static_assert` proofs, `[[likely]]`/`[[unlikely]]`,
`std::atomic::wait/notify`, `hardware_destructive_interference_size`.

## Build & Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/unit_tests           # 199k correctness checks
./build/stress_test 8 200000 # 8 threads, cross-thread frees
./build/benchmark            # pool vs malloc

# Sanitizer builds
cmake -B build-tsan -DSANITIZE=thread && cmake --build build-tsan -j
./build-tsan/stress_test 8 50000

# Zero-leak proof
valgrind --leak-check=full ./build/unit_tests
```

Requirements: C++20 compiler (GCC 12+/Clang 15+), CMake ≥ 3.20. The
library itself is header-only with zero dependencies.

## Project Structure

```
include/mempool/
├── size_classes.hpp   consteval size-class table + O(1) lookup
├── slab.hpp           RAII slab: 64 KiB aligned, carved into blocks
├── central_cache.hpp  sharded central free lists, spinlock, stats
├── pool_allocator.hpp ThreadCache + MemoryPool facade
└── adapters.hpp       std::allocator + std::pmr adapters, concepts
tests/                 unit tests + multithreaded stress test
bench/                 benchmark harness vs malloc
docs/DESIGN.md         architecture decisions and trade-offs
.github/workflows/     CI: Release + ASan/UBSan + TSan + Valgrind matrix
```

## The Performance Story

The first working version benchmarked **slower than malloc** (24 ns vs
14 ns). Profiling the hot path revealed the culprit: atomic statistics
counters on every allocate/deallocate — four contended cache-line writes
per pair. Moving instrumentation behind a compile-time flag
(`MEMPOOL_STATS`, enabled for tests, off for release) dropped the hot
path to 3.2 ns — a 7.5× improvement from a one-design-decision fix, and
a concrete lesson in how observability can silently dominate a fast path.

## License

MIT
