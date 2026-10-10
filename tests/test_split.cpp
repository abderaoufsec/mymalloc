// Phase 7 test: verify block splitting (my_block_split) against the contract
// documented in docs/phase7_splitting.md:
//   * a valid block splits into FRONT (same address) + REMAINDER (tail),
//   * sizes add up and stay alignment multiples,
//   * the physical neighbor chain is repaired on both sides,
//   * both halves inherit the free state,
//   * refusal cases leave the block untouched,
//   * split-then-allocate reuse (oversized free block -> allocated front +
//     listed remainder), which is what my_malloc gains from splitting.
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

// Layout invariants shared by a lone block and by each half of a split.
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

void test_lone_split() {
    void* region = acquire_region(0, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t size = 4096U;
    auto* block = reinterpret_cast<my_block_header*>(region);
    MYMALLOC_CHECK(my_block_init(block, size, 1 /* free */, nullptr, nullptr) == 1);

    const std::size_t front_size = 1024U; // exact multiple of the default alignment
    MYMALLOC_CHECK(my_block_split(block, front_size) == 1);

    my_block_header* const remainder = block->next;
    MYMALLOC_CHECK(remainder != nullptr);
    if (remainder == nullptr) {
        return;
    }

    // Front keeps the address, size, prev, free state.
    MYMALLOC_CHECK(block->size == front_size);
    MYMALLOC_CHECK(block->free == 1);
    MYMALLOC_CHECK(block->prev == nullptr);
    // Remainder occupies the exact tail, same free state, chained correctly.
    MYMALLOC_CHECK(remainder->size == size - front_size);
    MYMALLOC_CHECK(remainder->free == 1);
    MYMALLOC_CHECK(remainder->prev == block);
    MYMALLOC_CHECK(remainder->next == nullptr);
    MYMALLOC_CHECK(reinterpret_cast<std::uintptr_t>(remainder) ==
                   reinterpret_cast<std::uintptr_t>(block) + front_size);

    MYMALLOC_CHECK(sane_block(block, nullptr, remainder));
    MYMALLOC_CHECK(sane_block(remainder, block, nullptr));

    teardown();
}

void test_middle_split() {
    void* region = acquire_region(1, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t front_size = 512U; // < mid's 1024 so a real remainder is left
    auto* base = reinterpret_cast<my_block_header*>(region);
    auto* below = base;
    auto* mid = reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(base) + 1024U);
    auto* upper =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(base) + 2048U);
    MYMALLOC_CHECK(my_block_init(below, 1024U, 0, nullptr, mid) == 1);
    MYMALLOC_CHECK(my_block_init(mid, 1024U, 0, below, upper) == 1);
    MYMALLOC_CHECK(my_block_init(upper, 1024U, 0, mid, nullptr) == 1);

    // Split the middle block; the upper neighbor must be re-linked.
    MYMALLOC_CHECK(my_block_split(mid, front_size) == 1);
    my_block_header* const remainder = mid->next;
    MYMALLOC_CHECK(remainder != nullptr);
    if (remainder == nullptr) {
        return;
    }

    MYMALLOC_CHECK(mid->size == front_size);
    MYMALLOC_CHECK(remainder->size == 1024U - front_size);
    MYMALLOC_CHECK(mid->prev == below);
    MYMALLOC_CHECK(mid->next == remainder);
    MYMALLOC_CHECK(remainder->prev == mid);
    MYMALLOC_CHECK(remainder->next == upper);
    MYMALLOC_CHECK(upper->prev == remainder);

    MYMALLOC_CHECK(sane_block(mid, below, remainder));
    MYMALLOC_CHECK(sane_block(remainder, mid, upper));
    MYMALLOC_CHECK(sane_block(upper, remainder, nullptr));
    MYMALLOC_CHECK(sane_block(below, nullptr, mid));

    teardown();
}

void test_last_block_split() {
    void* region = acquire_region(2, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    auto* base = reinterpret_cast<my_block_header*>(region);
    auto* below = base;
    auto* last = reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(base) + 2048U);
    MYMALLOC_CHECK(my_block_init(below, 2048U, 0, nullptr, last) == 1);
    MYMALLOC_CHECK(my_block_init(last, 2048U, 1 /* free */, below, nullptr) == 1);

    // Splitting the LAST block: it has no upper neighbor, so nothing above is
    // re-linked and the lower neighbor stays untouched.
    MYMALLOC_CHECK(my_block_split(last, 1024U) == 1);
    my_block_header* const remainder = last->next;
    MYMALLOC_CHECK(remainder != nullptr);
    if (remainder == nullptr) {
        return;
    }

    MYMALLOC_CHECK(remainder->next == nullptr); // nothing above to re-link
    MYMALLOC_CHECK(remainder->prev == last);
    MYMALLOC_CHECK(below->next == last); // the lower neighbor is untouched

    MYMALLOC_CHECK(sane_block(below, nullptr, last));
    MYMALLOC_CHECK(sane_block(last, below, remainder));
    MYMALLOC_CHECK(sane_block(remainder, last, nullptr));

    teardown();
}

