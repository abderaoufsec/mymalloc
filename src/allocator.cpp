// The mymalloc allocator: my_malloc / my_free / my_calloc / my_realloc.
//
// Model: every allocation gets its own raw region (page-rounded) holding one
// block. Freed blocks are published to the address-ordered free list and
// reused by the active placement strategy (first fit by default, best fit
// optional); an oversized reused block is split so the request takes the front
// and a worthwhile remainder stays listed; on free, a block coalesces with
// adjacent free neighbors so split/free cycles do not fragment a region;
// my_realloc resizes in place when the block fits or can absorb its free upper
// neighbor, and moves with a bounded copy otherwise. Every acquired raw region
// is tracked, and fully-free regions are returned to the OS just before fresh
// memory is acquired (scavenge-on-acquire). Every allocation path records the
// exact request in the block header so my_heap_stats_collect can measure
// internal fragmentation. See docs/architecture.md for the design.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>
#include <mymalloc/strategy.h>

#include "debug_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Global placement strategy (Phase 9). Process-wide, not thread-safe.
enum my_strategy g_strategy = MY_STRATEGY_FIRST_FIT;

// Splitting a reused free block is only worthwhile when the leftover
// remainder can stand on its own as a free-list member: a valid block
// header (my_block_user_offset()) plus room for the intrusive list links
// (sizeof(my_free_links)). A smaller leftover is kept as internal slack in
// the returned allocation instead of creating a free block too small to be
// re-listed (which my_free_list_insert would reject).
std::size_t min_split_remainder() {
    return my_block_user_offset() + sizeof(my_free_links);
}

// Picks the free block to reuse for a request needing `total` bytes (header
// included) according to the active strategy. Returns NULL when nothing fits.
my_block_header* select_fit(std::size_t total) {
    switch (g_strategy) {
    case MY_STRATEGY_BEST_FIT:
        return my_free_list_best_fit(total);
    case MY_STRATEGY_FIRST_FIT:
    default:
        return my_free_list_first_fit(total);
    }
}

// Marks `block` free, coalesces it with physically adjacent FREE neighbors,
// and publishes the single lowest-address survivor to the free list. This is
// the Phase 8 policy shared by my_free and my_realloc's in-place paths: the
// block may be freshly freed (my_free) or freshly CARVED out of an allocated
// block (my_realloc shrinking/splitting a tail), and in both cases its upper
// neighbor can already be free (a Phase 7 remainder), which must be merged in
// to keep the no-adjacent-free-blocks invariant. The freed block is not
// listed yet, so only its neighbors need un-listing; insert can only fail on
// a header too broken to validate, and staying free-but-unlisted is
// leak-safe (see docs/architecture.md).
void coalesce_and_publish(my_block_header* block) {
    block->free = 1;
    my_block_header* merged = block;

    my_block_header* const lower = my_block_prev(merged);
    if (lower != NULL && my_block_is_free(lower) == 1 && my_free_list_remove(lower) == 1 &&
        my_block_merge(lower, merged) == 1) {
        merged = lower; // survivor keeps the lowest address
    }

    my_block_header* const upper = my_block_next(merged);
    if (upper != NULL && my_block_is_free(upper) == 1 && my_free_list_remove(upper) == 1 &&
        my_block_merge(merged, upper) == 1) {
        // `merged` now spans the upper neighbor too.
    }

    static_cast<void>(my_free_list_insert(merged));
}

// --- lock-assumed internals (caller must hold the allocator lock) ----------

