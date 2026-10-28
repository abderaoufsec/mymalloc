// Heap statistics and fragmentation (<mymalloc/stats.h>): requested/allocated/
// free bytes, block counts, internal vs external fragmentation, and the
// collect contract (1 + filled on a consistent heap, 0 + untouched on
// refusal or inconsistency).
#include <cstddef>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>
#include <mymalloc/stats.h>

#include "test_framework.hpp"

namespace {

// Everything below is computed from the platform's real constants (header
// offset, alignment, page size) so the suite holds on any LP64 target.
std::size_t offset() {
    return my_block_user_offset();
}

std::size_t align_up(std::size_t bytes) {
    std::size_t aligned = 0;
    MYMALLOC_CHECK(my_align_up_size(bytes, my_default_alignment(), &aligned) == 1);
    return aligned;
}

// The allocator maps at least 16 pages per fresh arena, so every scenario
// starts from this region size instead of one page.
std::size_t arena_bytes() {
    return my_raw_page_size() * 16U;
}

std::size_t align_to_page(std::size_t bytes) {
    const std::size_t page = my_raw_page_size();
    return ((bytes + page - 1U) / page) * page;
}

// Truncated permille, exactly like ratio_permille in src/stats.cpp.
unsigned int permille(std::size_t part, std::size_t whole) {
    return whole == 0 ? 0U : static_cast<unsigned int>((part * 1000U) / whole);
}

// Structural invariants that must hold for ANY consistent snapshot: the
// accounting adds up and both ratios are in range.
void check_invariants(const my_heap_stats& s) {
    MYMALLOC_CHECK(s.block_count == s.live_blocks + s.free_blocks);
    MYMALLOC_CHECK(s.live_bytes + s.free_bytes == s.mapped_bytes); // regions tile
    MYMALLOC_CHECK(s.requested_bytes <= s.payload_bytes);
    MYMALLOC_CHECK(s.payload_bytes == s.live_bytes - s.live_blocks * offset());
    MYMALLOC_CHECK(s.internal_slack_bytes == s.payload_bytes - s.requested_bytes);
    MYMALLOC_CHECK(s.overhead_bytes == s.block_count * offset());
    MYMALLOC_CHECK(s.largest_free_bytes <= s.free_bytes);
    MYMALLOC_CHECK(s.external_frag_permille ==
                   permille(s.free_bytes - s.largest_free_bytes, s.free_bytes));
    MYMALLOC_CHECK(s.internal_frag_permille == permille(s.internal_slack_bytes, s.payload_bytes));
    MYMALLOC_CHECK(s.external_frag_permille <= 1000U);
    MYMALLOC_CHECK(s.internal_frag_permille <= 1000U);
}

my_heap_stats collect() {
    my_heap_stats s = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&s) == 1);
    check_invariants(s);
    return s;
}

// --- scenario 1: empty heap, refusals, untracked memory is invisible ------
void test_empty_and_refusals() {
    my_heap_stats s = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&s) == 1);
    MYMALLOC_CHECK(s.region_count == 0 && s.mapped_bytes == 0 && s.block_count == 0);
    MYMALLOC_CHECK(s.live_blocks == 0 && s.free_blocks == 0 && s.requested_bytes == 0);
    MYMALLOC_CHECK(s.external_frag_permille == 0 && s.internal_frag_permille == 0);

    // Refusal contract: NULL is rejected and writes nothing.
    MYMALLOC_CHECK(my_heap_stats_collect(nullptr) == 0);

    // A foreign region acquired directly through the raw layer is NOT
    // tracked, so it must not appear in any statistic.
    void* foreign = my_raw_acquire(my_raw_page_size());
    MYMALLOC_CHECK(foreign != nullptr);
    my_heap_stats after = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&after) == 1);
    MYMALLOC_CHECK(after.region_count == 0 && after.mapped_bytes == 0);
    my_raw_release(foreign, my_raw_page_size());
}

