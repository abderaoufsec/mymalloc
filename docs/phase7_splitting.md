// mymalloc Phase 7 — splitting design and invariants.
//
// Phase 6 added an address-ordered free list so my_malloc could REUSE a
// freed block by first fit. It deliberately reused such a block WHOLE: a
// 1-page free block handed to a 1-byte request kept all 4 KiB, the slack
// staying as internal fragmentation. This phase removes that waste with
// SPLITTING, without touching coalescing (Phase 8) or region release
// (Phase 12).
//
// ---------------------------------------------------------------------
// 1. What Phase 7 changes
// ---------------------------------------------------------------------
//   my_malloc, on a first-fit hit of `total` bytes in a block of size N:
//       N - total >= MIN_REMAINDER  ->  SPLIT (this phase)
//                                        front (total)   = allocation
//                                        tail  (N-total) = stays listed
//       N - total <  MIN_REMAINDER  ->  reuse whole (Phase 6 path, unchanged)
//
// MIN_REMAINDER = my_block_user_offset() + sizeof(my_free_links):
// the smallest leftover that is still (a) a legal block header and (b)
// able to hold the intrusive free-list links. A smaller leftover would be
// created only to be rejected by my_free_list_insert, so it is left as
// internal slack instead. On x86-64: 32 + 16 = 48 bytes.
//
// ---------------------------------------------------------------------
// 2. The split primitive: my_block_split (block metadata layer)
// ---------------------------------------------------------------------
// Splitting is a PHYSICAL operation on the neighbor chain (Phase 4), so it
// belongs to the block layer, not the allocator:
//
//   my_block_split(block, front_size)
//       [ size | free | prev | next ][ ...payload... ]
//        \_________________ ________________/
//                        v
//       [ front: size=front_size ]  [ remainder: size=N-front_size ]
//        prev  (unchanged)            prev = front
//        next  = remainder            next = old next
//       old_upper->prev = remainder
//
//   - FRONT keeps the block's address, prev, and free state (only size and
//     next change). Reusing the address is what makes the returned user
//     pointer equal to the original block's — an existing allocation
//     identity is preserved.
//   - REMAINDER occupies the exact tail, inherits the free state, and is
//     wired between the front and the old upper neighbor. All neighbor
//     links are repaired, so my_block_valid holds for every block.
//   - Both halves inherit `block`'s free state; the CALLER decides each
//     state (the allocator sets front=allocated, remainder=free).
//
// This touches ONLY headers and physical links. Free-list membership is the
// caller's responsibility — splitting never edits the list (that separation
// keeps the block layer free of Phase 6 state).
//
// Validation (returns 0, leaving the block UNTOUCHED) when:
//   - block is NULL or fails my_block_valid,
//   - front_size is 0, not a multiple of my_default_alignment(), or
//     < my_block_user_offset(),
//   - front_size >= block->size (no remainder would be left),
//   - the remainder (block->size - front_size) < my_block_user_offset().
// Because every accepted size is an alignment multiple and block starts are
// alignment-aligned, the remainder header lands on the default alignment —
// my_block_init/valid never trip on a split-produced block.
//
// ---------------------------------------------------------------------
// 3. Allocator wiring (src/allocator.cpp)
// ---------------------------------------------------------------------
// On the first-fit hit the allocator, when splitting:
//   1. my_block_split(fit, total)         -> front `fit`, remainder `fit->next`
//   2. my_free_list_remove(fit)           -> the front leaves the list
//   3. fit->free = 0                      -> front allocated
//   4. tail->free = 1; my_free_list_insert(tail) -> remainder reusable
//   5. return my_block_to_user(fit)
// When the remainder is too small, the Phase 6 whole-reuse path runs
// unchanged. Acquiring a fresh region (Phase 5) is untouched and remains the
// fallback when the list has no fit.
//
// Interaction with the free list: after a split the list holds the remainder
// at a HIGHER address than the (now unlisted) front. Since the list is
// strictly ascending (Phase 6), and insert walks by address, the remainder
// is inserted in its correct place automatically — no reordering needed.
//
// ---------------------------------------------------------------------
// 4. What Phase 7 deliberately does NOT do
// ---------------------------------------------------------------------
//   * Coalescing: a freed neighbor is not merged back into an adjacent free
//     block (Phase 8). Splitting therefore fragments a region over time —
//     the exact problem coalescing later solves.
//   * Region release: remainders keep regions mapped (Phase 12).
//   * Best fit / size segregation: still pure first fit (lowest address).
//   * Diagnostics: an oversized-but-fitting block is split silently; no
//     fragmentation reporting (Phase 13) or corruption diagnostics (Phase
//     14).
//   * Thread safety: none (unchanged since Phase 2).
//
// ---------------------------------------------------------------------
// 5. Test map (tests/test_split.cpp)
// ---------------------------------------------------------------------
//   * lone split: front keeps address/size/free/prev; remainder is the exact
//     tail with correct links; sizes sum to the original.
//   * middle split: the upper neighbor is re-linked to the remainder; all
//     four blocks validate.
//   * last-block split: no upper neighbor to re-link; links stay correct.
//   * refusals: front_size 0 / unaligned / too small / >= size / remainder
//     too small / NULL / misaligned — each returns 0 and leaves the block
//     byte-for-byte unchanged.
//   * split reuse (end-to-end): freeing a full-page block then allocating
//     16 bytes yields an allocated front of 48 bytes and a listed remainder
//     of the rest; a 64-byte request reuses the remainder without growing.
//   * remainder-too-small: a request whose leftover is just under the
//     threshold reuses the 8192-byte block WHOLE (no remainder created).
//
// ---------------------------------------------------------------------
// 6. Demo (examples/split_demo.cpp)
// ---------------------------------------------------------------------
// Prints the oversized block, the split front + remainder with sizes that
// sum to the region, the reuse of the remainder, and the whole-reuse case
// when the leftover would be too small to be a block.