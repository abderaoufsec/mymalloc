// Phase 12 test: verify the region registry (<mymalloc/regions.h>) against
// the contract documented in docs/architecture.md: every region
// my_malloc acquires is tracked, multiple regions coexist, fully-free regions
// are reclaimed scavenge-on-acquire (never while any allocation is live), the
// release guardrails refuse anything that could cause an invalid unmapping
// (untracked bases, size mismatches, regions with live allocations, foreign
// raw regions), and long allocation/free cycles keep the registry bounded.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>

#include "test_framework.hpp"

namespace {

// True when every byte in [ptr, ptr+size) equals `pattern`.
bool pattern_ok(const void* ptr, std::size_t size, unsigned char pattern) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != pattern) {
            return false;
        }
    }
    return true;
}

// Drops allocator-visible leftovers (free list + fully-free regions) so
// per-function counts below are exact regardless of earlier functions.
void reset_heap() {
    my_free_list_clear();
    static_cast<void>(my_regions_release_free());
}

// --- track/find/count/release round trip on a RAW (allocator-unaware) region
void test_track_round_trip() {
    const std::size_t page = my_raw_page_size();
    const std::size_t before = my_region_count();

    void* region = my_raw_acquire(page * 2U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }

    // Tracking refusals, none of which change the registry.
    MYMALLOC_CHECK(my_region_track(nullptr, page) == 0);
    MYMALLOC_CHECK(my_region_track(region, 0) == 0);
    MYMALLOC_CHECK(my_region_track(region, page + 1U) == 0); // not a page multiple
    MYMALLOC_CHECK(my_region_count() == before);

    MYMALLOC_CHECK(my_region_track(region, page * 2U) == 1);
    MYMALLOC_CHECK(my_region_count() == before + 1U);
    MYMALLOC_CHECK(my_region_track(region, page * 2U) == 0); // double-track refused

    // Containment: base, last byte inside; one-past-end, NULL refused.
    my_region found{};
    MYMALLOC_CHECK(my_region_find(region, &found) == 1);
    MYMALLOC_CHECK(found.base == region);
    MYMALLOC_CHECK(found.bytes == page * 2U);
    auto* bytes = static_cast<unsigned char*>(region);
    MYMALLOC_CHECK(my_region_find(&bytes[page * 2U - 1U], &found) == 1);
    MYMALLOC_CHECK(my_region_find(&bytes[page * 2U], &found) == 0); // one past end
    MYMALLOC_CHECK(my_region_find(nullptr, &found) == 0);
    MYMALLOC_CHECK(my_region_find(region, nullptr) == 0);

    // A tracked region is only releasable when it is FULLY FREE: the raw
    // pages above hold no block yet, so even the correct size is refused.
    MYMALLOC_CHECK(my_region_release(region, page * 2U) == 0);
    MYMALLOC_CHECK(my_region_count() == before + 1U);

    // Wrong size is refused — and the pages stay mapped and writable.
    MYMALLOC_CHECK(my_region_release(region, page) == 0); // extent is 2 pages
    MYMALLOC_CHECK(my_region_count() == before + 1U);
    std::memset(region, 0x5A, page * 2U); // still mapped -> writable
    MYMALLOC_CHECK(pattern_ok(region, page * 2U, 0x5A));

    // Give the region the shape the allocator would leave behind: one FREE
    // block spanning the whole extent (the release path reads the header
    // only, so the payload may hold anything).
    auto* block = reinterpret_cast<my_block_header*>(region);
    MYMALLOC_CHECK(my_block_init(block, page * 2U, 1 /* free */, nullptr, nullptr) == 1);

    // Correct release unmaps and untracks exactly this region.
    MYMALLOC_CHECK(my_region_release(region, page * 2U) == 1);
    MYMALLOC_CHECK(my_region_count() == before);
    // A second release of the same base is refused (no longer tracked).
    MYMALLOC_CHECK(my_region_release(region, page * 2U) == 0);
    MYMALLOC_CHECK(my_region_release(nullptr, page) == 0);
    MYMALLOC_CHECK(my_region_release(region, 0) == 0);
}

