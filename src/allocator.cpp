// The mymalloc allocator (Phases 5-11): my_malloc / my_free / my_calloc /
// my_realloc.
//
// Model: every allocation gets its own raw region (page-rounded) holding one
// block. Freed blocks are published to the free list and reused by the active
// placement strategy (Phase 9; default first fit since Phase 6, optional best
// fit); an oversized reused block is SPLIT so the request takes the front and
// a worthwhile remainder stays listed (Phase 7); on free, a block COALESCES
// with adjacent free neighbors so split/free cycles do not fragment a region
// (Phase 8); my_realloc resizes in place when the block fits or can absorb its
// free upper neighbor, and moves with a bounded copy otherwise (Phase 11); no
// region release (Phase 12) yet. See docs/phase11_realloc.md for the resize
// design, docs/phase9_allocation_strategies.md for the strategy choice,
// docs/phase8_coalescing.md for the merge policy, docs/phase7_splitting.md
// for the split policy, docs/phase6_free_list.md for the reuse design and
// docs/phase5_allocator.md for the base model and its limits.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/strategy.h>

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
// leak-safe (see docs/phase8_coalescing.md).
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

} // namespace

void my_set_strategy(enum my_strategy strategy) {
    // Ignore unknown values so a bogus cast cannot silently disable reuse.
    if (strategy == MY_STRATEGY_FIRST_FIT || strategy == MY_STRATEGY_BEST_FIT) {
        g_strategy = strategy;
    }
}

enum my_strategy my_get_strategy(void) {
    return g_strategy;
}

void* my_malloc(size_t size) {
    // Phase 5 decision: zero-sized requests fail loudly (C allows either).
    if (size == 0) {
        return NULL;
    }

    // 1. Align the payload size, then add the header — both overflow-checked.
    std::size_t aligned_payload = 0;
    if (my_align_up_size(size, my_default_alignment(), &aligned_payload) != 1) {
        return NULL; // rounding the request itself would overflow
    }
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
            tail->free = 1;                               // remainder stays free (explicit)
            static_cast<void>(my_free_list_insert(tail)); // remainder reusable
            return my_block_to_user(fit);
        }
        // Too small a remainder to split: hand back the whole fit (Phase 6).
        if (my_free_list_remove(fit) != 1) {
            return NULL; // cannot happen: the scan just found it listed
        }
        fit->free = 0; // allocated again
        return my_block_to_user(fit);
    }

    // 2. Decide the region size up front (page-rounded) so the raw layer and
    //    the block header agree on the real extent of the mapping.
    std::size_t region_bytes = 0;
    if (my_align_up_size(total, my_raw_page_size(), &region_bytes) != 1) {
        return NULL;
    }

    // 3. Acquire memory when needed (fails -> NULL).
    void* region = my_raw_acquire(total);
    if (region == NULL) {
        return NULL;
    }

    // 4. Use the whole region as one block; trim if a platform's page size
    //    were not an alignment multiple (never true on x86/ARM, but safe).
    std::size_t block_bytes = region_bytes;
    if ((block_bytes % my_default_alignment()) != 0) {
        block_bytes -= block_bytes % my_default_alignment();
    }

    auto* block = reinterpret_cast<my_block_header*>(region);
    if (my_block_init(block, block_bytes, 0 /* allocated */, NULL, NULL) != 1) {
        my_raw_release(region, total); // cannot happen; defensive
        return NULL;
    }

    // 5. Return the aligned payload pointer.
    return my_block_to_user(block);
}

void my_free(void* ptr) {
    // free(nullptr) is a documented no-op (C requirement).
    if (ptr == NULL) {
        return;
    }

    // Phase 5: minimal validation — refuse anything that is not a block we
    // could have produced. Full foreign-pointer validation is Phase 14.
    my_block_header* block = my_user_to_block(ptr);
    if (my_block_valid(block) != 1) {
        return;
    }

    // Phase 6: re-freeing an already-free block would double-insert it and
    // corrupt the free list. Ignore it silently for now (diagnostics land in
    // Phase 14); the block stays a valid free-list member either way.
    if (my_block_is_free(block) == 1) {
        return;
    }

    // Phase 8: coalesce with physically adjacent FREE neighbors before
    // publishing, so the split/free cycles of Phases 6-7 do not fragment a
    // region into many small free blocks. The earlier my_block_valid(block)
    // guarantees a consistent physical chain, so a neighbor that un-lists
    // cleanly also merges cleanly (helper defined above; shared with
    // my_realloc since Phase 11).
    coalesce_and_publish(block);
}

void* my_calloc(size_t count, size_t size) {
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

    void* pointer = my_malloc(total);
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

void* my_realloc(void* ptr, size_t new_size) {
    // realloc(NULL, new_size) is defined to be malloc(new_size).
    if (ptr == NULL) {
        return my_malloc(new_size);
    }
    // realloc(ptr, 0) frees the block and returns NULL — consistent with
    // my_malloc(0) failing loudly (documented decision; C leaves realloc(p, 0)
    // implementation-defined, and this choice also guarantees a NULL return,
    // which every portable caller must already handle).
    if (new_size == 0) {
        my_free(ptr);
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

    // Size computation identical to my_malloc: align the payload, add the
    // header, both overflow-checked. An impossible request fails here WITHOUT
    // touching the original block — the C requirement for realloc failure is
    // that the caller's pointer stays valid and unchanged.
    std::size_t aligned_payload = 0;
    if (my_align_up_size(new_size, my_default_alignment(), &aligned_payload) != 1) {
        return NULL;
    }
    const std::size_t offset = my_block_user_offset();
    if (aligned_payload > SIZE_MAX - offset) {
        return NULL;
    }
    const std::size_t total = aligned_payload + offset;

    // --- fits: shrink, same size, or grow within existing slack ------------
    // total <= block->size, so the whole resize happens at the header: the
    // pointer (and every prefix byte the caller holds) stays put.
    if (total <= block->size) {
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
        return ptr; // same pointer, prefix data preserved
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
        return ptr; // same pointer, prefix data preserved
    }

    // --- move: allocate fresh, copy the overlap, free the old block --------
    // Neither the block nor its neighborhood could satisfy the request. A
    // failed my_malloc returns NULL with the ORIGINAL block untouched (the C
    // requirement); on success only the overlap min(old payload, new size)
    // bytes carries over — exactly what the contract promises the caller.
    void* fresh = my_malloc(new_size);
    if (fresh == NULL) {
        return NULL; // original block untouched
    }
    const std::size_t old_payload = my_block_payload_size(block);
    const std::size_t copy = old_payload < new_size ? old_payload : new_size;
    std::memcpy(fresh, ptr, copy);
    my_free(ptr); // old block rejoins the free list (coalesced, Phase 8)
    return fresh;
}