void* malloc_locked(size_t size) {
    // Phase 5 decision: zero-sized requests fail loudly (C allows either).
    if (size == 0) {
        return NULL;
    }

    // 1. Align the payload size, then add the header — both overflow-checked.
    //    Phase 14: when checks are enabled, reserve an extra trailing alignment
    //    unit so a canary can live just past the request (0 when checks are
    //    off, keeping the Phase 5-13 layout and sizes byte-for-byte).
    const std::size_t reserve = my_dbg_canary_reserve();
    std::size_t aligned_payload = 0;
    if (my_align_up_size(size, my_default_alignment(), &aligned_payload) != 1) {
        return NULL; // rounding the request itself would overflow
    }
    if (aligned_payload > SIZE_MAX - reserve) {
        return NULL; // request + canary reserve would overflow
    }
    aligned_payload += reserve;
    const std::size_t offset = my_block_user_offset();
    if (aligned_payload > SIZE_MAX - offset) {
        return NULL; // payload + header would overflow
    }
    const std::size_t total = aligned_payload + offset; // alignment multiple

    // Phase 6-7: reuse before acquiring — fit over the free list using the
    // active strategy (Phase 9; default first fit). The chosen block is at
    // least `total` bytes (header included). Phase 7: when the leftover
    // remainder is large enough to be a useful free block, split the fit —
    // return the front `total` bytes and keep the remainder listed; otherwise
    // reuse the fit whole (Phase 6) and absorb the small slack.
    my_block_header* const fit = select_fit(total);
    if (fit != NULL) {
        const std::size_t remainder = fit->size - total; // total <= fit->size
        if (remainder >= min_split_remainder()) {
            // Split: `fit` keeps its address as the FRONT (size total); a new
            // REMAINDER block is carved at fit->next. Both inherit free state.
            if (my_block_split(fit, total) != 1) {
                return NULL; // defensive: fit is valid and total < fit->size
            }
            my_block_header* const tail = fit->next; // the created remainder
            if (my_free_list_remove(fit) != 1) {
                return NULL; // defensive: the scan just found it listed
            }
            fit->free = 0;                                // front is now allocated
            my_block_note_requested(fit, size);           // Phase 13: record the live request
            tail->free = 1;                               // remainder stays free (explicit)
            static_cast<void>(my_free_list_insert(tail)); // remainder reusable
            my_dbg_note_alloc(fit->size);                 // Phase 14: live usage accounting
            my_dbg_canary_arm(fit);                       // Phase 14: canary (if enabled)
            return my_block_to_user(fit);
        }
        // Too small a remainder to split: hand back the whole fit (Phase 6).
        if (my_free_list_remove(fit) != 1) {
            return NULL; // cannot happen: the scan just found it listed
        }
        fit->free = 0;                      // allocated again
        my_block_note_requested(fit, size); // Phase 13: record the live request
        my_dbg_note_alloc(fit->size);       // Phase 14: live usage accounting
        my_dbg_canary_arm(fit);             // Phase 14: canary (if enabled)
        return my_block_to_user(fit);
    }

    // 2. Decide the region size up front so the raw layer and
    //    the block header agree on the real extent of the mapping.
    //    Use a minimum chunk size so that small allocations don't each
    //    cause a separate OS mapping (arena/chunk growth, Phase 18).
    std::size_t region_bytes = 0;
    const std::size_t min_chunk = my_raw_page_size() * 16U; // 16 pages minimum
    std::size_t requested_region_bytes = total > min_chunk ? total : min_chunk;
    if (my_align_up_size(requested_region_bytes, my_raw_page_size(), &region_bytes) != 1) {
        return NULL;
    }

    // 3. Reclaim fully-free regions before growing (Phase 12). Free memory
    //    lives on the free list; regions are only returned to the OS here —
    //    when they could not satisfy the request after all — so hot
    //    alloc/free paths never thrash mmap/munmap. Blocks with any live
    //    allocation keep their region (see docs/architecture.md).
    static_cast<void>(my_regions_release_free_locked());

    // 4. Acquire memory when needed (fails -> NULL).
    //    Acquire at least min_chunk bytes so the region can carve a remainder
    //    block reusable by future allocations.
    void* region = my_raw_acquire(requested_region_bytes);
    if (region == NULL) {
        return NULL;
    }

    // 5. Use the whole region as one block; trim if a platform's page size
    //    were not an alignment multiple (never true on x86/ARM, but safe).
    std::size_t block_bytes = region_bytes;
    if ((block_bytes % my_default_alignment()) != 0) {
        block_bytes -= block_bytes % my_default_alignment();
    }

    auto* block = reinterpret_cast<my_block_header*>(region);
    if (my_block_init(block, block_bytes, 0 /* allocated */, NULL, NULL) != 1) {
        my_raw_release(region, region_bytes); // cannot happen; defensive
        return NULL;
    }
    my_block_note_requested(block, size); // Phase 13: the region is this
    // request's block; record the exact ask for the fragmentation statistics

    // 6. Adopt ownership of the region (Phase 12), then carve the request
    //    from the front of the block. The remainder becomes a free block
    //    on the free list (Phase 6-7 split policy), so future allocations
    //    can reuse it without calling the OS.
    if (my_region_track_locked(region, region_bytes) != 1) {
        my_raw_release(region, region_bytes);
        return NULL;
    }

    // Carve the front `total` bytes and initialize the remainder as a free block.
    // Both the front and the remainder share the same physical starting address
    // (the region base), but have independent headers and physical neighbor links.
    if (my_block_init(block, total, 0 /* allocated */, NULL, NULL) != 1) {
        my_raw_release(region, region_bytes); // cannot happen; defensive
        return NULL;
    }
    my_block_note_requested(block, size); // Phase 13: record the live request

    // Remainder block starts at region base + total bytes.
    my_block_header* const remainder =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(block) + total);

    // Initialize the remainder: total bytes left, free, with physical chain.
    const std::size_t remainder_size = block_bytes - total;
    if (remainder_size < my_block_user_offset() || (remainder_size % my_default_alignment()) != 0) {
        // Remainder too small to be a valid block; keep the whole region allocated.
        // The allocator falls through to the existing "whole fit" path below.
        // But since we're past the reuse path, we need to handle this: unmap and fail,
        // or keep the region as one allocated block. We keep it as one allocated block:
        my_dbg_note_alloc(block->size);
        my_dbg_canary_arm(block);
        return my_block_to_user(block);
    }
    remainder->size = remainder_size;
    remainder->free = 1;
    remainder->prev = block;
    remainder->next = NULL;
    remainder->requested = 0; // fresh free block: no live request yet

    // Wire the remainder into the physical chain after the front block.
    // The front block's next pointer now points to the remainder.
    block->next = remainder;

    // Insert the remainder into the free list (address-ordered).
    // Since the remainder is at a higher address than the front block and there
    // are no other blocks in between (it's the only free block initially), it
    // becomes the new free-list head (lowest-address free block after any prior
    // free-list entries from my_regions_release_free).
    // The remainder is FREE, not live: its bytes are already reflected in
    // mapped_bytes and it is counted as live only when later split out.
    if (my_free_list_insert(remainder) != 1) {
        // Defensive: if insert fails (e.g., remainder already listed), just
        // keep the remainder unlisted but free; the region will be scavenged later.
        // Don't return NULL — the allocation still succeeds with the front block.
    }

    my_dbg_note_alloc(block->size); // Phase 14: live usage accounting (front block)
    my_dbg_canary_arm(block);       // Phase 14: canary (if enabled)
    return my_block_to_user(block);
}

