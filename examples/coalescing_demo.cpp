// mymalloc Phase 8 — coalescing adjacent free blocks experiment.
//
// Phase 7 let my_malloc SPLIT an oversized free block into an allocated
// front plus a listed remainder. Without the other half of the story, a
// region would slowly fragment into many small free blocks as fronts are
// returned. Phase 8 adds COALESCING (my_block_merge) to my_free: a freed
// block is fused with its physically adjacent FREE neighbors so the
// split/free cycles of Phases 6-7 round-trip back to one whole block.
//
// See docs/phase8_coalescing.md for the merge policy and its invariants.
#include <cstddef>
#include <cstdio>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>

namespace {

void print_block(const char* label, my_block_header* block) {
    if (block == nullptr) {
        std::printf("%-12s (null)\n", label);
        return;
    }
    std::printf("%-12s block=%p size=%-5zu free=%d valid=%s\n", label, static_cast<void*>(block),
                block->size, my_block_is_free(block), my_block_valid(block) == 1 ? "yes" : "NO");
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 8: coalescing adjacent free blocks ===\n");
    std::printf("default alignment: %zu   header offset: %zu bytes\n\n", my_default_alignment(),
                my_block_user_offset());

    // --- one oversized free block, then a split ---------------------------------
    // A whole-page region (4096) freed whole; a small request splits it into an
    // allocated front + a listed remainder (Phase 7).
    std::printf("--- split an oversized free block (Phase 7) ---\n");
    void* big = my_malloc(4096U - my_block_user_offset());
    my_free(big);
    std::printf("freed whole block; list has %zu block(s)\n\n", my_free_list_count());

    void* front = my_malloc(16U);
    my_block_header* fb = my_user_to_block(front);
    my_block_header* remainder = fb != nullptr ? fb->next : nullptr;
    print_block("front(16)", fb);
    print_block("remainder", remainder);
    std::printf("two adjacent blocks now share the region\n\n");

    // --- freeing the front COALESCES it back with the remainder ----------------
    // Without coalescing this would leave TWO listed free blocks; with it, the
    // front and the free remainder fuse into one whole-region block again.
    std::printf("--- free the front: it coalesces with the remainder ---\n");
    my_free(front);
    my_block_header* head = my_free_list_head();
    print_block("coalesced", head);
    std::printf("list has %zu block(s) (coalesced, not 2), size=%zu (whole region)\n\n",
                my_free_list_count(), head != nullptr ? head->size : 0U);

    // --- fragmentation is bounded across many split/free cycles ----------------
    // Each cycle splits the region and frees the front again. With coalescing
    // the block count and the survivor size stay constant: no fragmentation
    // accrues, and the resident set does not grow.
    std::printf("--- 1000 split/free cycles keep the region whole ---\n");
    // Reuse the single coalesced block already in the list: each cycle splits
    // off a small front (Phase 7) and frees it again, so coalescing (Phase 8)
    // must fuse it straight back — count and size never drift.
    const std::size_t start_size = my_free_list_head() != nullptr ? my_free_list_head()->size : 0U;
    bool stable = true;
    for (int i = 0; i < 1000; ++i) {
        void* x = my_malloc(16U);
        if (x == nullptr || my_free_list_count() != 1U || my_free_list_valid() != 1) {
            stable = false;
            break;
        }
        my_free(x);
        my_block_header* survivor = my_free_list_head();
        if (my_free_list_count() != 1U || survivor == nullptr || survivor->size != start_size) {
            stable = false;
            break;
        }
    }
    std::printf("after 1000 cycles: count=%zu size=%zu (unchanged=%s)\n\n", my_free_list_count(),
                my_free_list_head() != nullptr ? my_free_list_head()->size : 0U,
                stable ? "yes" : "NO");

    std::printf("free(NULL) is a no-op: ");
    my_free(nullptr);
    std::printf("ok\n");
    return 0;
}
