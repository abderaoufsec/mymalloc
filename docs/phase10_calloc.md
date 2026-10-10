# mymalloc — Phase 10: `calloc`

## Goal

`calloc(count, size)` differs from `malloc(size)` in two ways that the C
standard makes mandatory, and both are the point of this phase:

1. **Overflow-safe multiply.** `count * size` must be rejected when it would
   overflow `size_t`, *before* the multiplication happens.
2. **Zero-initialized memory.** Every byte of the returned array reads as `0`.

Everything else (alignment, region acquisition, reuse, splitting) is inherited
unchanged from `my_malloc`.

## The overflow check (do it before the multiply)

```
if (count == 0 || size == 0) return NULL;   // documented decision
if (count > SIZE_MAX / size) return NULL;   // count*size would overflow
total = count * size;                        // now provably safe
```

- The `count == 0 || size == 0` guard comes first, which also makes
  `SIZE_MAX / size` safe to compute (no division by zero).
- `count > SIZE_MAX / size` is the canonical test: if `count` exceeds the
  largest count for which `count * size` fits, the product wraps.
- Symmetric by construction — `calloc(SIZE_MAX, 2)` and `calloc(2, SIZE_MAX)`
  are both caught, because the test divides by whichever factor and compares
  the other.

### Why not compute the product and compare?

`total = count * size` **already wrapped** by the time you could compare it
against `SIZE_MAX`. Once wrapped, `total` looks small and *plausible* — the
classic exploit is `calloc(0x100, 0x100000000)` style inputs that yield a tiny
"allocation" the program then overruns. The division test has no such window.

## The zeroing requirement (the subtle part here)

A **fresh** raw region arrives zero-filled from the OS (Phase 2). But this
allocator **reuses** freed blocks (Phase 6), **splits** them (Phase 7), and
**coalesces** them (Phase 8). A reused block is memory a previous allocation
already wrote to — it carries **stale, non-zero bytes**.

Therefore `calloc` must **explicitly zero** the allocation rather than trust
the mapping:

```c
void* p = my_malloc(count * size);
if (p == NULL) return NULL;
memset(p, 0, count * size);   // only the requested bytes are defined
return p;
```

- Only `count * size` bytes are zeroed — exactly the C contract. Slack inside
  the block (header padding, alignment rounding) is left untouched; it is not
  part of the array the caller can name.
- The memset uses `total` (`count * size`), the *requested* size, not the
  block payload size, so calloc never writes past the caller's array.

## Rejecting zero (a documented decision)

`calloc(0, n)`, `calloc(n, 0)`, and `calloc(0, 0)` all return `NULL`, matching
`my_malloc(0)` from Phase 5. The C standard leaves `calloc(0, ...)`
implementation-defined (it may return a unique freeable pointer or `NULL`);
mymalloc picks the loud, consistent-with-malloc behavior.

## Product fits but is too large to map

A product that passes the `size_t` check can still be impossible to satisfy —
`my_calloc(SIZE_MAX/8, 1)` fits in `size_t` but `my_malloc` cannot map it, so
the whole thing still returns `NULL`. Overflow safety and OS-failure safety are
layered: calloc's check prevents arithmetic wraparound; `my_malloc`'s overflow
checks (Phase 5) and `my_raw_acquire` failure path (Phase 2) handle the rest.

## API

`my_calloc(count, size)` was already declared in `<mymalloc/mymalloc.h>`
(Phase 0); Phase 10 only adds the definition in `src/allocator.cpp`. No header
change.

## Test map (`tests/test_calloc.cpp`)

- zero count / zero size → `NULL`
- basic zero-init: contents all zero, aligned, valid block, writable after
- odd shapes: `1x1`, `4096x1`, `7x1000` (non-page total)
- **reuse zeroing**: `malloc` → dirty with `0xFF` → `free` → `calloc` same size
  is still all zero (the property that mandates the explicit memset)
- overflow: `(SIZE_MAX,2)`, `(2,SIZE_MAX)`, `(SIZE_MAX,SIZE_MAX)`,
  `(SIZE_MAX/2,4)`, `(SIZE_MAX/8,1)` all `NULL`
- allocator still works after failures; 100-cycle calloc/free loop

## Limitations / deferred

- No calloc-specific reuse of an already-zero block (would avoid the memset);
  that is an optimization, not a correctness concern.
- `calloc` does not track that a block was zeroed; re-`calloc` of a *reused*
  block re-zeroes, which is always correct.
- Thread safety: none (unchanged).
- Loud double-free / corruption diagnostics: Phase 14.
