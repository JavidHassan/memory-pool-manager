# Design

## Problem

General-purpose `malloc` must handle every allocation pattern, so it pays
costs a specialized allocator can avoid: size-bin search, arena locking,
metadata headers on every block, and fragmentation from mixed lifetimes.
Workloads dominated by small, fixed-ish sizes (nodes, messages, handles)
can do dramatically better with slab allocation.

## Architecture

```
        allocate(size)                       deallocate(ptr, size)
              │                                       │
              ▼                                       ▼
┌───────────────────────────  Tier 1  ───────────────────────────────┐
│ ThreadCache (thread_local) — one intrusive free list per class.    │
│ Hot path: pop/push head. No locks, no atomics, no sharing.         │
└───────────────┬─────────────────────────────────────▲──────────────┘
        miss → fetch_batch(32)              watermark → release_batch
┌───────────────▼─────────────────────────────────────┴──────────────┐
│ CentralCache (Tier 2) — per-class shard: spinlock + free list,     │
│ each shard alignas(cache line) to prevent false sharing.           │
└───────────────┬─────────────────────────────────────────────────────┘
        empty → carve new slab
┌───────────────▼─────────────────────────────────────────────────────┐
│ Slab backend (Tier 3) — 64 KiB aligned regions, RAII-owned,        │
│ carved once into equal blocks linked through their own bytes.      │
└──────────────────────────────────────────────────────────────────────┘
```

## Key decisions

**Sized deallocation, zero headers.** `deallocate(ptr, size)` mirrors the
`std::pmr` and `std::allocator` contracts, which always know the size.
Blocks therefore carry no metadata: a 64-byte request consumes exactly
64 bytes of slab. malloc typically burns 8–16 bytes per block on headers.

**Intrusive free lists.** A free block stores the next-pointer in its own
first 8 bytes — the list costs zero extra memory and stays cache-friendly.

**Batching amortizes synchronization.** A thread-cache miss pulls 32
blocks in one locked operation, so the spinlock is touched once per 32
allocations in steady state (measured hit rate: ~99%).

**Per-shard locks + cache-line padding.** Different size classes never
contend, and `alignas(hardware_destructive_interference_size)` stops
neighbouring shards from false-sharing a line.

**Spinlock over mutex for the central tier.** Critical sections are a few
pointer writes; a TTAS spinlock with x86 `pause` backoff and a C++20
`atomic::wait` fallback avoids futex syscalls on the common path.

**Compile-time instrumentation.** Statistics are `if constexpr`-gated
behind `MEMPOOL_STATS`. The first implementation updated atomic counters
on every operation and benchmarked *slower than malloc* (24 ns vs 14 ns);
removing them from the hot path brought it to 3.2 ns. Tests build with
stats on; release/benchmark builds pay nothing.

**Fragmentation bounds.** Geometric size classes cap internal
fragmentation below ~34% worst-case; slab carving eliminates external
fragmentation entirely within a class (blocks are interchangeable).

## Size class table

`8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024`

Requests map to classes via a `consteval`-built lookup table — O(1), no
branching, verified by `static_assert` at compile time. Sizes above
1024 B fall back to `::operator new` (tracked when stats are enabled).

## Thread lifecycle

`ThreadCache` is a `thread_local` with a destructor that flushes all
cached blocks back to the central tier — a thread can allocate, exit,
and its cached memory is reclaimed for other threads (verified by the
alloc == dealloc balance check in the stress test).

## Verification strategy

| Tool | What it proves |
|------|----------------|
| Unit tests (199k checks) | correctness, reuse, adapters, integrity |
| 8-thread stress + checksums | no cross-thread corruption, balance |
| ThreadSanitizer | no data races |
| AddressSanitizer + UBSan | no overflows / UB |
| Valgrind | `0 bytes in use at exit` — zero leaks |

## Known limitations

- Blocks are 8-byte aligned; over-aligned requests (> `max_align_t`)
  bypass the pool (handled in the pmr adapter).
- Slabs are never returned to the OS (standard slab-allocator trade-off:
  peak-sized working sets are retained for reuse).
- The size must be supplied at deallocation — matches STL allocator
  contracts, but raw `free(ptr)`-style usage would need a header variant.
