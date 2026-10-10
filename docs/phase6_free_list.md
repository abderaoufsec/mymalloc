// mymalloc Phase 6 — free list design and invariants.
//
// Phase 5 shipped my_malloc/my_free with a region-per-block model: every
// allocation got its own page-rounded mmap region, freed memory was only
// marked (never recovered), and the heap therefore never shrinks (LOWMEM
// demonstration with 200 blocks). This phase removes the "no reuse"
// limitation by adding a free list, WITHOUT introducing splitting,
// coalescing or region release.
//
// ---------------------------------------------------------------------
// 1. The model that Phase 6 fixes
// ---------------------------------------------------------------------
//   Before (Phase 5): my_malloc -> my_raw_acquire -> one block per region
//                     my_free    -> block->free = 1 (block stays invisible)
//   After  (Phase 6): my_free    -> block->free = 1 AND publish to free list
//                     my_malloc  -> first fit over the list BEFORE acquiring
//                                  -> fallback to my_raw_acquire (Phase 5
//                                     path, unchanged) when nothing fits
//
// Consequence: a same-size alloc/free cycle no longer grows the resident
// set; the freed region is handed back out by first fit.
//
// ---------------------------------------------------------------------
// 2. Where the links live (zero header growth)
// ---------------------------------------------------------------------
// The block header stays 32 bytes on x86-64 (size, free, prev, next).
// The free-list links (my_free_links: 2 pointers) are overlaid on the
// free block's PAYLOAD — precisely the bytes the block.h §8 note reserved:
//
//   block start                     user pointer (= links overlay)
//   v                               v
//   [ size | free | prev | next ][ fl_prev | fl_next | ...unused... ]
//   <---------- size ------------->
//
// The header's prev/next remain purely PHYSICAL neighbors (Phase 4).
// Logical free-list membership lives ONLY in the payload, and only while
// the block is free: allocated blocks pay zero extra bytes.
//
// Why overlay instead of a global registry (size_t -> block)? (a) the
// header must stay fixed for ABI stability of the documented layout;
// (b) payload space of a free block is otherwise wasted; (c) a registry
// keyed on size alone cannot answer "is THIS pointer listed?".
//
// Safety rules that follow from overlay:
//   * insert() NEVER READS the existing payload bytes — it overwrites
//     both link fields, so stale user data can never be parsed as list
//     state. This is what makes freeing a block twice (after the guard)
//     or freeing after payload corruption harmless at the list level.
//   * a block is only listed while free == 1, and insert() refuses any
//     block whose payload cannot hold my_free_links (size < offset +
//     sizeof(links)) — so links always fit within the block.
//   * remove() clears both links; a block outside the list has NULL links,
//     which is exactly the invariant my_malloc/my_free rely on.
//
// ---------------------------------------------------------------------
// 3. Ordering: strictly ascending addresses
// ---------------------------------------------------------------------
// insert() performs ONE address-ordered walk that simultaneously proves
// non-membership and locates the position (double-insert guard). The list
// is therefore a sorted structure, not an append queue:
//
//   * FIRST FIT == LOWEST-ADDRESS FIT. my_free_list_first_fit(n) returns
//     the first block with size >= n — deterministically the lowest
//     address. This gives a stable, reproducible placement policy.
//   * strictly ascending order rules out cycles: any cycle would have to
//     contain a backwards step.
//   * the walk is bounded by the count, which insert/remove maintain
//     exactly, so corruption cannot loop the walk forever.
//
// Layout consequence: block regions arrive at arbitrary addresses from
// mmap, so list order != allocation order. Tests and the demo never
// assume the address of a fresh region.
//
// ---------------------------------------------------------------------
// 4. my_free_list_valid(): the machine-checked audit
// ---------------------------------------------------------------------
// Ordered defensively like my_block_valid: every address-level fact is
// established BEFORE any link is followed.
//   1. every listed block passes my_block_valid AND has free == 1,
//   2. its payload can hold my_free_links,
//   3. entries appear in strictly ascending address order (also rules out
//      cycles),
//   4. fl_prev/fl_next are reciprocal with the walk order and NULL at the
//      ends,
//   5. the walk visits exactly my_free_list_count() entries.
// The count is checked last because it is the only invariant that needs a
// completed walk.
//
// ---------------------------------------------------------------------
// 5. API (include/mymalloc/free_list.h)
// ---------------------------------------------------------------------
//   my_free_list_insert(block)    1/0 — address-ordered, double-insert-safe
//   my_free_list_remove(block)    1/0 — clears links; not-a-member is 0
//   my_free_list_first_fit(bytes) block*/NULL — lowest-address fit
//   my_free_list_head()           block*/NULL
//   my_free_list_count()          size_t (exact)
//   my_free_list_valid()          1/0 consistency audit
//   my_free_list_clear()          drop the list without touching blocks
//
// ---------------------------------------------------------------------
// 6. Allocator wiring (src/allocator.cpp)
// ---------------------------------------------------------------------
// my_malloc: after overflow-safe sizing (unchanged), call
// my_free_list_first_fit(total) BEFORE my_raw_acquire. On a hit: remove
// from the list, mark allocated, return the payload. The fit is never
// shrunk — splitting is Phase 7 — so internal slack is preserved and
// LISTED blocks are simply re-used whole.
// my_free: after the existing my_block_valid check, guard against
// re-freeing an already-free block (double-insert corruption), then
// block->free = 1; my_free_list_insert(block).
//
// Consequence for the Phase 5 tests: nothing changes — reuse is invisible
// to isolation/alignment/failure assertions. The Phase 5 "no reuse"
// documentation is now scoped: reuse exists, SPLITTING does not.
//
// ---------------------------------------------------------------------
// 7. What Phase 6 deliberately does NOT do
// ---------------------------------------------------------------------
//   * Splitting: a 1-page block reused for 1 byte keeps its 4 KiB region.
//   * Coalescing: neighbors are not merged on free (Phase 8).
//   * Region release: freed regions stay mapped (Phase 12); the Phase 5
//     LOWMEM demonstration is unaffected except for reused allocations.
//   * Diagnostics: double free is silently ignored here (guarded); loud
//     detection arrives in Phase 14.
//   * Thread safety: none (same policy as every phase so far).
//
// ---------------------------------------------------------------------
// 8. Test map (tests/test_free_list.cpp)
// ---------------------------------------------------------------------
//   * empty-state contracts (head/count/valid/first-fit/insert/remove)
//   * insert: address ordering after scrambled inserts, double-insert
//     refusal, non-member removal returning 0, link reciprocity
//   * remove: head removal, cleared links, count bookkeeping
//   * insert refusals: allocated block, too-small block, NULL
//   * first fit: lowest-address adequate block; too-small request finds
//     nothing; scans never mutate; SIZE_MAX and 0 requests are safe
//   * audit corruption detection: free bit flipped, reciprocity broken,
//     chain severed, count mismatch (and restoration)
//   * end-to-end: alloc/free same size reuses the block (p2 == p1), large
//     request bypasses a too-small list entry, double free adds no entry,
//     mixed-size draining/refilling stays consistent, 64-cycle reuse loop
//
// The 64-cycle loop doubles as a stability/ASan check: the list must
// remain valid and the resident set must not grow while cycling.