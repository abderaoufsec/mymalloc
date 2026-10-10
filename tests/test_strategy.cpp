// Phase 9 test: allocation strategies (first fit vs best fit) against the
// contract in docs/phase9_allocation_strategies.md:
//   * my_free_list_best_fit returns the SMALLEST free block that fits, ties
//     broken by lowest address (the list is address-ascending),
//   * best fit and first fit agree when only one block fits, and diverge
//     when several do (best fit keeps large blocks intact),
//   * the strategy selector defaults to FIRST_FIT, round-trips, and ignores
//     unknown values (reuse never silently disables),
//   * end-to-end: switching the strategy changes which free block my_malloc
//     hands back, and best fit measurably reduces leftover slack.
#include <cstddef>
#include <cstdint>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/strategy.h>

#include "test_framework.hpp"

namespace {

// Regions acquired during the test; released by teardown(). The paired size
// is the exact value passed to my_raw_acquire, so teardown can release with
// the same size (my_raw_release rounds it up identically) — releasing a fixed
// page*N regardless of the real size would over-unmap and could clobber a
// neighboring region.
void* g_regions[16] = {};
std::size_t g_region_sizes[16] = {};

// A page-rounded region large enough to hold a `size`-byte block.
void* acquire_region(std::size_t index, std::size_t size) {
    void* region = my_raw_acquire(size);
    if (region != nullptr && index < sizeof(g_regions) / sizeof(g_regions[0])) {
        g_regions[index] = region;
        g_region_sizes[index] = size;
    }
    return region;
}

void teardown() {
    my_free_list_clear();
    for (std::size_t i = 0; i < sizeof(g_regions) / sizeof(g_regions[0]); ++i) {
        if (g_regions[i] != nullptr) {
            my_raw_release(g_regions[i], g_region_sizes[i]);
            g_regions[i] = nullptr;
            g_region_sizes[i] = 0;
        }
    }
    my_set_strategy(MY_STRATEGY_FIRST_FIT); // restore the default
}

// Builds `count` independent free blocks of the given sizes (bytes, each a
// multiple of the default alignment) inside freshly acquired regions and
// lists them all. Returns false if any region could not be acquired.
bool make_free_blocks(const std::size_t* sizes, std::size_t count, my_block_header** out_blocks) {
    for (std::size_t i = 0; i < count; ++i) {
        void* region = acquire_region(i, sizes[i]);
        if (region == nullptr) {
            return false;
        }
        auto* block = reinterpret_cast<my_block_header*>(region);
        if (my_block_init(block, sizes[i], 1 /* free */, nullptr, nullptr) != 1) {
            return false;
        }
        if (my_free_list_insert(block) != 1) {
            return false;
        }
        out_blocks[i] = block;
    }
    return true;
}

// ---------------------------------------------------------------------------
// my_free_list_best_fit — the pure-structure primitive
// ---------------------------------------------------------------------------

// Among blocks of sizes {64, 256, 1024}, a request of 200 fits only in 256 or
// 1024; best fit must pick 256 (the smallest adequate).
void test_best_fit_picks_smallest() {
    my_free_list_clear();
    const std::size_t sizes[3] = {1024U, 256U, 64U};
    my_block_header* blocks[3] = {};
    MYMALLOC_CHECK(make_free_blocks(sizes, 3, blocks));
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // Identify blocks by size regardless of the address order mmap returns.
    my_block_header* b256 = nullptr;
    for (std::size_t i = 0; i < 3; ++i) {
        if (blocks[i]->size == 256U) {
            b256 = blocks[i];
        }
    }
    MYMALLOC_CHECK(b256 != nullptr);

    // A 200-byte payload needs 200 + header; only 256 and 1024 clear it.
    const std::size_t need = 200U + my_block_user_offset();
    MYMALLOC_CHECK(my_free_list_first_fit(need) != nullptr);
    MYMALLOC_CHECK(my_free_list_best_fit(need) == b256); // smallest adequate

    my_free_list_clear();
}

// A request that fits only in the single largest block: best fit and first
// fit must agree — the strategies only diverge when more than one block fits.
void test_best_fit_agrees_with_first_when_unique() {
    my_free_list_clear();
    const std::size_t sizes[2] = {512U, 64U};
    my_block_header* blocks[2] = {};
    MYMALLOC_CHECK(make_free_blocks(sizes, 2, blocks));

    const std::size_t need = 300U + my_block_user_offset(); // only 512 fits
    my_block_header* const ff = my_free_list_first_fit(need);
    my_block_header* const bf = my_free_list_best_fit(need);
    MYMALLOC_CHECK(ff == bf);
    MYMALLOC_CHECK(bf != nullptr);
    MYMALLOC_CHECK(bf->size == 512U);

    my_free_list_clear();
}

// Defensive: a zero request and a request larger than every block both yield
// NULL, and neither call mutates the list.
void test_best_fit_none_fits() {
    my_free_list_clear();
    const std::size_t sizes[2] = {128U, 256U};
    my_block_header* blocks[2] = {};
    MYMALLOC_CHECK(make_free_blocks(sizes, 2, blocks));
    const std::size_t count_before = my_free_list_count();

    MYMALLOC_CHECK(my_free_list_best_fit(0) == NULL);       // defensive
    MYMALLOC_CHECK(my_free_list_best_fit(100000U) == NULL); // nothing fits
    MYMALLOC_CHECK(my_free_list_count() == count_before);   // unchanged
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();
}

// Best fit does NOT mutate the list: a scan leaves it valid and unchanged.
void test_best_fit_scan_is_pure() {
    my_free_list_clear();
    const std::size_t sizes[3] = {64U, 128U, 256U};
    my_block_header* blocks[3] = {};
    MYMALLOC_CHECK(make_free_blocks(sizes, 3, blocks));
    const std::size_t count_before = my_free_list_count();
    my_block_header* const head_before = my_free_list_head();

    (void)my_free_list_best_fit(200U + my_block_user_offset());

    MYMALLOC_CHECK(my_free_list_count() == count_before);
    MYMALLOC_CHECK(my_free_list_head() == head_before);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    (void)blocks;

    my_free_list_clear();
}

// ---------------------------------------------------------------------------
// Strategy selector (my_set_strategy / my_get_strategy)
// ---------------------------------------------------------------------------

// The default is FIRST_FIT (unchanged since Phase 6).
void test_strategy_defaults_to_first_fit() {
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_FIRST_FIT);
}

