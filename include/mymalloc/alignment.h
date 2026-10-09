// mymalloc — alignment helpers (Phase 3).
//
// Alignment requirements for mymalloc (see docs/phase3_alignment.md):
// - my_malloc (Phase 5+) must return pointers aligned to
//   my_default_alignment(), i.e. alignof(max_align_t) — 16 bytes on
//   x86-64 — matching the guarantee of the standard malloc.
// - All alignments are powers of two, which enables the branchless
//   rounding trick (value + mask) & ~mask.
//
// Every rounding operation is overflow-checked: when rounding up would
// exceed the type's range, the helpers fail (return 0) instead of silently
// wrapping to a small, wrongly-"aligned" value.
#ifndef MYMALLOC_ALIGNMENT_H
#define MYMALLOC_ALIGNMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 when value is a non-zero power of two, else 0.
int my_is_power_of_two(size_t value);

// Returns the base alignment my_malloc guarantees (a power of two;
// alignof(max_align_t) — 16 on x86-64).
size_t my_default_alignment(void);

// Computes the smallest multiple of `alignment` that is >= `value`.
// Returns 1 and writes the result through `out` on success.
// Returns 0 (leaving `*out` untouched) when `out` is NULL, `alignment` is
// not a power of two, or the rounding would overflow size_t.
int my_align_up_size(size_t value, size_t alignment, size_t* out);

// Same rounding for address values. Returns 0 (leaving `*out` untouched)
// on NULL out, non-power-of-two alignment, or uintptr_t overflow.
int my_align_up_address(uintptr_t address, size_t alignment, uintptr_t* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_ALIGNMENT_H