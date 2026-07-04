// test_main.cpp — unit tests (no external framework: a ~30-line harness
// keeps the project dependency-free and trivially buildable anywhere).
#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <memory_resource>
#include <set>
#include <string>
#include <vector>

#include "mempool/adapters.hpp"
#include "mempool/pool_allocator.hpp"
#include "mempool/size_classes.hpp"

// ── mini test harness ──────────────────────────────────────────────
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " \
                      << #cond << "\n";                                 \
        }                                                               \
    } while (0)

#define TEST(name) static void name()

using namespace mempool;

// ── size class math ────────────────────────────────────────────────
TEST(test_size_classes) {
    CHECK(size_to_class(1) == 0);
    CHECK(class_to_size(size_to_class(1)) >= 1);
    for (std::size_t s = 1; s <= kMaxPooledSize; ++s) {
        const std::size_t cls = size_to_class(s);
        CHECK(class_to_size(cls) >= s);          // block fits request
        if (cls > 0) CHECK(class_to_size(cls - 1) < s);  // tightest class
    }
    CHECK(!is_pooled(0));
    CHECK(is_pooled(1024));
    CHECK(!is_pooled(1025));
}

// ── basic allocate/deallocate ──────────────────────────────────────
TEST(test_basic_alloc) {
    auto& pool = MemoryPool::instance();
    void* p = pool.allocate(64);
    CHECK(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 8 == 0);  // aligned
    std::memset(p, 0xAB, 64);  // must be writable
    pool.deallocate(p, 64);
}

// ── freed blocks are recycled ──────────────────────────────────────
TEST(test_block_reuse) {
    auto& pool = MemoryPool::instance();
    void* first = pool.allocate(128);
    pool.deallocate(first, 128);
    void* second = pool.allocate(128);
    CHECK(first == second);  // LIFO thread cache returns the same block
    pool.deallocate(second, 128);
}

// ── unique addresses while live ────────────────────────────────────
TEST(test_unique_addresses) {
    auto& pool = MemoryPool::instance();
    std::set<void*> seen;
    std::vector<void*> ptrs;
    for (int i = 0; i < 5000; ++i) {
        void* p = pool.allocate(48);
        CHECK(seen.insert(p).second);  // never handed out twice
        ptrs.push_back(p);
    }
    for (void* p : ptrs) pool.deallocate(p, 48);
}

// ── data integrity across many live blocks ─────────────────────────
TEST(test_no_overlap_scribble) {
    auto& pool = MemoryPool::instance();
    constexpr int kN = 2000;
    std::vector<unsigned char*> ptrs(kN);
    for (int i = 0; i < kN; ++i) {
        ptrs[i] = static_cast<unsigned char*>(pool.allocate(96));
        std::memset(ptrs[i], i & 0xFF, 96);
    }
    for (int i = 0; i < kN; ++i) {          // verify nothing was trampled
        for (int b = 0; b < 96; ++b) CHECK(ptrs[i][b] == (i & 0xFF));
        pool.deallocate(ptrs[i], 96);
    }
}

// ── oversized requests fall back to operator new ───────────────────
TEST(test_large_fallback) {
    auto& pool = MemoryPool::instance();
    const auto before = pool.stats().fallback_allocs.load();
    void* p = pool.allocate(1'000'000);
    CHECK(p != nullptr);
    std::memset(p, 0, 1'000'000);
    pool.deallocate(p, 1'000'000);
    CHECK(pool.stats().fallback_allocs.load() == before + 1);
}

// ── std::allocator adapter with real containers ────────────────────
TEST(test_std_containers) {
    std::vector<int, PoolAllocator<int>> v;
    for (int i = 0; i < 10'000; ++i) v.push_back(i);
    CHECK(v[9'999] == 9'999);

    std::list<std::string, PoolAllocator<std::string>> l;
    for (int i = 0; i < 100; ++i) l.emplace_back("node " + std::to_string(i));
    CHECK(l.back() == "node 99");

    std::map<int, int, std::less<>,
             PoolAllocator<std::pair<const int, int>>> m;
    for (int i = 0; i < 1000; ++i) m[i] = i * i;
    CHECK(m[31] == 961);
}

// ── pmr adapter ────────────────────────────────────────────────────
TEST(test_pmr_resource) {
    PoolResource res;
    std::pmr::vector<double> v{&res};
    for (int i = 0; i < 5000; ++i) v.push_back(i * 0.5);
    CHECK(v[4'999] == 4'999 * 0.5);

    std::pmr::string s{&res};
    s = "polymorphic allocators route through the slab pool";
    CHECK(s.size() > 0);
}

// ── stats sanity ───────────────────────────────────────────────────
TEST(test_stats) {
    auto& st = MemoryPool::instance().stats();
    CHECK(st.allocations.load() > 0);
    CHECK(st.slabs_created.load() > 0);
    CHECK(st.bytes_reserved.load() ==
          st.slabs_created.load() * kSlabSize);
    // Everything this test suite allocated has been freed:
    CHECK(st.allocations.load() == st.deallocations.load());
}

int main() {
    test_size_classes();
    test_basic_alloc();
    test_block_reuse();
    test_unique_addresses();
    test_no_overlap_scribble();
    test_large_fallback();
    test_std_containers();
    test_pmr_resource();
    test_stats();  // must run last (checks alloc == dealloc)

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " — " << g_checks
              << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