// --- scenario 2: one tiny allocation on a fresh heap ----------------------
// The fresh path carves the request from the front of a 16-page arena and
// leaves the remainder as one free block, so a my_malloc(1) has a small live
// block and a large free remainder in the same region.
void test_single_allocation_page_math() {
    void* p = my_malloc(1);
    MYMALLOC_CHECK(p != nullptr);

    const std::size_t region_bytes = arena_bytes();
    my_heap_stats s = collect();
    MYMALLOC_CHECK(s.region_count == 1);
    MYMALLOC_CHECK(s.mapped_bytes == region_bytes);
    MYMALLOC_CHECK(s.block_count == 2 && s.live_blocks == 1 && s.free_blocks == 1);
    MYMALLOC_CHECK(s.live_bytes == align_up(1) + offset());
    MYMALLOC_CHECK(s.free_bytes == region_bytes - s.live_bytes);
    MYMALLOC_CHECK(s.requested_bytes == 1);
    MYMALLOC_CHECK(s.payload_bytes == align_up(1));
    MYMALLOC_CHECK(s.internal_slack_bytes == align_up(1) - 1U);
    MYMALLOC_CHECK(s.overhead_bytes == 2U * offset());
    MYMALLOC_CHECK(s.external_frag_permille == 0);
    MYMALLOC_CHECK(s.internal_frag_permille == permille(align_up(1) - 1U, align_up(1)));

    // my_region_at: bounds and refusal (iteration helper for the walk).
    my_region r = {};
    MYMALLOC_CHECK(my_region_at(0, &r) == 1 && r.bytes == region_bytes);
    MYMALLOC_CHECK(my_region_at(1, &r) == 0);
    MYMALLOC_CHECK(my_region_at(0, nullptr) == 0);

    // free() never shrinks the heap: the region stays mapped and becomes one
    // big free block.
    my_free(p);
    my_heap_stats f = collect();
    MYMALLOC_CHECK(f.region_count == 1 && f.mapped_bytes == region_bytes);
    MYMALLOC_CHECK(f.block_count == 1 && f.free_blocks == 1);
    MYMALLOC_CHECK(f.free_bytes == region_bytes);
    MYMALLOC_CHECK(f.largest_free_bytes == region_bytes);
    MYMALLOC_CHECK(f.live_bytes == 0 && f.requested_bytes == 0 && f.internal_slack_bytes == 0);
    MYMALLOC_CHECK(f.external_frag_permille == 0 && f.internal_frag_permille == 0);
}

// --- scenario 3: alignment padding sweep ----------------------------------
// For a run of request sizes: requested is exact, slack == aligned payload
// - request, live/free bytes tile the region, and free() merges everything
// back into one block.
void test_padding_sweep() {
    const std::size_t region_bytes = arena_bytes();
    MYMALLOC_CHECK(collect().free_bytes == region_bytes); // scenario 2's region
    const std::size_t sizes[] = {1U, 15U, 31U, 32U, 33U, 63U, 64U, 100U, 1000U};
    for (std::size_t size : sizes) {
        void* p = my_malloc(size);
        MYMALLOC_CHECK(p != nullptr);

        my_heap_stats s = collect();
        const std::size_t payload = align_up(size);
        MYMALLOC_CHECK(s.live_blocks == 1);
        MYMALLOC_CHECK(s.live_bytes == payload + offset());
        MYMALLOC_CHECK(s.requested_bytes == size);
        MYMALLOC_CHECK(s.payload_bytes == payload);
        MYMALLOC_CHECK(s.internal_slack_bytes == payload - size);
        MYMALLOC_CHECK(s.free_blocks == 1);
        MYMALLOC_CHECK(s.free_bytes == region_bytes - s.live_bytes);
        MYMALLOC_CHECK(s.external_frag_permille == 0); // one free block = no external
        MYMALLOC_CHECK(s.internal_frag_permille == permille(payload - size, payload));

        my_free(p);
        my_heap_stats f = collect();
        MYMALLOC_CHECK(f.block_count == 1 && f.free_blocks == 1);
        MYMALLOC_CHECK(f.free_bytes == region_bytes); // fully coalesced again
        MYMALLOC_CHECK(f.requested_bytes == 0 && f.internal_slack_bytes == 0);
    }
}

