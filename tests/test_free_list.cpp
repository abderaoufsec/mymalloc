// mymalloc Phase 6 — free list and first-fit reuse tests.
//
// Directly exercises the free-list module (insert/remove/first-fit/clear and
// the my_free_list_valid() consistency audit over a small crafted region),
// then verifies end-to-end block REUSE through my_malloc/my_free: freed
// blocks are recycled before fresh regions are acquired, and first-fit
// returns the lowest-addressed adequate block deterministically.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include <cstdint>
#include <cstdio>

#include "test_framework.hpp"

namespace {

// Global so ASan's leak check sees crafted regions still reachable at exit
// (regions are released in normal runs; this only guards abort paths).
void* g_regions[8] = {};

my_block_header* first_fit_block(std::size_t bytes) {
    return my_free_list_first_fit(bytes);
}

} // namespace

int main() {
    // --- Empty state ---------------------------------------------------
    MYMALLOC_CHECK(my_free_list_head() == nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 0);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    MYMALLOC_CHECK(first_fit_block(0) == nullptr); // defensive
    MYMALLOC_CHECK(first_fit_block(4096) == nullptr);
    MYMALLOC_CHECK(my_free_list_insert(nullptr) == 0);
    MYMALLOC_CHECK(my_free_list_remove(nullptr) == 0);

    // --- Crafted free blocks in raw regions ----------------------------
    const std::size_t page = my_raw_page_size();
    // Two crafted blocks: same size, different addresses.
    void* ra = my_raw_acquire(page);
    void* rb = my_raw_acquire(page);
    g_regions[0] = ra;
    g_regions[1] = rb;
    MYMALLOC_CHECK(ra != nullptr && rb != nullptr && ra != rb);
    auto* ba = reinterpret_cast<my_block_header*>(ra);
    auto* bb = reinterpret_cast<my_block_header*>(rb);
    MYMALLOC_CHECK(my_block_init(ba, page, 1 /* free */, nullptr, nullptr) == 1);
    MYMALLOC_CHECK(my_block_init(bb, page, 1 /* free */, nullptr, nullptr) == 1);

    // Insert both (order chosen so the list must re-sort by address);
    // membership, count and the audit all follow.
    my_block_header* lower = ba;
    my_block_header* higher = bb;
    if (reinterpret_cast<std::uintptr_t>(ba) > reinterpret_cast<std::uintptr_t>(bb)) {
        lower = bb;
        higher = ba;
    }
    MYMALLOC_CHECK(my_free_list_insert(higher) == 1);
    MYMALLOC_CHECK(my_free_list_insert(lower) == 1);
    MYMALLOC_CHECK(my_free_list_count() == 2);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    MYMALLOC_CHECK(my_free_list_head() == lower);
    MYMALLOC_CHECK(my_free_list_links(higher)->prev == my_free_list_links(lower));

    // Double-insert is refused; links of a member are never re-read.
    MYMALLOC_CHECK(my_free_list_insert(lower) == 0);
    MYMALLOC_CHECK(my_free_list_count() == 2);

    // Remove: cleared links, in-list then not-in-list outcomes.
    MYMALLOC_CHECK(my_free_list_remove(lower) == 1);
    MYMALLOC_CHECK(my_free_list_links(lower)->prev == nullptr);
    MYMALLOC_CHECK(my_free_list_links(lower)->next == nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    MYMALLOC_CHECK(my_free_list_remove(lower) == 0);
    MYMALLOC_CHECK(my_free_list_count() == 1);

    // Re-insert for the audit-corruption tests below.
    MYMALLOC_CHECK(my_free_list_insert(lower) == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // --- Consistency audit detects damage ------------------------------
    higher->free = 0;
    MYMALLOC_CHECK(my_free_list_valid() == 0);
    higher->free = 1;

    my_free_list_links(higher)->prev = nullptr; // broken reciprocity
    MYMALLOC_CHECK(my_free_list_valid() == 0);
    my_free_list_links(higher)->prev = my_free_list_links(lower);

    my_free_list_links(lower)->next = nullptr; // severed chain
    MYMALLOC_CHECK(my_free_list_valid() == 0);
    my_free_list_links(lower)->next = my_free_list_links(higher);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // --- Insert refusals (leave the list untouched) --------------------
    void* rc = my_raw_acquire(page); // one more region for states below
    g_regions[2] = rc;
    MYMALLOC_CHECK(rc != nullptr);
    auto* bc = reinterpret_cast<my_block_header*>(rc);

    // An allocated (free == 0) block never enters the list.
    MYMALLOC_CHECK(my_block_init(bc, page, 0 /* allocated */, nullptr, nullptr) == 1);
    MYMALLOC_CHECK(my_free_list_insert(bc) == 0);
    MYMALLOC_CHECK(my_free_list_count() == 2);

    // A block too small to hold my_free_links is refused.
    void* rt = my_raw_acquire(page);
    g_regions[3] = rt;
    MYMALLOC_CHECK(rt != nullptr);
    auto* bt = reinterpret_cast<my_block_header*>(rt);
    const std::size_t tiny = my_block_user_offset(); // zero payload capacity
    MYMALLOC_CHECK(my_block_init(bt, tiny, 1 /* free */, nullptr, nullptr) == 1);
    MYMALLOC_CHECK(my_free_list_insert(bt) == 0);
    MYMALLOC_CHECK(my_free_list_count() == 2);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // --- First fit: lowest-address adequate block, no list mutation -----
    const std::size_t small_need = page / 2;
    const std::size_t big_need = page + 1; // each block is exactly one page
    MYMALLOC_CHECK(first_fit_block(0) == nullptr);
    MYMALLOC_CHECK(first_fit_block(small_need) == lower); // both fit; lowest wins
    MYMALLOC_CHECK(first_fit_block(big_need) == nullptr); // neither fits
    MYMALLOC_CHECK(first_fit_block(SIZE_MAX) == nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 2); // scans never mutate
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // --- End-to-end reuse through my_malloc / my_free -------------------
    my_free_list_clear();
    MYMALLOC_CHECK(my_free_list_count() == 0);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    void* p1 = my_malloc(64);
    MYMALLOC_CHECK(p1 != nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 0); // fresh acquisition, nothing free
    my_free(p1);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    MYMALLOC_CHECK(my_free_list_head() == my_user_to_block(p1));

    // Same-size allocation reuses the freed block: no new region needed.
    void* p2 = my_malloc(64);
    MYMALLOC_CHECK(p2 == p1); // deterministic first fit over the singleton
    MYMALLOC_CHECK(my_free_list_count() == 0);
    MYMALLOC_CHECK(my_block_is_free(my_user_to_block(p2)) == 0);
    MYMALLOC_CHECK(my_block_valid(my_user_to_block(p2)) == 1);

    // A too-small free list entry is bypassed by a large request.
    my_free(p2);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    void* bigp = my_malloc(2 * 1024 * 1024); // 2 MiB > the 96-byte block
    MYMALLOC_CHECK(bigp != nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 1); // small block still listed
    my_free(bigp);
    MYMALLOC_CHECK(my_free_list_count() == 2);

    // Double free is a guarded no-op: no second list entry appears.
    my_free(p1); // p1 == p2, whose block is already free
    MYMALLOC_CHECK(my_free_list_count() == 2);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // Mixed-size allocations drain and refill the list deterministically.
    void* x = my_malloc(16);  // always reuses the 96-byte listed block
    void* y = my_malloc(128); // 160 bytes: reuses or takes a fresh region
    void* z = my_malloc(32);  // 64 bytes: reuses whichever small block remains
    MYMALLOC_CHECK(x != nullptr && y != nullptr && z != nullptr);
    my_free(y);
    my_free(x);
    my_free(z);
    // The two listed blocks came back, plus the one fresh region required.
    MYMALLOC_CHECK(my_free_list_count() == 3);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // Long reuse cycle: repeated same-size alloc/free keeps the audit green.
    for (int i = 0; i < 64; ++i) {
        void* t = my_malloc(64);
        MYMALLOC_CHECK(t != nullptr);
        std::snprintf(static_cast<char*>(t), 8, "%d", i);
        my_free(t);
        MYMALLOC_CHECK(my_free_list_valid() == 1);
    }

    // Teardown: unlist everything, then release the crafted regions.
    my_free_list_clear();
    for (void*& region : g_regions) {
        if (region != nullptr) {
            my_raw_release(region, page);
            region = nullptr;
        }
    }
    return 0;
}