// slab.hpp — a contiguous, cache-line-aligned region of memory carved
// into fixed-size blocks. RAII: the Slab owns its memory; destruction
// releases it unconditionally (leak-free by construction).
#pragma once

#include <cstddef>
#include <memory>
#include <new>

#include "size_classes.hpp"

namespace mempool {

class Slab {
public:
    explicit Slab(std::size_t block_size)
        : block_size_(block_size),
          capacity_(kSlabSize / block_size),
          memory_(static_cast<std::byte*>(
              ::operator new(kSlabSize, std::align_val_t{kAlignment}))) {}

    ~Slab() { ::operator delete(memory_, std::align_val_t{kAlignment}); }

    Slab(const Slab&) = delete;
    Slab& operator=(const Slab&) = delete;
    Slab(Slab&&) = delete;
    Slab& operator=(Slab&&) = delete;

    // Carve the slab into an intrusive singly-linked free list.
    // Returns head; blocks are linked via their first sizeof(void*) bytes.
    [[nodiscard]] void* carve() noexcept {
        std::byte* first = memory_;
        for (std::size_t i = 0; i + 1 < capacity_; ++i) {
            void** link = reinterpret_cast<void**>(memory_ + i * block_size_);
            *link = memory_ + (i + 1) * block_size_;
        }
        void** last =
            reinterpret_cast<void**>(memory_ + (capacity_ - 1) * block_size_);
        *last = nullptr;
        return first;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

    [[nodiscard]] bool contains(const void* p) const noexcept {
        auto* b = static_cast<const std::byte*>(p);
        return b >= memory_ && b < memory_ + kSlabSize;
    }

    static constexpr std::size_t kAlignment = 64;  // cache line

private:
    std::size_t block_size_;
    std::size_t capacity_;
    std::byte* memory_;
};

}  // namespace mempool
