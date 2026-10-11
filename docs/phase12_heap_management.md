# Phase 12 — Heap Management: Regions, Ownership and Safe Release

## Problem

Until Phase 12 mymalloc's heap only ever grew. `my_malloc` acquired raw
page-rounded regions through `my_raw_acquire` (Phase 2) and handed them to
the block layer, but nothing remembered which regions existed. A fully
freed region stayed mapped forever, listed as one big free block — correct,
but a long-running program that allocates in waves (a burst of 2 MiB
buffers, all freed, then a burst of 4 KiB nodes, ...) would pin every wave's
pages for the life of the process. Phase 12 closes that gap:

1. **Track managed regions** — remember every region the allocator owns.
2. **Support multiple regions** — the registry scales with the heap.
3. **Safely release eligible regions** — give fully-free regions back to the OS.
4. **Prevent invalid unmapping** — never release a region that is foreign,
   mis-sized, or still contains live data.

## The ownership model

Ownership has exactly two levels:

- **Regions belong to mymalloc.** A region is one `my_raw_acquire` extent —
  page-aligned, page-rounded. mymalloc acquires it, tracks it, carves it
  into blocks, and is the only party allowed to `my_raw_release` it.
- **Blocks belong to the caller.** The caller receives block payloads
  (`my_malloc`) and returns them (`my_free`); that is the entire public
  contract. Every bookkeeping decision about regions stays inside the
  allocator.

Code that acquires raw regions DIRECTLY (tests, demos, future phases) owns
them itself. Such regions are invisible to the registry, which is precisely
what keeps the allocator's release paths from ever unmapping them — verified
by `test_untracked_regions_untouched` in tests/test_regions.cpp.

## The registry (src/regions.cpp, include/mymalloc/regions.h)

Each tracked region is one `{base, bytes}` entry, where `bytes` is the exact
page-rounded extent that must be passed to `my_raw_release`. The entries live
in a growable table:

- the buffer itself is acquired with `my_raw_acquire` — allocator metadata
  must never recurse into `my_malloc` (that would re-enter the free list,
  splitting, and scavenge logic from inside itself);
- it starts at 64 entries and doubles when full, so there is **no artificial
  cap** on simultaneously live regions — a Phase 15 stress workload with
  thousands of live blocks cannot exhaust it (growth fails only when the OS
  refuses memory, in which case `my_malloc` releases the just-acquired
  region and returns `NULL` — loud failure, no leak);
- `my_region_find` returns a COPY of the entry (the table moves on growth),
  and `my_region_track` refuses double-tracking a base (which could
  otherwise be released twice).

`my_malloc`'s acquisition path is the only place regions are tracked:
acquire → initialize the block → track → return the payload. If tracking
fails, the fresh region is released on the spot.

## Release policy: scavenge-on-acquire

The policy decision is **when** to give regions back. Two shapes were
considered:

- **Release on free** (every `my_free` that leaves a region fully free
  immediately unmaps it). Rejected: the commonest workload — a tight
  alloc/free loop — would unmap and remap pages on every other call, and
  even `malloc A; malloc B; free A; malloc C` would lose A's region a
  moment after paying to create it.
- **Scavenge on acquire** (chosen): free memory lives on the free list, and
  regions are reclaimed **only when the allocator must grow anyway** —
  immediately before `my_raw_acquire` inside `my_malloc`, every fully-free
  region is released (`my_regions_release_free`). `my_free` never shrinks
  the heap.

Properties of the chosen policy:

- Hot alloc/free paths perform **zero** OS traffic: reuse (Phase 6) and
  coalescing (Phase 8) absorb cycles inside the free list, and the loop in
  `test_long_cycles_bounded` keeps the registry at a handful of regions
  across 2000 mixed-size cycles.
- Pages are still returned: any fresh acquisition reclaims every region
  that nothing points into anymore, so "waves" of allocations do not
  accumulate.
- `my_realloc`'s move path is safe by construction: the copy out of the old
  block happens before `my_free(old)`; the old region can only be reclaimed
  by a LATER acquire, and only while fully free.
- At process exit the last region(s) remain mapped. That is normal and
  costs nothing — the OS reclaims them with the address space.

## Preventing invalid unmapping

Every release passes through `my_region_release`, which re-checks four
guardrails — a refused call unmaps NOTHING:

1. **Exact membership**: `base` must equal a tracked entry's base. Interior
   pointers, foreign raw regions and already-released regions are refused
   without being dereferenced.
2. **Exact extent**: `bytes` must equal the tracked extent, so the unmap can
   never cover more (over-unmapping a neighbor) or less (leaking the tail of)
   the region. `my_raw_release` rounds both calls identically, so the values
   agree with the OS.
3. **Structural full-freeness**: the region must be exactly one FREE block
   spanning the whole extent — `my_block_valid` at the base, `free == 1`, no
   physical neighbors, `size == bytes`. A single live allocation anywhere in
   the region fails this check.
4. **Unlist before unmap**: the block is removed from the free list before
   its pages are released, so the list can never dangle into unmapped memory
   (`my_free_list_valid` would otherwise walk into a fault).

`my_regions_release_free` (the scavenge) applies the same check per entry.

## Invariants (Phase 12 additions)

1. Every region handed out by `my_malloc` is tracked exactly once, and every
   tracked extent is page-aligned and page-sized-rounded.
2. A tracked region's block chain never leaves the region (unchanged Phase 4
   invariant — now it also means "never leaves the registry's knowledge").
3. `my_raw_release` is called by the allocator ONLY with an exact
   tracked `{base, bytes}` pair whose region is fully free.
4. Releasing a region removes its block from the free list first.
5. Registry growth cannot lose entries (copy-then-release ordering) and
   cannot recurse into the allocator.

## Known limitations

- **Not thread-safe** (consistent with every phase so far).
- `my_malloc`/`my_free` still do not validate that a pointer is live
  (foreign/dangling pointers): freeing a pointer whose region was already
  reclaimed is undefined behaviour the allocator cannot yet detect — that is
  Phase 14's pointer validation. The updated test_free_list documents this:
  after a scavenge, old pointers must not be touched.
- A fully-free region whose block was unlisted (e.g. by
  `my_free_list_clear`) is still released by the scavenge — the registry's
  full-freeness test is structural, not list-membership-based. This is
  deliberate: unlisted-but-free blocks are unreachable, i.e. pure cache.

## API

No public API changed. The registry is an internal module:

```c
int    my_region_track(void* base, size_t bytes);         // adopt a fresh region
int    my_region_find(const void* addr, my_region* out);  // containment lookup (copy)
size_t my_region_count(void);                             // diagnostics/tests
int    my_region_release(void* base, size_t bytes);       // guarded unmapping
size_t my_regions_release_free(void);                     // scavenge fully-free regions
```

## Tests (tests/test_regions.cpp, 6 scenarios / ~18k checks)

1. **track round trip** — refusals (NULL, zero, non-page size, double
   track), containment (base/last byte/one-past-end), wrong-size release
   refused with pages still writable, correct release, double release.
2. **malloc tracking** — one region per fresh acquisition, user pointer
   inside it, free keeps it tracked and listed, reuse adds no region.
3. **multi-region + scavenge** — three live allocations occupy three
   regions; freeing keeps all three; one oversized request reclaims exactly
   those three and maps one.
4. **live blocks protect regions** — freeing one of two allocations sharing
   a region never triggers its release; the survivor's bytes survive the
   scavenge; freeing the survivor makes the next acquire reclaim it.
5. **untracked regions untouched** — a foreign raw region with a free
   whole-region block survives every scavenge (the consistency walk would
   fault if it were unmapped).
6. **long cycles** — 2000 cycles of four mixed-size allocations with rotated
   free order: payloads verified every cycle, registry bounded, list audit
   green, one final large request collapses everything to a single region.

Regression: all 13 previous suites pass unchanged under the new policy —
in particular the free-list, split, coalescing and strategy suites confirm
that `my_free` behaviour and free-list contents are identical whenever no
fresh acquisition intervenes.

## Files

- `include/mymalloc/regions.h` — registry API and guardrail contract.
- `src/regions.cpp` — growable registry, guarded release, scavenge.
- `src/allocator.cpp` — scavenge hook + tracking in `my_malloc`.
- `tests/test_regions.cpp` — the six scenarios above.
- `examples/regions_demo.cpp` — live demo of counts, scavenge and guards.
- `tests/test_free_list.cpp` — updated for the new policy (and its failure
  reporting fixed: it now ends with MYMALLOC_TEST_MAIN like every suite).
