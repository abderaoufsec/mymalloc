# Phase 2 — Raw Memory

This document covers the Phase 2 study of OS memory primitives, the strategy
decision, and the resulting raw-memory layer. See also
[phase1_memory_model.md](phase1_memory_model.md) for the address-space
background.

## 1. Study: `mmap()` / `munmap()` (POSIX)

```c
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
```

- Creates/destroys **anonymous private mappings** (`MAP_PRIVATE | MAP_ANONYMOUS`):
  slices of the virtual address space not backed by any file.
- Works in **whole pages**: `length` is rounded up, `addr` may be any value
  (pass `NULL` to let the OS choose). The result is **page-aligned**.
- `PROT_READ | PROT_WRITE` gives a private, writable region — exactly what an
  allocator needs.
- Fresh anonymous pages are **zero-filled** by the OS, lazily (the first
  write triggers a page fault that backs the page with physical RAM).
- `mmap` returns `MAP_FAILED` (`(void*)-1`) on failure — it does **not** set
  `errno`-style NULL; the caller must compare against `MAP_FAILED`.
- `munmap` can unmap **any** page-aligned sub-range, releasing address space
  back to the OS (the physical pages disappear when no longer referenced).

## 2. Study: `brk()` / `sbrk()` (POSIX)

```c
int   brk(void* end_data_segment);
void* sbrk(intptr_t increment);
```

- Moves the **program break**: the end of the contiguous heap that starts
  just above `.bss` (see Phase 1 diagram). `sbrk(n)` grows/shrinks the heap
  by `n` bytes and returns the *previous* break.
- glibc's `malloc` uses `brk` for small allocations and `mmap` above a
  threshold (~128 KiB).
- Limitations for a from-scratch allocator:
  - **POSIX-only** — Windows has no `brk`/`sbrk` at all;
  - the heap is **one contiguous range**: memory in the middle can only be
    reused, never returned to the OS without collapsing the whole tail;
  - shrinking the break is unsafe if any live pointer references the tail;
  - global lock over one break — poor concurrency characteristics.

## 3. Strategy decision

**Chosen: `mmap`/`munmap` on POSIX, `VirtualAlloc`/`VirtualFree` on Windows.**

| Criterion | mmap + VirtualAlloc | brk/sbrk |
|-----------|--------------------|----------|
| Portability (Linux + Windows CI/dev) | ✅ both platforms | ❌ POSIX only |
| Release memory to the OS | ✅ any region, any time | ⚠️ only the tail of the heap |
| Granularity | page (matches Phase 1 finding) | byte (break), but pages underneath |
| Failure handling | explicit return value | explicit, but break semantics subtle |
| Multi-region design (Phase 12) | natural fit | forces one mega-region |

Windows equivalents used by the same API:

```c
void* VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
BOOL  VirtualFree(ptr, 0, MEM_RELEASE);   // frees the whole reservation
```

Both OS paths return zero-initialized, page-aligned memory, so the raw layer
exposes one uniform contract on every platform.

## 4. The raw layer API — `include/mymalloc/raw_memory.h`

```c
size_t my_raw_page_size(void);              // cached page size
void*  my_raw_acquire(size_t bytes);        // page-rounded, or NULL on failure
void   my_raw_release(void* ptr, size_t bytes);  // same `bytes` as acquire
```

Contract / invariants (all machine-checked by `tests/test_raw_memory.cpp`):

1. `my_raw_page_size()` returns a power-of-two page size, queried once.
2. `my_raw_acquire(b)` returns either `NULL` or a **page-aligned** block of
   at least `round_up(b, page_size)` **writable, zero-filled** bytes.
3. Failure (returns `NULL`): `b == 0`; rounding `b` up would overflow
   `size_t`; the OS refuses (e.g. `b > SIZE_MAX/2` exceeds any user-space
   address space).
4. `my_raw_release` frees the whole region; passing the original `b` is
   enough because rounding is deterministic. `NULL` is a no-op.
5. Concurrently live regions never overlap (OS guarantee, verified).

Architecture change (rule 7): the library now has a dedicated OS layer —
allocation policy in later phases must go through it:

```text
Phase 0-1:  mymalloc API ────────────── version helpers
Phase 2:    mymalloc API ── raw_memory.h ── mmap/VirtualAlloc (OS)
```

## 5. Implementation notes (`src/raw_memory.cpp`)

- **One rounding helper**: `round_up_to_pages` returns `0` as a "must fail"
  marker for zero-byte requests and for requests that would overflow `size_t`
  when rounded — overflow can never silently produce a smaller region.
- **Page size cache**: a function-local `static const` initialized by a lambda
  ("magic static") — thread-safe since C++11, no `std::call_once` needed.
- **Uniform failure semantics**: both backends map their native failure
  (`MAP_FAILED`, `NULL`) to `NULL`, so callers never deal with platform
  specifics.
- **`MAP_ANONYMOUS` fallback**: `#ifndef MAP_ANONYMOUS` → `MAP_ANON` for
  older BSDs/macOS.
- **Release safety**: `munmap` errors are ignored (unmapping valid memory we
  acquired cannot fail in practice); `VirtualFree(ptr, 0, MEM_RELEASE)` is
  used because `MEM_RELEASE` requires freeing the entire reservation.

## 6. Tests (`tests/test_raw_memory.cpp`)

| Check | Why it matters |
|-------|----------------|
| page size power of two, sane bounds | Phase 1 invariant still holds at the API level |
| acquire(page): aligned, zero-filled, writable end-to-end | base contract |
| acquire(8 pages): whole range writable | multi-page regions work |
| acquire(1): full page writable | rounding is real, not just advertised |
| fresh region reads all-zero | OS zero-fill guarantee surfaced to callers |
| acquire(0), acquire(SIZE_MAX), acquire(SIZE_MAX-100) → NULL | zero/overflow handled (rules 4) |
| acquire(SIZE_MAX/2) → NULL | OS-refusal path exercised portably |
| release(NULL, ...) no-op | documented behavior |
| 4 live regions pairwise disjoint | OS never hands out overlapping memory |
| 200 acquire/write/verify/release cycles | repeated cycles (TODO item) leak-free & stable under sanitizers |

## 7. Experiment (`examples/raw_memory_demo.cpp`)

Acquires three regions (1 page, 8 pages, 100 bytes → 1 page), prints
addresses/alignment, touches every page, demonstrates the three failure
modes, and releases everything:

```sh
cmake --build --preset debug
./out/build/debug/examples/raw_memory_demo
```

## 8. Limitations

- No guard pages or `madvise` hints yet (possible future hardening).
- `my_raw_release` trusts the caller about `bytes`; a real heap manager
  (Phase 12) will track region sizes itself and stop relying on the caller.
- Not thread-cached: every acquire is a direct syscall. Fine for Phase 2;
  caching belongs to later phases.
- Zero-size allocation semantics for `my_malloc(0)` are decided in Phase 5,
  not here (the raw layer simply rejects 0).