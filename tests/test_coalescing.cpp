// Phase 8 test: verify block coalescing against the contract documented in
// docs/phase8_coalescing.md:
//   * my_block_merge fuses two linked, address-ordered neighbors into the
//     lower block, summing sizes and repairing the physical chain,
//   * it is a pure physical operation — free flags are the caller's job,
//   * refusal cases (NULL, non-adjacent, invalid) leave both blocks intact,
//   * my_free coalesces a freed block with its FREE neighbors and publishes
//     exactly one survivor, so the split/free cycles of Phases 6-7 do not
//     fragment a region into many small free blocks (fragmentation reduction).
#include <cstddef>
#include <cstdint>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include "test_framework.hpp"

namespace {

// Regions acquired during the test; released by teardown().
void* g_regions[32] = {};

// A page-rounded region large enough to hold a `size`-byte block.
void* acquire_region(std::size_t index, std::size_t size) {
    void* region = my_raw_acquire(size);
    if (region != nullptr && index < sizeof(g_regions) / sizeof(g_regions[0])) {
        g_regions[index] = region;
    }
    return region;
}

void teardown() {
    for (std::size_t i = 0; i < sizeof(g_regions) / sizeof(g_regions[0]); ++i) {
        if (g_regions[i] != nullptr) {
            my_raw_release(g_regions[i], my_raw_page_size() * 64U);
            g_regions[i] = nullptr;
        }
    }
}

// Layout invariants shared by a lone block and by the survivor of a merge.
bool sane_block(const my_block_header* block, const my_block_header* expected_prev,
                const my_block_header* expected_next) {
    if (my_block_valid(block) != 1) {
        return false;
    }
    if ((block->size % my_default_alignment()) != 0U) {
        return false;
    }
    if (block->prev != expected_prev || block->next != expected_next) {
        return false;
    }
    if (expected_prev != nullptr) {
        if (expected_prev->next != block) {
            return false;
        }
        if (reinterpret_cast<std::uintptr_t>(expected_prev) + expected_prev->size !=
            reinterpret_cast<std::uintptr_t>(block)) {
            return false;
        }
    }
    if (expected_next != nullptr) {
        if (expected_next->prev != block) {
            return false;
        }
        if (reinterpret_cast<std::uintptr_t>(block) + block->size !=
            reinterpret_cast<std::uintptr_t>(expected_next)) {
            return false;
        }
    }
    return true;
}

// Two linked, adjacent free blocks: merging keeps the LOWER address, sums the
// sizes, and bridges the physical chain over the absorbed upper block.
void test_lone_merge() {
    void* region = acquire_region(0, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t half = 1024U; // exact multiple of the default alignment
    auto* lower = reinterpret_cast<my_block_header*>(region);
    auto* upper =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(region) + half);
    MYMALLOC_CHECK(my_block_init(lower, half, 1 /* free */, nullptr, upper) == 1);
    MYMALLOC_CHECK(my_block_init(upper, half, 1 /* free */, lower, nullptr) == 1);

    MYMALLOC_CHECK(my_block_merge(lower, upper) == 1);

    // Survivor keeps the lower address, its prev, and now spans both extents.
    MYMALLOC_CHECK(reinterpret_cast<std::uintptr_t>(lower) ==
                   reinterpret_cast<std::uintptr_t>(region));
    MYMALLOC_CHECK(lower->size == half + half);
    MYMALLOC_CHECK(lower->free == 1);
    MYMALLOC_CHECK(sane_block(lower, nullptr, nullptr));

    teardown();
}

// Merging the lower of three adjacent blocks absorbs the middle: the survivor
// spans lower+middle and re-links the upper block to itself.
void test_merge_with_upper_neighbor() {
    void* region = acquire_region(1, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t chunk = 1024U;
    auto* base = static_cast<unsigned char*>(region);
    auto* a = reinterpret_cast<my_block_header*>(base);
    auto* b = reinterpret_cast<my_block_header*>(base + chunk);
    auto* c = reinterpret_cast<my_block_header*>(base + 2U * chunk);
    MYMALLOC_CHECK(my_block_init(a, chunk, 1, nullptr, b) == 1);
    MYMALLOC_CHECK(my_block_init(b, chunk, 1, a, c) == 1);
    MYMALLOC_CHECK(my_block_init(c, chunk, 1, b, nullptr) == 1);

    MYMALLOC_CHECK(my_block_merge(a, b) == 1);

    MYMALLOC_CHECK(a->size == 2U * chunk);
    MYMALLOC_CHECK(sane_block(a, nullptr, c));
    MYMALLOC_CHECK(sane_block(c, a, nullptr));

    teardown();
}

// my_block_merge is a pure physical operation: it never rewrites the free
// flag of either half. Reconciling free state is the caller's responsibility
// (the coalescing path in my_free relies on this).
void test_merge_preserves_free_flags() {
    void* region = acquire_region(2, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t half = 512U;
    auto* lower = reinterpret_cast<my_block_header*>(region);
    auto* upper =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(region) + half);
    MYMALLOC_CHECK(my_block_init(lower, half, 0 /* allocated */, nullptr, upper) == 1);
    MYMALLOC_CHECK(my_block_init(upper, half, 1 /* free */, lower, nullptr) == 1);

    MYMALLOC_CHECK(my_block_merge(lower, upper) == 1);

    // my_block_merge is a pure PHYSICAL primitive: it fuses the extents and
    // repairs the chain but leaves the survivor's free bit EXACTLY as it was
    // (here: allocated). Managing free state is the caller's job (my_free).
    MYMALLOC_CHECK(lower->size == 2U * half);
    MYMALLOC_CHECK(lower->free == 0);       // untouched by the merge
    MYMALLOC_CHECK(lower->next == nullptr); // upper->next was NULL
    MYMALLOC_CHECK(lower->prev == nullptr);
    MYMALLOC_CHECK(my_block_valid(lower) == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1); // merge never touched the list
}

// Refusals leave both blocks untouched: NULL inputs, and a pair that is not
// linked as direct neighbors.
void test_merge_refusals() {
    void* region = acquire_region(3, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t chunk = 1024U;
    auto* base = static_cast<unsigned char*>(region);
    auto* a = reinterpret_cast<my_block_header*>(base);
    auto* b = reinterpret_cast<my_block_header*>(base + chunk);
    auto* c = reinterpret_cast<my_block_header*>(base + 2U * chunk);
    MYMALLOC_CHECK(my_block_init(a, chunk, 1, nullptr, b) == 1);
    MYMALLOC_CHECK(my_block_init(b, chunk, 1, a, c) == 1);
    MYMALLOC_CHECK(my_block_init(c, chunk, 1, b, nullptr) == 1);

    // NULL operands.
    MYMALLOC_CHECK(my_block_merge(nullptr, b) == 0);
    MYMALLOC_CHECK(my_block_merge(a, nullptr) == 0);
    // a and c are NOT adjacent (b sits between them): a->next != c.
    MYMALLOC_CHECK(my_block_merge(a, c) == 0);
    // Reversed order is not the linked neighbor pair either.
    MYMALLOC_CHECK(my_block_merge(b, a) == 0);

    // A corrupted header makes one operand invalid.
    a->size = 100U; // not an alignment multiple
    MYMALLOC_CHECK(my_block_valid(a) == 0);
    MYMALLOC_CHECK(my_block_merge(a, b) == 0);

    // None of the refusals mutated the still-valid operands.
    MYMALLOC_CHECK(b->size == chunk);
    MYMALLOC_CHECK(b->prev == a);
    MYMALLOC_CHECK(b->next == c);
    MYMALLOC_CHECK(c->size == chunk);
    MYMALLOC_CHECK(c->prev == b);

    teardown();
}

// End-to-end via the public API: a split region's allocated front, when
// freed, coalesces with its listed remainder back into a single whole-region
// free block — the round-trip that undoes Phase 7 splitting.
void test_free_coalesces_with_remainder() {
    my_free_list_clear();

    // One whole-page free block (block size 4096).
    void* a = my_malloc(4096U - my_block_user_offset());
    MYMALLOC_CHECK(a != nullptr);
    if (a == nullptr) {
        return;
    }
    my_free(a);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // A small request splits it: an allocated front plus a listed remainder.
    void* b = my_malloc(16U);
    MYMALLOC_CHECK(b != nullptr);
    if (b == nullptr) {
        return;
    }
    my_block_header* const front = my_user_to_block(b);
    my_block_header* const remainder = front->next;
    MYMALLOC_CHECK(remainder != nullptr);
    if (remainder == nullptr) {
        return;
    }
    MYMALLOC_CHECK(front->free == 0);
    MYMALLOC_CHECK(remainder->free == 1);
    MYMALLOC_CHECK(my_free_list_count() == 1); // only the remainder is listed

    // Freeing the front coalesces it with the remainder: one block again.
    my_free(b);
    MYMALLOC_CHECK(my_free_list_count() == 1); // NOT 2 — coalescing merged them
    my_block_header* const head = my_free_list_head();
    MYMALLOC_CHECK(head != nullptr);
    if (head == nullptr) {
        return;
    }
    MYMALLOC_CHECK(head->size == 4096U); // the whole region is one block again
    MYMALLOC_CHECK(head->prev == nullptr);
    MYMALLOC_CHECK(head->next == nullptr);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();
}

// Fragmentation reduction: repeatedly split a region and free the front.
// Without coalescing each free would ADD a listed block (count growing 1,2,3…);
// with coalescing the count stays constant and the region never fragments —
// it remains a single whole-region free block after every cycle.
void test_fragmentation_cycle() {
    my_free_list_clear();

    // One region of two pages (block size 8192) as a single free block.
    void* base = my_malloc(8192U - my_block_user_offset());
    MYMALLOC_CHECK(base != nullptr);
    if (base == nullptr) {
        return;
    }
    my_free(base);
    MYMALLOC_CHECK(my_free_list_count() == 1);

    const int cycles = 64;
    for (int i = 0; i < cycles; ++i) {
        void* x = my_malloc(16U); // splits: allocated front + listed remainder
        MYMALLOC_CHECK(x != nullptr);
        if (x == nullptr) {
            return;
        }
        MYMALLOC_CHECK(my_free_list_count() == 1); // the remainder, not the front
        MYMALLOC_CHECK(my_free_list_valid() == 1);

        my_free(x);                                // coalesces the front back into the remainder
        MYMALLOC_CHECK(my_free_list_count() == 1); // still a single free block
        MYMALLOC_CHECK(my_free_list_valid() == 1);

        my_block_header* const head = my_free_list_head();
        MYMALLOC_CHECK(head != nullptr);
        if (head == nullptr) {
            return;
        }
        MYMALLOC_CHECK(head->size == 8192U); // no fragmentation accrued
    }

    my_free_list_clear();
}

} // namespace

int main() {
    test_lone_merge();
    test_merge_with_upper_neighbor();
    test_merge_preserves_free_flags();
    test_merge_refusals();
    test_free_coalesces_with_remainder();
    test_fragmentation_cycle();
    MYMALLOC_TEST_MAIN("coalescing");
}
