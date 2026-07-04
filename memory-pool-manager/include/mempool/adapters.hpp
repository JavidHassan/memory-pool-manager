// adapters.hpp — standard-library integration.
//   PoolAllocator<T>  : drop-in std::allocator replacement (works with
//                       std::vector, std::list, std::map, ...)
//   PoolResource      : std::pmr::memory_resource for pmr containers.
#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>

#include "pool_allocator.hpp"

namespace mempool {

// C++20 concept documenting the requirements this adapter relies on.
template <typename P>
concept PoolLike = requires(P& p, std::size_t n, void* ptr) {
    { p.allocate(n) } -> std::same_as<void*>;
    { p.deallocate(ptr, n) };
};

static_assert(PoolLike<MemoryPool>);

template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    PoolAllocator() noexcept = default;
    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        return static_cast<T*>(MemoryPool::instance().allocate(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        MemoryPool::instance().deallocate(p, n * sizeof(T));
    }

    template <typename U>
    bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
};

// std::pmr adapter: pass &resource to any pmr container.
class PoolResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Pool blocks are 8-byte aligned by construction (all class sizes
        // are multiples of 8, slabs are 64-byte aligned). Over-aligned
        // requests fall through to the aligned global allocator.
        if (alignment > alignof(std::max_align_t)) [[unlikely]] {
            return ::operator new(bytes, std::align_val_t{alignment});
        }
        return MemoryPool::instance().allocate(bytes);
    }

    void do_deallocate(void* p, std::size_t bytes,
                       std::size_t alignment) override {
        if (alignment > alignof(std::max_align_t)) [[unlikely]] {
            ::operator delete(p, std::align_val_t{alignment});
            return;
        }
        MemoryPool::instance().deallocate(p, bytes);
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace mempool