// --- scenario 4: external fragmentation workload --------------------------
// Eight 512-byte blocks plus a filler tile one 16-page arena exactly.
// Freeing every other small block turns uniform free space into the classic
// checkerboard, driving the external ratio through 750/500/333/142 back to 0
// as coalescing recovers; the filler keeps the rest of the arena live so the
// numbers stay exact.
void test_fragmentation_workload() {
    const std::size_t region_bytes = arena_bytes();
    const std::size_t block = 512U;               // 480 payload + 32 header
    const std::size_t request = block - offset(); // tiles the arena exactly
    MYMALLOC_CHECK(region_bytes % block == 0U);
    MYMALLOC_CHECK(block % my_default_alignment() == 0U);

    void* p[8] = {};
    for (int i = 0; i < 8; ++i) {
        p[i] = my_malloc(request);
        MYMALLOC_CHECK(p[i] != nullptr);
    }
    // Consume the remaining arena as one live filler block.
    const std::size_t filler_total = region_bytes - 8U * block;
    MYMALLOC_CHECK(filler_total > offset());
    const std::size_t filler_request = filler_total - offset();
    void* filler = my_malloc(filler_request);
    MYMALLOC_CHECK(filler != nullptr);

    my_heap_stats full = collect();
    MYMALLOC_CHECK(full.block_count == 9 && full.live_blocks == 9);
    MYMALLOC_CHECK(full.live_bytes == region_bytes && full.free_bytes == 0);
    MYMALLOC_CHECK(full.requested_bytes == 8U * request + filler_request);
    MYMALLOC_CHECK(full.external_frag_permille == 0 && full.internal_slack_bytes == 0);

    // Checkerboard: free the odds -> free space is uniform but fragmented.
    for (int i = 1; i < 8; i += 2) {
        my_free(p[i]);
    }
    my_heap_stats checker = collect();
    MYMALLOC_CHECK(checker.free_blocks == 4 && checker.free_bytes == 4U * block);
    MYMALLOC_CHECK(checker.largest_free_bytes == block);
    MYMALLOC_CHECK(checker.live_blocks == 5 && checker.live_bytes == region_bytes - 4U * block);
    MYMALLOC_CHECK(checker.external_frag_permille == 750U);
    MYMALLOC_CHECK(checker.internal_slack_bytes == 0);

    // Free the evens one by one: each merge grows the largest free block.
    my_free(p[0]); // merges with p[1] -> 1024 contiguous + 3 stranded odds
    my_heap_stats s0 = collect();
    MYMALLOC_CHECK(s0.largest_free_bytes == 2U * block);
    MYMALLOC_CHECK(s0.free_bytes == 5U * block);
    MYMALLOC_CHECK(s0.external_frag_permille == 600U);

    my_free(p[2]); // merges down and up -> 2048 contiguous
    my_heap_stats s2 = collect();
    MYMALLOC_CHECK(s2.largest_free_bytes == 4U * block);
    MYMALLOC_CHECK(s2.free_bytes == 6U * block && s2.free_blocks == 3);
    MYMALLOC_CHECK(s2.external_frag_permille == 333U);

    my_free(p[4]); // -> 3072 contiguous, p[7] still separate
    my_heap_stats s4 = collect();
    MYMALLOC_CHECK(s4.largest_free_bytes == 6U * block);
    MYMALLOC_CHECK(s4.free_bytes == 7U * block && s4.free_blocks == 2);
    MYMALLOC_CHECK(s4.external_frag_permille == 142U);

    my_free(p[6]); // last live small block: full merge with p[7]
    my_heap_stats done = collect();
    MYMALLOC_CHECK(done.block_count == 2 && done.free_blocks == 1);
    MYMALLOC_CHECK(done.free_bytes == 8U * block);
    MYMALLOC_CHECK(done.live_blocks == 1 && done.requested_bytes == filler_request);
    MYMALLOC_CHECK(done.external_frag_permille == 0); // fully recovered

    my_free(filler); // the whole arena is free again for the next scenario
    MYMALLOC_CHECK(collect().free_bytes == region_bytes);
}

