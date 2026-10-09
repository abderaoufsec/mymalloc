# Phase 5 — First Allocator

The target API from the project documentation becomes real:

```c
void* my_malloc(size_t size);
void  my_free(void* ptr);
```

(Phases 10 and 11 add `my_calloc` / `my_realloc`.) Built strictly on the
previous phases: raw regions (2), alignment (3), block metadata (4).

## 1. Model: one region, one block

Every `my_malloc` acquires its **own raw region**, rounded up to pages, and
uses the entire region as exactly one block:

```text
region (page-rounded)                 block = whole region
[ header (32 B) | payload ... ]  <--  my_malloc returns &payload
```

This is deliberately the simplest thing that satisfies the Phase 5 TODO.
What it does **not** do yet — and which phase adds it:

| Missing in Phase 5 | Added by |
|--------------------|----------|
| Reusing freed blocks | Phase 6 (free list) |
| Splitting oversized blocks | Phase 7 (splitting) |
| Merging adjacent free blocks | Phase 8 (coalescing) |
| Releasing wholly-free regions to the OS | Phase 12 (heap management) |
| First-fit/best-fit strategy choice | Phase 9 (strategies) |

Consequence (documented honestly): a 1-byte allocation currently costs one
page (4 KiB) plus one syscall, and `my_free` only *marks* the block free —
nothing is reused or unmapped yet. Every follow-up phase attacks exactly
this.

## 2. `my_malloc` flow

```text
my_malloc(size)
  → reject size == 0 (documented decision; C permits NULL)
  → align size up to my_default_alignment()      [Phase 3, overflow-checked]
  → add my_block_user_offset()                   [overflow-checked]
  → round total up to page size                  [Phase 2/3 math]
  → my_raw_acquire()                             [Phase 2; failure → NULL]
  → my_block_init(block, whole region, allocated)[Phase 4]
  → return my_block_to_user(block)               [always aligned]
```

Failure paths (all return `NULL`, all tested): zero size request; request
rounding overflows `size_t` (`SIZE_MAX`); payload + header overflows
(`SIZE_MAX − 31`); the OS refuses the region (`SIZE_MAX/2`, `SIZE_MAX/4` —
they align fine but exceed any user-space address space).

## 3. `my_free` flow

```text
my_free(ptr)
  → NULL? no-op                                   [C requirement]
  → block = my_user_to_block(ptr)
  → my_block_valid(block)? if not: refuse silently [Phase 14 adds loud checks]
  → block->free = 1                               [nothing else yet]
```

Double-freeing only re-marks the flag in Phase 5; double-free *diagnosis*
is Phase 14. Freeing foreign pointers is out of contract until Phase 14's
pointer validation — the header validity check is a cheap first filter, not
a guarantee.

## 4. Invariants (machine-checked by `tests/test_allocator.cpp`)

1. Every non-NULL `my_malloc` result is aligned to `my_default_alignment()`.
2. Every result is a valid Phase 4 block (`my_block_valid`), allocated, with
   payload ≥ the requested size.
3. The full requested range is writable and isolated — patterns written to
   one allocation never appear in another.
4. Simultaneous allocations are pairwise distinct and non-overlapping.
5. `free(nullptr)` and repeated frees do not crash.
6. All four failure classes return `NULL` and the allocator keeps working
   afterwards.
7. 100 alloc/free cycles with varying sizes complete normally.

## 5. Experiment (`examples/allocator_demo.cpp`)

Allocates blocks of 1 / 16 / 1000 / 4096 / 50000 bytes, printing the
returned pointer, alignment, backing block validity and payload size;
demonstrates simultaneous isolated allocations and the failure cases:

```sh
cmake --build --preset debug
./out/build/debug/examples/allocator_demo
```

## 6. Limitations

- **Wasteful by design (temporary)**: every allocation costs at least one
  page + one syscall; freed memory is never reused (Phase 6) or unmapped
  (Phase 12). This is the starting point the next four phases improve.
- `my_malloc(0)` returns `NULL` (chosen over the C-permitted
  "unique pointer" behavior; revisit with `calloc`/`realloc` semantics in
  Phases 10/11 if needed).
- `my_free` on foreign pointers is out of contract until Phase 14 (the
  Phase 4 validity check only filters pointer values that are misaligned or
  structurally impossible).
- Not thread-safe (no locks/atomics anywhere yet) — same policy as all
  phases so far.
- Memory returned by `my_malloc` is *not* zeroed; use `my_calloc` (Phase 10)
  when that lands.