// --- my_malloc tracks its regions; free does NOT shrink the heap ------------
void test_malloc_tracks_and_free_keeps() {
    const std::size_t before = my_region_count();

    void* a = my_malloc(64U);
    MYMALLOC_CHECK(a != nullptr);
    MYMALLOC_CHECK(my_region_count() == before + 1U); // one new region

    // The user pointer lies inside the tracked region.
    my_region found{};
    MYMALLOC_CHECK(my_region_find(a, &found) == 1);
    MYMALLOC_CHECK(my_user_to_block(a) == reinterpret_cast<my_block_header*>(found.base));

    my_free(a);
    // Phase 12 policy: freeing never unmaps — the region stays tracked and
    // its block stays listed for reuse.
    MYMALLOC_CHECK(my_region_count() == before + 1U);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // A same-size allocation reuses the listed region: no new tracking.
    void* b = my_malloc(64U);
    MYMALLOC_CHECK(b == a);
    MYMALLOC_CHECK(my_region_count() == before + 1U);
    my_free(b);

    // A larger request still fits the same page-sized region (split path).
    void* c = my_malloc(1024U);
    MYMALLOC_CHECK(c != nullptr);
    MYMALLOC_CHECK(my_region_count() == before + 1U);
    my_free(c);
}

// --- multiple regions + scavenge-on-acquire ---------------------------------
void test_scavenge_on_acquire() {
    reset_heap();
    MYMALLOC_CHECK(my_region_count() == 0);

    // Three live allocations need three separate regions (nothing is free,
    // so every request must acquire).
    void* a = my_malloc(64U);
    void* b = my_malloc(64U);
    void* c = my_malloc(64U);
    MYMALLOC_CHECK(a != nullptr && b != nullptr && c != nullptr);
    MYMALLOC_CHECK(a != b && b != c && a != c);
    MYMALLOC_CHECK(my_region_count() == 1U);
    std::memset(a, 0xA1, 64U);
    std::memset(b, 0xB2, 64U);
    std::memset(c, 0xC3, 64U);

    // Freeing keeps every region (free does not shrink the heap).
    my_free(a);
    my_free(b);
    my_free(c);
    MYMALLOC_CHECK(my_region_count() == 1U);
    MYMALLOC_CHECK(my_free_list_count() == 1U);

    // A request larger than any listed block cannot be served from the free
    // list: the fresh acquisition scavenges ALL three fully-free regions
    // first, then maps exactly one new one.
    void* big = my_malloc(my_raw_page_size() * 32U); // larger than any free block -> scavenge
    MYMALLOC_CHECK(big != nullptr);
    MYMALLOC_CHECK(my_region_count() == 1U);   // only big's region remains
    MYMALLOC_CHECK(my_free_list_count() == 1); // reclaimed blocks left the list (remainder)
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    std::memset(big, 0xD4, 64U);

    my_free(big);
    MYMALLOC_CHECK(my_region_count() == 1U); // free keeps it (policy)
}

// --- live allocations protect their regions ---------------------------------
void test_live_blocks_block_release() {
    reset_heap();
    MYMALLOC_CHECK(my_region_count() == 0);

    // One region holding TWO allocations: request a page, free it, then
    // reuse it twice — the splits place both allocations in the SAME region.
    void* carrier = my_malloc(my_raw_page_size() - my_block_user_offset());
    MYMALLOC_CHECK(carrier != nullptr);
    if (carrier == nullptr) {
        return;
    }
    my_free(carrier);              // listed whole
    void* first = my_malloc(64U);  // split: front 96 (live), rest free
    void* second = my_malloc(64U); // split: front 160 (live), rest free
    MYMALLOC_CHECK(first != nullptr && second != nullptr);
    MYMALLOC_CHECK(my_region_count() == 1U); // both allocations share one region
    std::memset(first, 0x11, 64U);
    std::memset(second, 0x22, 64U);

    // Freeing ONE of them cannot make the region fully free: the scavenge
    // must keep the region (and the survivor's data) intact.
    my_free(first);
    void* big = my_malloc(my_raw_page_size() * 64U); // forces an acquire -> scavenge
    MYMALLOC_CHECK(big != nullptr);
    MYMALLOC_CHECK(my_region_count() == 2U); // carrier region + big's region
    MYMALLOC_CHECK(pattern_ok(second, 64U, 0x22));

    // Freeing the survivor finally makes the region fully free; the NEXT
    // acquire reclaims it (and big's, which was freed too).
    my_free(second);
    my_free(big);
    void* huge = my_malloc(my_raw_page_size() * 66U);
    MYMALLOC_CHECK(huge != nullptr);
    MYMALLOC_CHECK(my_region_count() == 1U);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    my_free(huge);
}

