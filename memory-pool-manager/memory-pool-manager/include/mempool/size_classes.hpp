// size_classes.hpp — compile-time size class table
// Maps allocation sizes to a small set of fixed block sizes, bounding
// internal fragmentation while keeping per-class free lists simple.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mempool {

// Size classes: 8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024.
// Geometric-ish progression keeps worst-case internal fragmentation < 34%.
inline constexpr std::array<std::size_t, 13> kSizeClasses = {
    8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};

inline constexpr std::size_t kNumClasses = kSizeClasses.size();
inline constexpr std::size_t kMaxPooledSize = kSizeClasses.back();
inline constexpr std::size_t kSlabSize = 64 * 1024;  // 64 KiB per slab

// Map a requested byte size to its size-class index. O(1) via a lookup
// table built at compile time (index = (size-1)/8).
namespace detail {
consteval auto build_class_lookup() {
    std::array<std::uint8_t, kMaxPooledSize / 8> table{};
    std::size_t cls = 0;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const std::size_t size = (i + 1) * 8;
        while (kSizeClasses[cls] < size) ++cls;
        table[i] = static_cast<std::uint8_t>(cls);
    }
    return table;
}
inline constexpr auto kClassLookup = build_class_lookup();
}  // namespace detail

// Round size up to a multiple of 8, then look up its class.
[[nodiscard]] constexpr std::size_t size_to_class(std::size_t size) noexcept {
    const std::size_t idx = (size + 7) / 8;  // 1..128 for sizes 1..1024
    return detail::kClassLookup[idx - 1];
}

[[nodiscard]] constexpr std::size_t class_to_size(std::size_t cls) noexcept {
    return kSizeClasses[cls];
}

[[nodiscard]] constexpr bool is_pooled(std::size_t size) noexcept {
    return size > 0 && size <= kMaxPooledSize;
}

static_assert(size_to_class(1) == 0);
static_assert(size_to_class(8) == 0);
static_assert(size_to_class(9) == 1);
static_assert(size_to_class(64) == 4);
static_assert(size_to_class(65) == 5);
static_assert(size_to_class(1024) == kNumClasses - 1);

}  // namespace mempool
