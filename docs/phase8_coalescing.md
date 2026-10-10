// mymalloc Phase 8 — coalescing design and invariants.
//
// Phase 7 added splitting: a first-fit block larger than the request is
// carved into an allocated FRONT and a listed REMAINDER. The missing half
// of a real allocator is COALESCING — when that front is later freed it
// must fuse back with its free neighbors, or every split/free cycle would
// permanently peel another fragment off the region. This phase adds that
// fuse, WITHOUT touching allocation strategy (Phase 9) or region release
// (Phase 12).
//
// ---------------------------------------------------------------------
// 1. Where adjacency comes from in this model
// ---------------------------------------------------------------------
// Regions are acquired whole (Phase 2) and, absent a split, hold exactly
// one block — so two PHYSICALLY adjacent blocks only ever exist inside a
// single region, and only ever because Phase 7 split it. That bounds the
// problem: coalescing never crosses an mmap boundary, and the physical
// neighbor chain (prev/next, Phase 4) is exactly the structure it needs.
//
// Consequence: the split of Phase 7 and the coalesce of Phase 8 are
// inverses. Split(carve front, list remainder) then free(front) round-trips
// the region back to one whole free block. That is what keeps repeated
// small allocations from fragmenting a region into many unusable pieces.
//
// ---------------------------------------------------------------------
// 2. The merge primitive: my_block_merge (block metadata layer)
// ---------------------------------------------------------------------
// Like my_block_split, fusing two blocks is a PHYSICAL operation on the
// neighbor chain, so it belongs to the block layer, not the allocator:
//
//   my_block_merge(lower, upper)        // must be linked neighbors
//       [ lower ][ upper ][ above ]
//        \_______/
//            v
//       [   merged (lower.size + upper.size)   ][ above ]
//        prev (unchanged)                         prev = merged
//        next = above                             (next unchanged)
//
//   - the SURVIVOR keeps the LOWER block's address, prev, and free flag;
//   - lower.size += upper.size; lower.next = above; above.prev = lower;
//   - the absorbed upper header is abandoned (its bytes become payload).
//
// It is deliberately free-state-agnostic: my_block_merge never inspects or
// changes `free`. Reconciling list membership and free state is the
// caller's job (see §3). This keeps the primitive small and testable and
// mirrors the header-only nature of my_block_split.
//
// Refuses (returns 0, leaving both operands untouched) when:
//   - either operand is NULL or fails my_block_valid,
//   - they are not the linked neighbor pair (lower->next != upper or
//     upper->prev != lower) — this rejects unrelated or reversed blocks,
//   - lower.size + upper.size would overflow (defensive; cannot happen for
//     two real adjacent mapped blocks).
//
// ---------------------------------------------------------------------
// 3. The coalescing policy: my_free (allocator layer)
// ---------------------------------------------------------------------
// my_free is where free STATE and list MEMBERSHIP live, so the merge policy
// is expressed there. After the Phase 5 validation and the Phase 6
// double-free guard, and after setting block->free = 1 (the freed block is
// not yet listed):
//
//   1. lower = my_block_prev(block):
//      if lower is FREE and listed, remove it from the list and merge it
//      under `block`; the survivor adopts lower's (lowest) address.
//   2. upper = my_block_next(survivor):
//      if upper is FREE and listed, remove it and merge it under the
//      survivor.
//   3. insert the single survivor (lowest address) into the list once.
//
// Why the LOWER address survives: the free list is address-ordered
// (Phase 6), and the coalesced block begins at the lowest address of the
// merge group. Keeping that address is what preserves the ordering
// invariant with a single re-insert.
//
// Why remove-then-insert rather than in-place edits: my_free_list_remove
// clears the absorbed neighbor's links, and a single insert of the survivor
// keeps count bookkeeping and the address-ordered walk consistent. The
// whole operation is guarded by my_block_valid(block) up front, so a
// neighbor that un-lists cleanly is also a neighbor that merges cleanly.
//
// Only FREE neighbors are touched. An allocated neighbor (e.g. a live front
// still splitting off the region) stays a hard boundary: coalescing stops
// at it, exactly as it should.
//
// ---------------------------------------------------------------------
// 4. Complexity and invariants
// ---------------------------------------------------------------------
//   * merge itself is O(1): constant header writes plus one neighbor relink.
//   * my_free does at most two O(1) neighbor merges plus one O(list) insert
//     walk (Phase 6). The neighbor removal is O(1) (my_free_list_remove is
//     a doubly-linked unlink). So coalescing adds no asymptotic cost to free.
//   * After any coalescing sequence the list is still strictly address-
//     ordered (single lowest-address survivor inserted), the physical chain
//     is consistent (my_block_valid holds for every block), and the count is
//     exact — all three are what my_free_list_valid checks.
//
// ---------------------------------------------------------------------
// 5. What Phase 8 deliberately does NOT do
// ---------------------------------------------------------------------
//   * Allocation strategy: my_malloc still uses first fit; best-fit etc. are
//     Phase 9. Coalescing is orthogonal to which block is chosen.
//   * Region release: a coalesced whole-region block stays MAPPED; giving
//     the memory back to the OS needs region tracking (Phase 12). Phase 8
//     reduces fragmentation WITHIN a region, it does not shrink the heap.
//   * Cross-region coalescing: impossible in this model (see §1) and not
//     needed — adjacency only arises from splits.
//   * Diagnostics: freeing an interior pointer or a foreign pointer is still
//     silently ignored (validated); loud detection is Phase 14.
//   * Thread safety: none (unchanged policy).
//
// ---------------------------------------------------------------------
// 6. Test map (tests/test_coalescing.cpp)
// ---------------------------------------------------------------------
//   * lone merge: two adjacent free blocks fuse into the lower; survivor
//     spans both, prev kept, next bridges to the old upper neighbor
//   * merge with an upper neighbor: the absorbed block's upper neighbor is
//     re-linked to the survivor (three-block chain collapses correctly)
//   * merge preserves free flags: the primitive never rewrites `free`
//   * refusals: NULL operands, non-adjacent pair, reversed pair, and a
//     corrupted (invalid) operand all return 0 and mutate nothing
//   * end-to-end: free(front) coalesces with the listed remainder so a
//     split region round-trips to ONE whole free block (count 1, not 2)
//   * fragmentation cycle: 64 split/free cycles keep count and survivor size
//     constant — the region never fragments, and the list stays valid
//
// The fragmentation cycle is the crux: without Phase 8 its count would grow
// 1,2,3,… and the survivor would shrink every cycle; with coalescing both
// stay flat, proving split and coalesce are true inverses here.