void test_refusals() {
    void* region = acquire_region(3, my_raw_page_size() * 64U);
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    const std::size_t size = 4096U;
    auto* block = reinterpret_cast<my_block_header*>(region);
    MYMALLOC_CHECK(my_block_init(block, size, 1 /* free */, nullptr, nullptr) == 1);

    // Bad front sizes: zero, unaligned, below the minimum block size.
    MYMALLOC_CHECK(my_block_split(block, 0) == 0);
    MYMALLOC_CHECK(my_block_split(block, 100) == 0); // not an alignment multiple
    MYMALLOC_CHECK(my_block_split(block, my_block_user_offset() - 8U) == 0);
    // front == size and front > size leave no remainder.
    MYMALLOC_CHECK(my_block_split(block, size) == 0);
    MYMALLOC_CHECK(my_block_split(block, size + 64U) == 0);
    // A front that would leave a sub-header remainder is refused.
    MYMALLOC_CHECK(my_block_split(block, size - 16U) == 0);
    // NULL and misaligned inputs.
    MYMALLOC_CHECK(my_block_split(nullptr, 64U) == 0);
    MYMALLOC_CHECK(my_block_split(reinterpret_cast<my_block_header*>(
                                      reinterpret_cast<unsigned char*>(region) + 1U),
                                  64U) == 0);

    // The failed splits must not have mutated the block.
    MYMALLOC_CHECK(block->size == size);
    MYMALLOC_CHECK(block->next == nullptr);
    MYMALLOC_CHECK(block->prev == nullptr);
    MYMALLOC_CHECK(block->free == 1);

    teardown();
}

void test_split_reuse() {
    my_free_list_clear();

    // One oversized free block (a whole page region, payload 4064).
    void* a = my_malloc(4096U - my_block_user_offset());
    MYMALLOC_CHECK(a != nullptr);
    if (a == nullptr) {
        return;
    }
    my_free(a);

    // A much smaller request splits the oversized free block: the front is
    // allocated, the remainder stays listed and is reusable.
    void* b = my_malloc(16U);
    MYMALLOC_CHECK(b != nullptr);
    if (b == nullptr) {
        return;
    }
    my_block_header* const front = my_user_to_block(b);
    MYMALLOC_CHECK(front->free == 0); // allocated front
    MYMALLOC_CHECK(front->size == my_block_user_offset() + 16U);
    my_block_header* const remainder = front->next;
    MYMALLOC_CHECK(remainder != nullptr);
    if (remainder == nullptr) {
        return;
    }
    MYMALLOC_CHECK(remainder->free == 1); // remainder stays free
    MYMALLOC_CHECK(remainder->size == (4096U - (my_block_user_offset() + 16U)));
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // Reuse again: a 64-byte request fits in the listed remainder (no grow).
    void* c = my_malloc(64U);
    MYMALLOC_CHECK(c != nullptr);
    if (c == nullptr) {
        return;
    }
    MYMALLOC_CHECK(reinterpret_cast<std::uintptr_t>(c) >
                   reinterpret_cast<std::uintptr_t>(front)); // beyond the first front
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free(b);
    my_free(c);
    my_free_list_clear();
}

void test_remainder_too_small() {
    my_free_list_clear();

    // A 2-page region gives a 8192-byte block (payload 8160).
    void* big = my_malloc(8192U - my_block_user_offset());
    MYMALLOC_CHECK(big != nullptr);
    if (big == nullptr) {
        return;
    }
    my_free(big);

    // Request size chosen so the leftover is exactly below the worthwhile
    // remainder threshold -> the block is reused whole, NOT split.
    const std::size_t min_remainder = my_block_user_offset() + sizeof(my_free_links);
    const std::size_t request = 8192U - min_remainder + 8U; // remainder < threshold
    void* p = my_malloc(request);
    MYMALLOC_CHECK(p != nullptr);
    if (p == nullptr) {
        return;
    }
    my_block_header* const block = my_user_to_block(p);
    MYMALLOC_CHECK(block->size == 8192U);   // reused whole (payload slack kept)
    MYMALLOC_CHECK(block->next == nullptr); // no remainder block created
    MYMALLOC_CHECK(block->free == 0);
    MYMALLOC_CHECK(my_free_list_count() == 0);

    my_free(p);
    my_free_list_clear();
}

} // namespace

int main() {
    test_lone_split();
    test_middle_split();
    test_last_block_split();
    test_refusals();
    test_split_reuse();
    test_remainder_too_small();
    MYMALLOC_TEST_MAIN("split");
}