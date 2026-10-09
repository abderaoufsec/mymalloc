// The first mymalloc allocator (Phase 5): my_malloc / my_free.
//
// Model: every allocation gets its own raw region (page-rounded), holding
// exactly one block. No splitting (Phase 7), no reuse of freed blocks
// (Phase 6), no coalescing (Phase 8), no region release (Phase 12) yet.
// See docs/phase5_allocator.md for the design and its documented limits.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include <cstdint>

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

    // Mark free. Reuse needs the free list (Phase 6); merging neighbors
    // needs coalescing (Phase 8); unmapping needs region tracking (Phase 12).
    block->free = 1;
}