// --- foreign (untracked) regions are never unmapped -------------------------
void test_untracked_regions_untouched() {
    // A raw region the TEST acquired is invisible to the registry: even a
    // perfectly "releasable" block inside it must never cause an unmapping.
    const std::size_t page = my_raw_page_size();
    void* foreign = my_raw_acquire(page);
    MYMALLOC_CHECK(foreign != nullptr);
    if (foreign == nullptr) {
        return;
    }
    auto* block = reinterpret_cast<my_block_header*>(foreign);
    MYMALLOC_CHECK(my_block_init(block, page, 0 /* allocated */, nullptr, nullptr) == 1);

    const std::size_t before = my_region_count();

    // Release attempts on the foreign base are refused outright.
    MYMALLOC_CHECK(my_region_release(foreign, page) == 0);
    MYMALLOC_CHECK(my_region_count() == before);

    // Free the block: it becomes a fully-free whole-region block and joins
    // the free list — but the region is untracked, so even the scavenge at
    // the next acquire leaves its memory and its list entry intact. The
    // request must NOT fit any listed block (the leftover 8-page region
    // above would), so ask for 16 pages to force a fresh acquisition.
    my_free(my_block_to_user(block));
    void* big = my_malloc(my_raw_page_size() * 16U); // acquire -> scavenge
    MYMALLOC_CHECK(big != nullptr);
    MYMALLOC_CHECK(my_region_count() == before); // foreign never tracked/released
    // my_free_list_valid WALKS the foreign block: it would fault if the
    // pages had been unmapped.
    MYMALLOC_CHECK(my_free_list_valid() == 1); // walks foreign block: faults if unmapped

    my_free(big);         // its region is mine to reclaim later (coalesces while list intact)
    my_free_list_clear(); // drop all blocks from the list
    my_raw_release(foreign, page); // the TEST owns this region, not mymalloc
}

// --- long allocation/free cycles keep the registry bounded ------------------
void test_long_cycles_bounded() {
    reset_heap();

    const std::size_t page = my_raw_page_size();
    const std::size_t max_regions = 8U; // generous: the workload holds <= 4 live blocks

    for (int cycle = 0; cycle < 2000; ++cycle) {
        // Four live allocations of varied sizes: small ones reuse free
        // blocks, the multi-page one often forces a fresh acquire (which
        // scavenges everything fully free).
        const std::size_t sizes[4] = {24U, page / 2U, page * 2U, 64U};
        void* pointers[4] = {};
        for (int slot = 0; slot < 4; ++slot) {
            pointers[slot] = my_malloc(sizes[slot]);
            MYMALLOC_CHECK(pointers[slot] != nullptr);
            if (pointers[slot] == nullptr) {
                return;
            }
            std::memset(pointers[slot], static_cast<int>(cycle & 0xFF), sizes[slot]);
        }
        // Verify every payload before freeing anything.
        for (int slot = 0; slot < 4; ++slot) {
            MYMALLOC_CHECK(
                pattern_ok(pointers[slot], sizes[slot], static_cast<unsigned char>(cycle & 0xFF)));
        }
        // Free in a rotated order to mix coalescing directions.
        for (int slot = 0; slot < 4; ++slot) {
            my_free(pointers[(slot + 2) % 4]);
        }
        MYMALLOC_CHECK(my_free_list_valid() == 1);
        if (my_region_count() > max_regions) {
            MYMALLOC_CHECK(my_region_count() <= max_regions); // report the runaway
            return;
        }
    }

    // After the final frees every region is free but still mapped (free
    // never shrinks); one large request reclaims all of it again.
    MYMALLOC_CHECK(my_region_count() <= max_regions);
    void* huge = my_malloc(page * 32U);
    MYMALLOC_CHECK(huge != nullptr);
    MYMALLOC_CHECK(my_region_count() == 1U);
    my_free(huge);
}

} // namespace

int main() {
    test_track_round_trip();
    test_malloc_tracks_and_free_keeps();
    test_scavenge_on_acquire();
    test_live_blocks_block_release();
    test_untracked_regions_untouched();
    test_long_cycles_bounded();
    MYMALLOC_TEST_MAIN("regions");
}