// --- scenario 5: realloc keeps the recorded request honest ----------------
// In-place shrink/grow must update requested_bytes; a move must retire the
// old block's request and start the new one. A request larger than the arena
// forces the move path.
void test_realloc_tracks_request() {
    const std::size_t region_bytes = arena_bytes();

    void* r = my_malloc(64);
    MYMALLOC_CHECK(r != nullptr);
    my_heap_stats s0 = collect();
    MYMALLOC_CHECK(s0.requested_bytes == 64 && s0.internal_slack_bytes == 0);
    MYMALLOC_CHECK(s0.live_bytes == 64U + offset() && s0.free_bytes == region_bytes - 96U);

    // Grow in place by absorbing the free upper neighbor; the request moves
    // to 1000, the payload rounds up -> alignment slack appears.
    r = my_realloc(r, 1000);
    MYMALLOC_CHECK(r != nullptr);
    const std::size_t payload_a = align_up(1000);
    my_heap_stats s1 = collect();
    MYMALLOC_CHECK(s1.requested_bytes == 1000 && s1.payload_bytes == payload_a);
    MYMALLOC_CHECK(s1.internal_slack_bytes == payload_a - 1000U);
    MYMALLOC_CHECK(s1.live_bytes == payload_a + offset());
    MYMALLOC_CHECK(s1.free_bytes == region_bytes - s1.live_bytes);

    // Shrink in place: slack is carved back into the free list.
    r = my_realloc(r, 32);
    MYMALLOC_CHECK(r != nullptr);
    my_heap_stats s2 = collect();
    MYMALLOC_CHECK(s2.requested_bytes == 32 && s2.internal_slack_bytes == 0);
    MYMALLOC_CHECK(s2.live_bytes == 64U && s2.free_bytes == region_bytes - 64U);

    // Grow in place again: 2000 rounds up to a fresh payload.
    r = my_realloc(r, 2000);
    MYMALLOC_CHECK(r != nullptr);
    const std::size_t payload_b = align_up(2000);
    my_heap_stats s3 = collect();
    MYMALLOC_CHECK(s3.requested_bytes == 2000 && s3.payload_bytes == payload_b);
    MYMALLOC_CHECK(s3.internal_slack_bytes == payload_b - 2000U);
    MYMALLOC_CHECK(s3.internal_frag_permille == permille(payload_b - 2000U, payload_b));
    MYMALLOC_CHECK(s3.live_bytes == payload_b + offset());
    MYMALLOC_CHECK(s3.free_bytes == region_bytes - s3.live_bytes);

    // Too big for the arena: the move path frees the old block (retiring its
    // request) and a fresh region serves the new one. The old 16-page region
    // stays mapped but fully free.
    const std::size_t moved_request = 70000U;
    r = my_realloc(r, moved_request);
    MYMALLOC_CHECK(r != nullptr);
    const std::size_t moved_total = align_up(moved_request) + offset();
    const std::size_t moved_region =
        align_to_page(moved_total > region_bytes ? moved_total : region_bytes);
    my_heap_stats s4 = collect();
    MYMALLOC_CHECK(s4.region_count == 2 && s4.mapped_bytes == region_bytes + moved_region);
    MYMALLOC_CHECK(s4.block_count == 3 && s4.live_blocks == 1 && s4.free_blocks == 2);
    MYMALLOC_CHECK(s4.requested_bytes == moved_request); // only the surviving request
    MYMALLOC_CHECK(s4.live_bytes == moved_total);
    MYMALLOC_CHECK(s4.free_bytes == region_bytes + (moved_region - moved_total));
    MYMALLOC_CHECK(s4.largest_free_bytes == region_bytes);

    // Free it: both regions are now fully free but isolated from each
    // other, so the free bytes read as fragmented across region borders.
    my_free(r);
    my_heap_stats s5 = collect();
    MYMALLOC_CHECK(s5.free_blocks == 2 && s5.free_bytes == region_bytes + moved_region);
    MYMALLOC_CHECK(s5.largest_free_bytes == moved_region);
    MYMALLOC_CHECK(s5.external_frag_permille ==
                   permille(region_bytes, region_bytes + moved_region));
    MYMALLOC_CHECK(s5.requested_bytes == 0);
}

