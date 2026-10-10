// The mymalloc allocator (Phases 5-7): my_malloc / my_free.
//
// Model: every allocation gets its own raw region (page-rounded) holding one
// block. Freed blocks are published to the free list and reused first-fit
// (Phase 6); an oversized reused block is SPLIT so the request takes the
// front and a worthwhile remainder stays listed (Phase 7); no coalescing
// (Phase 8), no region release (Phase 12) yet. See docs/phase7_splitting.md
// for the split policy, docs/phase6_free_list.md for the reuse design and
// docs/phase5_allocator.md for the base model and its limits.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include <cstdint>

namespace {

// Splitting a reused free block is only worthwhile when the leftover
// remainder can stand on its own as a free-list member: a valid block
// header (my_block_user_offset()) plus room for the intrusive list links
// (sizeof(my_free_links)). A smaller leftover is kept as internal slack in
// the returned allocation instead of creating a free block too small to be
// re-listed (which my_free_list_insert would reject).
std::size_t min_split_remainder() {
    return my_block_user_offset() + sizeof(my_free_links);
}

} // namespace

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

    // Phase 6-7: reuse before acquiring — first fit over the free list. The
    // chosen block is at least `total` bytes (header included). Phase 7: when
    // the leftover remainder is large enough to be a useful free block, split
    // the fit — return the front `total` bytes and keep the remainder listed;
    // otherwise reuse the fit whole (Phase 6) and absorb the small slack.
    my_block_header* const fit = my_free_list_first_fit(total);
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

    // Mark free and publish it to the free list for first-fit reuse.
    // Merging neighbors needs coalescing (Phase 8); unmapping the region
    // needs region tracking (Phase 12).
    block->free = 1;
    // Defensive: insert can only fail on a header too broken to validate
    // (checked above). Staying free-but-unlisted is leak-safe; corrupting
    // the list would not be.
    static_cast<void>(my_free_list_insert(block));
}