// mymalloc — managed region registry (Phase 12).
//
// Until Phase 12 the allocator acquired raw regions (my_raw_acquire) but
// never recorded them, so OS memory could only ever grow: a fully freed
// region stayed mapped and listed forever. This module gives mymalloc
// OWNERSHIP of its regions: every region the allocator acquires is tracked
// here as a {base, bytes} entry, and a region whose blocks are all freed may
// be returned to the OS through my_region_release.
//
// Storage: a growable table of entries whose buffer is itself acquired via
// my_raw_acquire (NEVER my_malloc — allocator metadata must not recurse into
// the allocator). The table starts small and doubles while needed, so there
// is no artificial cap on simultaneously live regions.
//
// Release guardrails (every one enforced again inside my_region_release, so
// no caller can cause an invalid unmapping):
//   1. the region must be TRACKED — `base` must match a registry entry
//      exactly (regions tests or other code acquired directly through
//      my_raw_acquire are invisible here and are never unmapped),
//   2. `bytes` must equal the tracked extent (refuses partial/oversized
//      unmaps of a correctly tracked region),
//   3. the region must be FULLY FREE: its single block starts at `base`,
//      passes my_block_valid, is free, has no physical neighbors and spans
//      the whole extent (a region with any live allocation is refused),
//   4. the block is removed from the free list BEFORE unmapping, so the
//      list can never dangle into released memory.
//
// The policy decision (WHEN the allocator gives regions back) lives in
// src/allocator.cpp — scavenge-on-acquire, see docs/phase12_heap_management.md.
//
// Public entry points serialize on the allocator lock and may be called
// concurrently with allocation.
#ifndef MYMALLOC_REGIONS_H
#define MYMALLOC_REGIONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One tracked raw region: [base, base + bytes). `bytes` is always the
// page-rounded extent that was (or must be) passed to my_raw_release.
typedef struct my_region {
    void* base;
    size_t bytes;
} my_region;

// Starts tracking the region [base, base + bytes). Returns 1 on success, 0
// when: base is NULL, bytes is 0 or not a whole number of pages, the region
// is already tracked, or the internal table could not be (re)allocated.
int my_region_track(void* base, size_t bytes);

// Copies the tracked region containing `addr` (base <= addr < base+bytes)
// into *out. Returns 1 on success, 0 when no tracked region contains the
// address (including when out is NULL).
int my_region_find(const void* addr, my_region* out);

// Number of currently tracked regions (exact; maintained by
// track/release).
size_t my_region_count(void);

// Copies the entry at zero-based `index` into *out. Returns 1 on success, 0
// when out is NULL or index is out of range. Entries are packed but
// swap-remove makes the ORDER unspecified — iteration must not assume
// address order. This exists so diagnostics can walk every tracked region
// (Phase 13's statistics collector, Phase 14's consistency checker).
int my_region_at(size_t index, my_region* out);

// Releases the tracked region that STARTS at exactly `base`, provided
// `bytes` matches the tracked extent and the region is fully free (see the
// guardrail list above). Un-lists the block, unmaps the pages and stops
// tracking the region. Returns 1 when the region was released, 0 when the
// call was refused — a refused call unmaps nothing and changes nothing.
int my_region_release(void* base, size_t bytes);

// Scavenges the registry: releases EVERY fully free region (the same
// guardrails as my_region_release apply). Returns the number of regions
// released. Used by my_malloc just before acquiring fresh memory.
size_t my_regions_release_free(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_REGIONS_H