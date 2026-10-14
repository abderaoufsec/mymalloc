// Alignment helpers for mymalloc (Phase 3).
//
// Rounding uses the classic power-of-two bit trick:
//   aligned = (value + (alignment - 1)) & ~(alignment - 1)
// which is branch-free but wraps around on overflow — hence the explicit
// range checks before adding the mask. See docs/phase3_alignment.md.
#include <mymalloc/alignment.h>

#include <cstddef>
#include <cstdint>

int my_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1U)) == 0U;
}

size_t my_default_alignment(void) {
    // Magic static: the C++ guarantee for malloc/new is alignof(max_align_t),
    // a power of two. That is 16 on x86-64 under GCC/Clang (long double is
    // 16-byte aligned) but only 8 under MSVC (long double is 8-byte aligned
    // there). mymalloc's documented contract — and what every test assumes —
    // is a 16-byte guarantee on 64-bit platforms, so floor the natural value
    // at 16 when pointers are 64-bit. On 32-bit platforms the natural
    // alignof(max_align_t) (typically 8) is kept as-is.
    static const std::size_t alignment = [] {
        const std::size_t natural = alignof(std::max_align_t);
        const std::size_t floor = sizeof(void*) >= 8 ? std::size_t{16} : std::size_t{8};
        return natural > floor ? natural : floor;
    }();
    return alignment;
}

int my_align_up_size(size_t value, size_t alignment, size_t* out) {
    if (out == NULL || !my_is_power_of_two(alignment)) {
        return 0;
    }
    const std::size_t mask = alignment - 1U;
    if (value > SIZE_MAX - mask) {
        return 0; // rounding up would overflow size_t
    }
    *out = (value + mask) & ~mask;
    return 1;
}

int my_align_up_address(uintptr_t address, size_t alignment, uintptr_t* out) {
    if (out == NULL || !my_is_power_of_two(alignment)) {
        return 0;
    }
    const uintptr_t mask = static_cast<uintptr_t>(alignment - 1U);
    if (address > UINTPTR_MAX - mask) {
        return 0; // rounding up would overflow uintptr_t
    }
    *out = (address + mask) & ~mask;
    return 1;
}