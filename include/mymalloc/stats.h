// mymalloc — heap statistics and fragmentation measurement (Phase 13).
//
// Definitions (design and rationale in docs/phase13_fragmentation.md):
// - INTERNAL fragmentation: bytes inside ALLOCATED blocks the caller cannot
//   use — the per-block header, alignment padding (payload - request), and
//   slack too small to split off (the Phase 7 policy). Measured against the
//   request recorded in the block header (my_block_note_requested).
// - EXTERNAL fragmentation: FREE bytes that exist but cannot serve a large
//   request because they are spread over several blocks:
//   free_bytes - largest_free_bytes.
//
// my_heap_stats_collect() derives every field by WALKING the tracked regions
// (the Phase 12 registry) and their physical block chains — a
// derive-from-ground-truth design that cannot drift the way incremental
// counters can. The walk validates each block (my_block_valid), stays inside
// its region, ends exactly at the region end, and cross-checks the free
// block count against the free list; ANY inconsistency makes the call fail
// without writing. Collection is O(blocks): it is a diagnostic, not a hot
// path. Ratios are integer permille (0..1000, truncated) for exact,
// C-portable assertions.
//
// Not thread-safe (same policy as every phase so far).
#ifndef MYMALLOC_STATS_H
#define MYMALLOC_STATS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct my_heap_stats {
    // Regions (tracked by the Phase 12 registry).
    size_t region_count; // tracked regions
    size_t mapped_bytes; // sum of their page-rounded extents
    // Blocks (physical chains inside the regions).
    size_t block_count;        // live_blocks + free_blocks
    size_t live_blocks;        // allocated blocks
    size_t live_bytes;         // sum of live block sizes (headers included)
    size_t free_blocks;        // blocks on the free list
    size_t free_bytes;         // sum of free block sizes
    size_t largest_free_bytes; // biggest single free block (the external-
                               // fragmentation lower bound: this much of the
                               // free bytes CAN serve any one request)
    // User view of the live blocks.
    size_t requested_bytes;      // sum of recorded caller requests
    size_t payload_bytes;        // live_bytes - live_blocks * header offset
    size_t internal_slack_bytes; // payload_bytes - requested_bytes
    size_t overhead_bytes;       // block_count * header offset (never usable)
    // Fragmentation in permille (0..1000, truncated):
    //   external = 1000 * (free - largest) / free, 0 when nothing is free
    //   internal = 1000 * slack / payload,         0 when nothing is live
    unsigned int external_frag_permille;
    unsigned int internal_frag_permille;
} my_heap_stats;

// Fills *out with a snapshot of the whole heap. Returns 1 on success. Returns
// 0 — writing NOTHING — when out is NULL or the walk finds any inconsistency
// (invalid block, chain escaping or undershooting its region, free-block
// count disagreeing with the free list), so callers can use the return value
// as a coarse heap-consistency check.
int my_heap_stats_collect(my_heap_stats* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_STATS_H
