// mymalloc Phase 13 demo: fragmentation and heap statistics.
//
// Walks one narrative through <mymalloc/stats.h>: what internal vs external
// fragmentation look like in real numbers, how the fresh-region model spikes
// internal slack, how the split/reuse and coalescing paths keep it in check,
// how realloc tracks the live request, and how scavenge-on-acquire shrinks
// the mapped footprint. See docs/architecture.md.
#include <cstdio>

#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/stats.h>

namespace {

// One-line snapshot of the fields that matter for the current step.
void print_stats(const char* label) {
    my_heap_stats s = {};
    if (my_heap_stats_collect(&s) != 1) {
        std::printf("  %-46s collect refused (inconsistent heap)\n", label);
        return;
    }
    std::printf("  %-46s regions=%zu mapped=%zu\n", label, s.region_count, s.mapped_bytes);
    std::printf("  %-46s live=%zu/%zu free=%zu/%zu largest_free=%zu\n", "", s.live_blocks,
                s.live_bytes, s.free_blocks, s.free_bytes, s.largest_free_bytes);
    std::printf("  %-46s requested=%zu payload=%zu slack=%zu overhead=%zu\n", "", s.requested_bytes,
                s.payload_bytes, s.internal_slack_bytes, s.overhead_bytes);
    std::printf("  %-46s external=%u/1000 internal=%u/1000\n", "", s.external_frag_permille,
                s.internal_frag_permille);
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 13: fragmentation & heap statistics ===\n\n");

    // 1. The Phase 5 model hands a fresh region over whole: one my_malloc(1)
    //    pins ~a full page of internal slack (payload - request).
    std::printf("[1] my_malloc(1) on an empty heap: the fresh region goes to one block\n");
    void* tiny = my_malloc(1);
    print_stats("internal fragmentation near maximum");

    // 2. free() never shrinks the heap: the page becomes one big free block.
    std::printf("\n[2] my_free: mapped bytes stay (free never unmaps)\n");
    my_free(tiny);
    print_stats("one big free block, nothing fragmented");

    // 3. Reuse splits the free block: the 480-byte piece carries ZERO
    //    internal slack — alignment padding only exists when the request
    //    is not alignment-exact.
    std::printf("\n[3] reuse: my_malloc(480) splits the free page\n");
    void* piece = my_malloc(480);
    print_stats("requested == payload -> internal 0/1000");
    my_free(piece);

    // 4. Eight 512-byte blocks tile the page; freeing every other one
    //    scatters the free bytes: 4 blocks of 512, largest still 512.
    std::printf("\n[4] checkerboard: 8 blocks tile the page, free every other one\n");
    void* owner = my_malloc(my_raw_page_size() - my_block_user_offset());
    my_free(owner);
    void* p[8] = {};
    for (int i = 0; i < 8; ++i) {
        p[i] = my_malloc(480);
    }
    for (int i = 1; i < 8; i += 2) {
        my_free(p[i]);
    }
    print_stats("free space is uniform but stranded: external ~750");

    // 5. Freeing the neighbors coalesces the page back into one block and
    //    the external ratio returns to zero.
    std::printf("\n[5] coalescing recovery: free the remaining blocks\n");
    for (int i = 0; i < 8; i += 2) {
        my_free(p[i]);
    }
    print_stats("fully merged: external 0/1000 again");

    // 6. realloc keeps the recorded request honest through shrink and grow.
    std::printf("\n[6] realloc: requested follows the live request\n");
    void* r = my_malloc(64);
    r = my_realloc(r, 3); // shrink: the tail returns to the free list
    print_stats("after my_realloc(r, 3): slack 0");
    r = my_realloc(r, 1000); // grow: 1000 rounds to a 1024 payload
    print_stats("after my_realloc(r, 1000): slack 24");
    my_free(r);

    // 7. Scavenge-on-acquire: an oversized request unmapped every
    //    fully-free region first, so mapped == live after the acquire.
    std::printf("\n[7] scavenge: my_malloc(256 KiB) collapses free regions\n");
    void* big = my_malloc(256U * 1024U);
    print_stats("fresh large region: only live memory is mapped");
    my_free(big);
    print_stats("freed but still mapped until the next big acquire");

    std::printf("\n=== done: every number above is derived by walking the heap ===\n");
    return 0;
}
