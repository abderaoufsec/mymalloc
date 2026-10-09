# Phase 3 — Alignment

This document defines mymalloc's alignment requirements, explains the
overflow-safe rounding helpers, and lists the machine-checked invariants
(`tests/test_alignment.cpp`). See
[phase1_memory_model.md](phase1_memory_model.md) for why the hardware cares
about pages.

## 1. What alignment is

- An address is **aligned to N** when it is a multiple of N. The hardware
  requires naturally aligned accesses (e.g. an 8-byte load must target a
  multiple of 8) — misaligned accesses fault or silently cost extra cycles.
- Every type has a **fundamental alignment** `alignof(T)`, its required
  placement multiple. The strictest fundamental alignment on the platform is
  `alignof(std::max_align_t)` — **16 bytes on x86-64**, typically 8 on
  32-bit x86.
- The C/C++ standard contract for `malloc`/`new`: returned memory is aligned
  to `alignof(std::max_align_t)`, so it can host *any* object without
  further treatment. **my_malloc must offer the same guarantee.**

## 2. mymalloc's alignment requirement

| Requirement | Value | Where |
|-------------|-------|-------|
| `my_malloc` return alignment | `my_default_alignment()` = `alignof(max_align_t)` (16 on x86-64) | Phase 5+ |
| Block/region sizes | multiples of the default alignment | Phase 4+ |
| Raw regions (Phase 2) | page-aligned — *stronger* than needed | already true |
| Future over-aligned types (C++17 `alignas(64)`) | optional extension, not guaranteed yet | see Limitations |

The default alignment is deliberately *not* page size: 16-byte granularity
lets a page hold many small blocks, while still satisfying every fundamental
type (including `long double` and SIMD types on x86-64).

## 3. Why powers of two — and the bit trick

All alignments are powers of two, so rounding up is a branchless mask:

```c
mask = alignment - 1;              /* e.g. align 16 -> mask 0xF  */
aligned = (value + mask) & ~mask;  /* round up to next multiple  */
```

vs. the modulo form `(value + alignment - 1) / alignment * alignment`, which
needs a division. This is what real allocators do — but the trick has a
sharp edge: **it silently wraps on overflow**. `align_up(SIZE_MAX, 8)` would
produce `0` — a tiny, perfectly "aligned" number that a caller could mistake
for a valid size. That failure mode is catastrophic inside an allocator
(it invites wrapping allocations and heap corruption), which is why the
helpers below never use the trick blindly.

## 4. Overflow-safe helper API — `include/mymalloc/alignment.h`

```c
int    my_is_power_of_two(size_t value);
size_t my_default_alignment(void);
int    my_align_up_size(size_t value, size_t alignment, size_t* out);
int    my_align_up_address(uintptr_t address, size_t alignment, uintptr_t* out);
```

Design rules (engineering rules 3 and 4):

1. **Status + out-parameter, not return-the-result**: rounding can fail, and
   a failure must be impossible to ignore. `1` = success (result written),
   `0` = failure (`*out` untouched).
2. **Failures**: `out == NULL`; `alignment` is 0 or not a power of two;
   rounding up would overflow `size_t` / `uintptr_t`.
3. **Exact multiples are unchanged** — `align_up(16, 16) == 16`, never 32.
   (Wasting a whole slot on every power-of-two request would be an
   unnecessary fragmentation source.)
4. `my_default_alignment()` is a power of two on every platform (asserted by
   the tests; C++ guarantees `max_align_t` is a struct with fundamental
   alignment, hence a power of two).

## 5. Invariants captured by the tests (`tests/test_alignment.cpp`)

1. `my_is_power_of_two`: true for 1, 2, 4, …, 2⁶²; false for 0, 3, 5, 4095,
   `SIZE_MAX`.
2. `my_default_alignment()` is a power of two in [8, 64].
3. For (value, alignment) pairs across a sweep of 7 alignments × 300 values
   plus boundary cases (`align-1`, `align`, `align+1`, `1024*align-1`):
   result ≥ value, result is a multiple of the alignment, and
   result − value < alignment (i.e. it is the *smallest* such multiple).
4. Exact multiples are unchanged.
5. Address rounding: `0x1001 → 0x2000` (page), `0x100F → 0x1010` (16).
6. Overflow boundaries: `align_up(SIZE_MAX − 7, 8)` succeeds with
   `SIZE_MAX − 7`; `align_up(SIZE_MAX − 6, 8)` fails; same for addresses
   with `UINTPTR_MAX`. The out-parameter keeps its sentinel value on every
   failure (NULL out, alignment 0/3/1023, overflow).
7. Integration with Phase 2: an interior pointer `raw + 1` of a live raw
   region aligns up to a properly aligned pointer that is still inside the
   region and can be written safely.

## 6. Experiment (`examples/alignment_demo.cpp`)

Prints the default alignment, a rounding table (exact / just-below /
just-above / page boundaries), the overflow-protection cases (including the
`SIZE_MAX − 6` case that would *wrap to 0* without the guard), and aligns an
interior pointer inside a real raw region:

```sh
cmake --build --preset debug
./out/build/debug/examples/alignment_demo
```

## 7. Limitations

- Only power-of-two alignments are supported (all real allocator designs
  need this; modulo-based rounding for arbitrary N is intentionally out of
  scope).
- Over-aligned C++ types (`alignas(64)`, `alignas(128)`) are **not** yet
  guaranteed by `my_malloc` — supporting them needs a `my_aligned_alloc`
  decision in a later phase. The helpers here already accept any
  power-of-two alignment, so the policy layer will not need new math.
- The helpers are not thread-safety-relevant: they are pure functions.