void free_locked(void* ptr) {
    // free(nullptr) is a documented no-op (C requirement).
    if (ptr == NULL) {
        return;
    }

    // Phase 5: minimal validation — refuse anything that is not a block we
    // could have produced. Full foreign-pointer validation is Phase 14.
    my_block_header* block = my_user_to_block(ptr);
    if (my_block_valid(block) != 1) {
        my_dbg_note_invalid_free(ptr); // Phase 14: report a foreign/invalid free
        return;
    }

    // Phase 6: re-freeing an already-free block would double-insert it and
    // corrupt the free list. Phase 14: when checks are on this is reported as
    // a double free; either way the block stays a valid free-list member.
    if (my_block_is_free(block) == 1) {
        my_dbg_note_double_free(ptr); // Phase 14: report a double free
        return;
    }

    // Phase 14: verify the trailing canary while the block is still intact, so
    // a heap overflow that ran past the request is caught here (a no-op when
    // checks are off). We still free — an overflow cannot be undone.
    static_cast<void>(my_dbg_canary_verify(block));

    // Phase 14: drop the block from the canary registry before its bytes are
    // reused, so a stale canary is never verified later (no-op when checks off).
    my_dbg_canary_disarm(block);

    // Capture the allocated size BEFORE coalescing: the survivor grows to
    // cover freed neighbors, but those were already free (not live), so the
    // live total must drop by exactly this block's own size.
    const std::size_t freed_total = block->size;

    // Phase 8: coalesce with physically adjacent FREE neighbors before
    // publishing, so the split/free cycles of Phases 6-7 do not fragment a
    // region into many small free blocks. The earlier my_block_valid(block)
    // guarantees a consistent physical chain, so a neighbor that un-lists
    // cleanly also merges cleanly (helper defined above; shared with
    // my_realloc since Phase 11).
    coalesce_and_publish(block);

    my_dbg_note_free(freed_total); // Phase 14: live usage accounting
}

