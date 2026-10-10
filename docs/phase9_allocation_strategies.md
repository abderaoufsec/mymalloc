// mymalloc Phase 9 — allocation strategies (first fit vs best fit).
//
// Phases 6-8 built the reuse machinery: freed blocks go on an intrusive,
// address-ascending free list (Phase 6), an oversized reused block is split
// (Phase 7), and freed neighbors coalesce (Phase 8). Through all of that,
// my_malloc reused blocks by FIRST FIT — it took the lowest-address block
// that was big enough and stopped. This phase adds a *choice* of placement
// strategy on top of the unchanged structure, and documents the default.
//
// ---------------------------------------------------------------------
// 1. What a placement strategy is (and is not)
// ---------------------------------------------------------------------
// A placement strategy answers exactly one question: given a request that
// needs `total` bytes (header included), WHICH free block should my_malloc
// take? It does not change the free list, splitting, or coalescing — those
// are Phases 6-8 and are reused verbatim. Phase 9 only selects the fit.
//
// Two strategies:
//   * FIRST_FIT — the lowest-address block whose size >= total. Scans from
//     the head and returns on the first hit: early exit, O(k) where k is the
//     number of blocks skipped.
//   * BEST_FIT  — the SMALLEST block whose size >= total. Must scan the whole
//     list to prove no smaller adequate block exists: O(n) per allocation.
//
// Because the list is strictly address-ascending (Phase 6), FIRST_FIT is
// deterministic (always the lowest-address fit) and BEST_FIT ties break by
// lowest address automatically.
//
// ---------------------------------------------------------------------
// 2. Free-list primitives (free_list.h)
// ---------------------------------------------------------------------
//   my_free_list_first_fit(min)  — unchanged from Phase 6 (lowest-address fit)
//   my_free_list_best_fit(min)   — NEW: smallest adequate block, ties by
//                                   lowest address; full scan, does not mutate
//
// Both return NULL when nothing fits and NULL for a zero request (defensive:
// a real request always includes the header, so min_block_size > 0).
//
// my_free_list_best_fit scans every member and keeps the strictly-smallest
// winner; on equal sizes it never replaces, so the first (lowest-address)
// block of the winning size is kept.
//
// ---------------------------------------------------------------------
// 3. The selector (strategy.h)
// ---------------------------------------------------------------------
//   enum my_strategy { MY_STRATEGY_FIRST_FIT = 0, MY_STRATEGY_BEST_FIT = 1 };
//   void        my_set_strategy(enum my_strategy);
//   enum my_strategy my_get_strategy(void);
//
// The strategy is a single process-wide variable (in allocator.cpp) with NO
// synchronization — same thread-safety policy as every phase so far.
// my_set_strategy IGNORES unknown values so a bogus cast
// (e.g. static_cast<my_strategy>(99)) cannot silently disable reuse.
//
// ---------------------------------------------------------------------
// 4. Allocator wiring (allocator.cpp)
// ---------------------------------------------------------------------
// select_fit(total) switches on g_strategy and calls the matching primitive;
// my_malloc uses select_fit instead of calling first_fit directly. Everything
// after the fit — remove-from-list, split if the remainder is worthwhile
// (Phase 7), hand back the front — is unchanged and shared by both
// strategies. So the ONLY behavioral difference is which block is chosen.
//
// ---------------------------------------------------------------------
// 5. The tradeoff, and why FIRST_FIT is the default
// ---------------------------------------------------------------------
// BEST_FIT minimizes per-placement leftover (internal) slack and tends to
// preserve large free blocks for large future requests. Its cost is a full
// O(n) scan per allocation. In the region-per-block model (Phase 5), every
// block is its own page-rounded mmap region, so free lists stay SHORT and
// large-block preservation matters far less than the constant per-alloc
// scan — the classic best-fit win does not materialize here. FIRST_FIT is
// therefore kept as the default (unchanged since Phase 6); best fit is
// opt-in for measurement/comparison, exactly as the TODO ("implement best-fit
// optionally") intends.
//
// When lists ARE long (Phase 12 heap management / many small blocks), the
// calculus can change — that is the documented revisit point, not now.
//
// ---------------------------------------------------------------------
// 6. What Phase 9 deliberately does NOT do
// ---------------------------------------------------------------------
//   * No segregation / size-class bins, no address-ordered fast paths.
//   * No per-strategy statistics yet (raw cost is O(1) vs O(n) by inspection;
//     measured numbers land with the benchmarks in Phase 16).
//   * No thread safety (selector is global, unsynchronized).
//   * The strategy affects only REUSE placement; a miss still acquires a fresh
//     page-rounded region exactly as in Phase 5.
//
// ---------------------------------------------------------------------
// 7. Test map (tests/test_strategy.cpp)
// ---------------------------------------------------------------------
//   * my_free_list_best_fit: picks the smallest adequate block; agrees with
//     first fit when only one block fits; returns NULL when none fits; scan
//     is pure (list unchanged)
//   * selector: defaults to FIRST_FIT; set/get round-trips; unknown values
//     are ignored
//   * end-to-end: under BEST_FIT my_malloc takes the smaller block and leaves
//     the large one intact; under the FIRST_FIT default the same cycle still
//     reuses and stays consistent
//
// ---------------------------------------------------------------------
// 8. API summary
// ---------------------------------------------------------------------
//   my_free_list_best_fit(min)  (free_list.h) — smallest adequate block
//   my_set_strategy / my_get_strategy (strategy.h) — process-wide selector
//
// Default = MY_STRATEGY_FIRST_FIT. See examples/strategy_demo.cpp for a
// side-by-side run of both strategies over the same free/free/alloc cycle.