// --- scenario 6: scavenge-on-acquire collapses fully-free regions ---------
void test_scavenge_updates_stats() {
    my_heap_stats before = collect();
    MYMALLOC_CHECK(before.region_count == 2); // from scenario 5

    const std::size_t request = 100000U; // larger than either free region
    void* big = my_malloc(request);
    MYMALLOC_CHECK(big != nullptr);

    const std::size_t total = align_up(request) + offset();
    const std::size_t region_bytes = align_to_page(total);
    my_heap_stats s = collect();
    MYMALLOC_CHECK(s.region_count == 1); // both old regions released
    MYMALLOC_CHECK(s.mapped_bytes == region_bytes);
    MYMALLOC_CHECK(s.block_count == 2 && s.live_blocks == 1 && s.free_blocks == 1);
    MYMALLOC_CHECK(s.live_bytes == total);
    MYMALLOC_CHECK(s.requested_bytes == request);
    MYMALLOC_CHECK(s.payload_bytes == align_up(request));
    MYMALLOC_CHECK(s.internal_slack_bytes == 0);
    MYMALLOC_CHECK(s.external_frag_permille == 0);

    my_free(big); // policy: free never shrinks; the region stays mapped
    my_heap_stats f = collect();
    MYMALLOC_CHECK(f.region_count == 1 && f.free_bytes == f.mapped_bytes);
    MYMALLOC_CHECK(f.requested_bytes == 0 && f.internal_slack_bytes == 0);
}

// --- scenario 7: calloc's request is the array size -----------------------
void test_calloc_accounting() {
    void* c = my_calloc(10, 20);
    MYMALLOC_CHECK(c != nullptr);
    MYMALLOC_CHECK(*static_cast<unsigned char*>(c) == 0); // still zeroed

    my_heap_stats s = collect();
    const std::size_t payload = align_up(200);
    MYMALLOC_CHECK(s.requested_bytes == 200U); // count * size, exactly
    MYMALLOC_CHECK(s.payload_bytes == payload);
    MYMALLOC_CHECK(s.internal_slack_bytes == payload - 200U);
    MYMALLOC_CHECK(s.internal_frag_permille == permille(payload - 200U, payload));

    my_free(c);
    MYMALLOC_CHECK(collect().requested_bytes == 0);
}

// --- scenario 8: long mixed cycles hold every invariant -------------------
// Rotating requests keep ALL seven blocks live mid-cycle (so the summed
// request must equal the summed sizes exactly), while split/absorb/move
// paths churn underneath. Invariants are checked on every cycle.
void test_long_cycles_bounded() {
    const std::size_t sizes[] = {7U, 33U, 100U, 400U, 1000U, 3000U, 9000U};
    const std::size_t expected_live_request = 7U + 33U + 100U + 400U + 1000U + 3000U + 9000U;
    void* held[7] = {};

    for (std::size_t cycle = 0; cycle < 250U; ++cycle) {
        for (std::size_t i = 0; i < 7U; ++i) {
            held[i] = my_malloc(sizes[(cycle + i) % 7U]);
            MYMALLOC_CHECK(held[i] != nullptr);
        }
        my_heap_stats live = collect();
        MYMALLOC_CHECK(live.requested_bytes == expected_live_request);
        MYMALLOC_CHECK(live.live_blocks == 7U);

        for (std::size_t i = 0; i < 7U; ++i) {
            my_free(held[(cycle + i) % 7U]);
        }
        my_heap_stats freed = collect();
        MYMALLOC_CHECK(freed.requested_bytes == 0);
        MYMALLOC_CHECK(freed.live_blocks == 0);
        MYMALLOC_CHECK(freed.free_bytes == freed.mapped_bytes); // all free per region
    }

    // Endgame: one oversized request scavenges every free region away.
    const std::size_t request = 1U * 1024U * 1024U;
    void* final = my_malloc(request);
    MYMALLOC_CHECK(final != nullptr);
    const std::size_t total = align_up(request) + offset();
    const std::size_t region_bytes = align_to_page(total);
    my_heap_stats s = collect();
    MYMALLOC_CHECK(s.region_count == 1);
    MYMALLOC_CHECK(s.mapped_bytes == region_bytes && s.live_bytes == total);
    MYMALLOC_CHECK(s.free_bytes == region_bytes - total);
    my_free(final);
    MYMALLOC_CHECK(collect().region_count == 1); // free keeps it mapped
}

} // namespace

int main() {
    test_empty_and_refusals();
    test_single_allocation_page_math();
    test_padding_sweep();
    test_fragmentation_workload();
    test_realloc_tracks_request();
    test_scavenge_updates_stats();
    test_calloc_accounting();
    test_long_cycles_bounded();
    MYMALLOC_TEST_MAIN("fragmentation");
}