void* calloc_locked(size_t count, size_t size) {
    // Consistent with my_malloc: a zero-element or zero-size array fails
    // (documented decision; C leaves calloc(0, n) implementation-defined).
    if (count == 0 || size == 0) {
        return NULL;
    }
    // Overflow check on count * size BEFORE the multiply — the canonical
    // calloc safety property. count != 0 here, so SIZE_MAX / count is defined.
    if (count > SIZE_MAX / size) {
        return NULL;
    }
    const size_t total = count * size;

    void* pointer = malloc_locked(total);
    if (pointer == NULL) {
        return NULL;
    }
    // Phase 10: guarantee the array is zero. A FRESH raw region arrives
    // zero-filled by the OS, but REUSED free blocks (Phases 6-7) carry stale
    // bytes, so calloc must zero explicitly rather than trust the mapping.
    // Only the requested `total` bytes are defined by the C contract.
    std::memset(pointer, 0, total);
    return pointer;
}

void* realloc_locked(void* ptr, size_t new_size) {
    // realloc(NULL, new_size) is defined to be malloc(new_size).
    if (ptr == NULL) {
        return malloc_locked(new_size);
    }
    // realloc(ptr, 0) frees the block and returns NULL — consistent with
    // my_malloc(0) failing loudly (documented decision; C leaves realloc(p, 0)
    // implementation-defined, and this choice also guarantees a NULL return,
    // which every portable caller must already handle).
    if (new_size == 0) {
        free_locked(ptr);
        return NULL;
    }

    // Validation mirrors my_free: a pointer we could not have produced cannot
    // be resized, so refuse loudly instead of trusting its header (full
    // foreign-pointer validation is Phase 14). A block that is already FREE
    // (a dangling pointer) is refused too — handing free memory back as a
    // live allocation would be worse than returning NULL.
    my_block_header* block = my_user_to_block(ptr);
    if (my_block_valid(block) != 1) {
        return NULL;
    }
    if (my_block_is_free(block) == 1) {
        return NULL;
    }

    // Phase 14: the block's size before any in-place resize, so the live-total
    // accounting can move by exactly the delta (the move path self-accounts via
    // its own my_malloc/my_free and never reaches the resize hooks).
    const std::size_t old_total = block->size;

    // Size computation identical to my_malloc: align the payload, add the
    // header, both overflow-checked. An impossible request fails here WITHOUT
    // touching the original block — the C requirement for realloc failure is
    // that the caller's pointer stays valid and unchanged.
    std::size_t aligned_payload = 0;
    if (my_align_up_size(new_size, my_default_alignment(), &aligned_payload) != 1) {
        return NULL;
    }
    // Phase 14: reserve canary space exactly as my_malloc does, so a resized
    // block keeps room for its trailing canary (0 when checks are off, which
    // keeps the Phase 11 layout and sizes byte-for-byte).
    const std::size_t reserve = my_dbg_canary_reserve();
    if (aligned_payload > SIZE_MAX - reserve) {
        return NULL; // request + canary reserve would overflow
    }
    aligned_payload += reserve;
    const std::size_t offset = my_block_user_offset();
    if (aligned_payload > SIZE_MAX - offset) {
        return NULL;
    }
    const std::size_t total = aligned_payload + offset;

    // --- fits: shrink, same size, or grow within existing slack ------------
    // total <= block->size, so the whole resize happens at the header: the
    // pointer (and every prefix byte the caller holds) stays put.
    if (total <= block->size) {
        // Phase 13: record the new live request BEFORE the optional split, so
        // the header stays self-consistent (requested <= payload) throughout.
        my_block_note_requested(block, new_size);
        const std::size_t remainder = block->size - total;
        if (remainder >= min_split_remainder()) {
            // Carve the slack off as a new block and publish it through the
            // same coalescing path as my_free. The block being split is
            // ALLOCATED, so the carve-out inherits that state and must be
            // flipped to free; its upper neighbor may already be free (a
            // Phase 7 remainder), and two adjacent free blocks would break
            // the Phase 8 invariant the coalescer maintains.
            if (my_block_split(block, total) == 1) {
                coalesce_and_publish(block->next);
            }
        }
        // Below the split threshold the slack stays internal (Phase 7
        // policy): slightly oversized allocation beats an unusable sliver.
        my_dbg_note_resize(old_total, block->size); // Phase 14: live usage delta
        my_dbg_canary_restamp(block);               // Phase 14: canary moved
        return ptr;                                 // same pointer, prefix data preserved
    }

    // --- grow in place: absorb a sufficient FREE upper neighbor ------------
    // Only the block above can extend this one without moving the pointer
    // (absorbing the block below would relocate the payload). Sufficiency is
    // checked BEFORE any mutation: total > block->size here, so the
    // subtraction cannot underflow, and an insufficient (or absent) neighbor
    // falls through to the move path with all metadata untouched. The
    // remove-then-merge chain mirrors my_free: a neighbor that un-lists
    // cleanly also merges cleanly.
    my_block_header* const upper = my_block_next(block);
    if (upper != NULL && my_block_is_free(upper) == 1 &&
        upper->size >= total - block->size && // enough bytes together
        my_free_list_remove(upper) == 1 && my_block_merge(block, upper) == 1) {
        // block now spans the absorbed neighbor (and stays allocated).
        const std::size_t remainder = block->size - total;
        if (remainder >= min_split_remainder()) {
            if (my_block_split(block, total) == 1) {
                coalesce_and_publish(block->next); // give the tail back
            }
        }
        my_block_note_requested(block, new_size);   // Phase 13: grown in place
        my_dbg_note_resize(old_total, block->size); // Phase 14: live usage delta
        my_dbg_canary_restamp(block);               // Phase 14: canary moved
        return ptr;                                 // same pointer, prefix data preserved
    }

    // --- move: allocate fresh, copy the overlap, free the old block --------
    // Neither the block nor its neighborhood could satisfy the request. A
    // failed my_malloc returns NULL with the ORIGINAL block untouched (the C
    // requirement); on success only the overlap min(old payload, new size)
    // bytes carries over — exactly what the contract promises the caller.
    // The whole move happens under the single lock already held: malloc_locked
    // and free_locked assume it, so there is no self-deadlock and no
    // unlock/relock window that could expose a data race.
    void* fresh = malloc_locked(new_size);
    if (fresh == NULL) {
        return NULL; // original block untouched
    }
    const std::size_t old_payload = my_block_payload_size(block);
    const std::size_t copy = old_payload < new_size ? old_payload : new_size;
    std::memcpy(fresh, ptr, copy);
    free_locked(ptr); // old block rejoins the free list (coalesced, Phase 8)
    return fresh;
}

} // namespace

// --- public wrappers: take the allocator lock exactly once ----------------

void* my_malloc(size_t size) {
    allocator_lock lock;
    return malloc_locked(size);
}

void my_free(void* ptr) {
    allocator_lock lock;
    free_locked(ptr);
}

void* my_calloc(size_t count, size_t size) {
    allocator_lock lock;
    return calloc_locked(count, size);
}

void* my_realloc(void* ptr, size_t new_size) {
    allocator_lock lock;
    return realloc_locked(ptr, new_size);
}

void my_set_strategy(enum my_strategy strategy) {
    allocator_lock lock;
    // Ignore unknown values so a bogus cast cannot silently disable reuse.
    if (strategy == MY_STRATEGY_FIRST_FIT || strategy == MY_STRATEGY_BEST_FIT) {
        g_strategy = strategy;
    }
}

enum my_strategy my_get_strategy(void) {
    allocator_lock lock;
    return g_strategy;
}