// Set/get round-trips for both known strategies.
void test_strategy_set_get_round_trip() {
    my_set_strategy(MY_STRATEGY_BEST_FIT);
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_BEST_FIT);
    my_set_strategy(MY_STRATEGY_FIRST_FIT);
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_FIRST_FIT);
    my_set_strategy(MY_STRATEGY_BEST_FIT); // leave a non-default in place
}

// Unknown values are ignored so a bogus cast cannot silently disable reuse.
void test_strategy_ignores_unknown() {
    my_set_strategy(MY_STRATEGY_BEST_FIT);
    my_set_strategy(static_cast<enum my_strategy>(2));         // not a known strategy
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_BEST_FIT); // unchanged
    my_set_strategy(MY_STRATEGY_FIRST_FIT);
}

// ---------------------------------------------------------------------------
// End-to-end: my_malloc actually honors the active strategy
// ---------------------------------------------------------------------------

// Two freed regions of different page counts become two free blocks of
// different sizes (separate mmaps, so no coalescing). A small request that
// fits BOTH must, under BEST fit, take the smaller block and leave the large
// one intact — the only block still >= 2 pages. This proves my_malloc
// dispatches to the selected strategy rather than always first-fitting.
void test_best_fit_preserves_large_block() {
    my_free_list_clear();
    const std::size_t page = my_raw_page_size();

    void* big = my_malloc(page);  // page payload -> 2-page region (8192)
    void* small = my_malloc(64U); // -> 1-page region (4096)
    MYMALLOC_CHECK(big != nullptr && small != nullptr && big != small);
    if (big == nullptr || small == nullptr) {
        return;
    }
    my_free(big);
    my_free(small);
    MYMALLOC_CHECK(my_free_list_count() == 2); // two independent free blocks

    my_set_strategy(MY_STRATEGY_BEST_FIT);
    void* picked = my_malloc(64U); // fits both; best fit must take the 4096 one
    MYMALLOC_CHECK(picked != nullptr);
    if (picked != nullptr) {
        // The 2-page block survived untouched: it is the only block >= 2 pages.
        my_block_header* const still_big = my_free_list_best_fit(2U * page);
        MYMALLOC_CHECK(still_big != nullptr);
        if (still_big != nullptr) {
            MYMALLOC_CHECK(still_big->size >= 2U * page);
        }
        my_free(picked);
    }
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    teardown();
}

// Under the DEFAULT (first fit) the same cycle still succeeds and leaves a
// consistent list — first fit never fails to reuse, it just may pick a larger
// block than best fit would.
void test_first_fit_default_reuses() {
    my_free_list_clear();
    my_set_strategy(MY_STRATEGY_FIRST_FIT);

    void* a = my_malloc(128U);
    void* b = my_malloc(256U);
    MYMALLOC_CHECK(a != nullptr && b != nullptr && a != b);
    if (a == nullptr || b == nullptr) {
        return;
    }
    my_free(a);
    my_free(b);
    MYMALLOC_CHECK(my_free_list_count() == 2);

    void* c = my_malloc(128U); // reused first fit from the list
    MYMALLOC_CHECK(c != nullptr);
    if (c != nullptr) {
        my_free(c);
    }
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    teardown();
}

} // namespace

int main() {
    test_best_fit_picks_smallest();
    test_best_fit_agrees_with_first_when_unique();
    test_best_fit_none_fits();
    test_best_fit_scan_is_pure();
    test_strategy_defaults_to_first_fit();
    test_strategy_set_get_round_trip();
    test_strategy_ignores_unknown();
    test_best_fit_preserves_large_block();
    test_first_fit_default_reuses();
    MYMALLOC_TEST_MAIN("strategy");
}
