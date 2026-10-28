// Phase 11 test: verify my_realloc (<mymalloc/mymalloc.h>) against the
// contract documented in docs/phase11_realloc.md:
//   * realloc(NULL, n) behaves like malloc(n); realloc(p, 0) frees and
//     returns NULL,
//   * shrink / grow-within-existing-slack resizes IN PLACE (same pointer,
//     prefix data preserved) and carves a worthwhile remainder off,
//   * grow absorbs a sufficient FREE upper neighbor in place (coalescing
//     the split-off tail through the Phase 8 path),
//   * an impossible grow MOVES: fresh block, overlap copied, old block freed,
//   * allocation failure returns NULL and leaves the original block intact,
//   * foreign and already-freed pointers are refused.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include "test_framework.hpp"

namespace {

// Regions acquired during the test; released by teardown(). Every region is
// page*64 so teardown can release exactly what acquire_region acquired.
void* g_regions[32] = {};
std::size_t g_region_count = 0;

void* acquire_region() {
    void* region = my_raw_acquire(my_raw_page_size() * 64U);
    if (region != nullptr && g_region_count < sizeof(g_regions) / sizeof(g_regions[0])) {
        g_regions[g_region_count++] = region;
    }
    return region;
}

void teardown() {
    my_free_list_clear(); // drop listed blocks before the memory goes away
    for (std::size_t i = 0; i < g_region_count; ++i) {
        my_raw_release(g_regions[i], my_raw_page_size() * 64U);
        g_regions[i] = nullptr;
    }
    g_region_count = 0;
}

// Data-preservation pattern: byte i holds (i & 0xFF). check_pattern(ptr, n)
// therefore also pins down that the first n bytes were NOT shifted or zeroed.
void fill_pattern(void* ptr, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>(i & 0xFFU);
    }
}

bool check_pattern(const void* ptr, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != static_cast<unsigned char>(i & 0xFFU)) {
            return false;
        }
    }
    return true;
}

// Handcrafts "A allocated (size_a) | B free (size_b, listed)" at the start of
// a fresh region — the layout realloc's shrink/absorb paths need but the
// public API cannot place on demand. Returns A's user pointer.
void* handcraft_pair(std::size_t size_a, std::size_t size_b) {
    void* region = acquire_region();
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return nullptr;
    }
    auto* block_a = reinterpret_cast<my_block_header*>(region);
    auto* block_b =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(region) + size_a);
    MYMALLOC_CHECK(my_block_init(block_a, size_a, 0 /* allocated */, nullptr, block_b) == 1);
    MYMALLOC_CHECK(my_block_init(block_b, size_b, 1 /* free */, block_a, nullptr) == 1);
    MYMALLOC_CHECK(my_free_list_insert(block_b) == 1);
    return my_block_to_user(block_a);
}

// Handcrafts a lone ALLOCATED block (no upper neighbor) in a fresh region.
void* handcraft_lone(std::size_t size) {
    void* region = acquire_region();
    MYMALLOC_CHECK(region != nullptr);
    if (region == nullptr) {
        return nullptr;
    }
    auto* block = reinterpret_cast<my_block_header*>(region);
    MYMALLOC_CHECK(my_block_init(block, size, 0 /* allocated */, nullptr, nullptr) == 1);
    return my_block_to_user(block);
}

// --- nullptr and zero-size semantics ---------------------------------------
void test_nullptr_and_zero() {
    my_free_list_clear();

    // realloc(NULL, n) is malloc(n): aligned, valid, writable.
    void* from_null = my_realloc(nullptr, 64U);
    MYMALLOC_CHECK(from_null != nullptr);
    MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(from_null) % my_default_alignment() == 0U);
    my_block_header* header = my_user_to_block(from_null);
    MYMALLOC_CHECK(my_block_valid(header) == 1);
    MYMALLOC_CHECK(my_block_is_free(header) == 0);
    std::memset(from_null, 0xAB, 64U);

    // realloc(p, 0) frees the block and returns NULL.
    void* zeroed = my_realloc(from_null, 0U);
    MYMALLOC_CHECK(zeroed == nullptr);
    MYMALLOC_CHECK(my_block_is_free(header) == 1); // the block really was freed
    MYMALLOC_CHECK(my_free_list_count() == 1);     // and published exactly once
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    // realloc(NULL, 0) collapses to the my_malloc(0) failure.
    MYMALLOC_CHECK(my_realloc(nullptr, 0U) == nullptr);

    my_free_list_clear();
}

// --- grow within the block's existing slack: same pointer, no list traffic -
void test_grow_within_slack() {
    my_free_list_clear();

    void* pointer = my_malloc(64U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_block_header* header = my_user_to_block(pointer);
    const std::size_t old_size = header->size;
    fill_pattern(pointer, 64U);

    // Grow to exactly fill the page-sized fresh region: total == block->size,
    // so no remainder exists to split — a pure header-level resize with zero
    // free-list traffic. (Smaller totals DO split a worthwhile remainder;
    // the shrink tests below pin that behaviour down.)
    const std::size_t fill = old_size - my_block_user_offset();
    void* grown = my_realloc(pointer, fill);
    MYMALLOC_CHECK(grown == pointer); // in place
    MYMALLOC_CHECK(my_user_to_block(grown) == header);
    MYMALLOC_CHECK(header->size == old_size); // extent untouched
    MYMALLOC_CHECK(my_block_payload_size(header) >= fill);
    MYMALLOC_CHECK(check_pattern(grown, 64U) == true); // prefix preserved
    MYMALLOC_CHECK(my_free_list_count() == 1);         // nothing carved (pre-existing remainder)

    // Same size: a no-op that still returns the same pointer.
    void* same = my_realloc(grown, fill);
    MYMALLOC_CHECK(same == grown);
    MYMALLOC_CHECK(check_pattern(same, 64U) == true);

    my_free(grown);
}

// --- shrink in place: split the slack off, coalesce with a free neighbor ---
void test_shrink_in_place() {
    my_free_list_clear();

    // A (1024, allocated) | B (1024, free, listed). Shrinking A to 100 needs
    // total = 144 bytes; the 880-byte slack is carved out, flipped to free,
    // and coalesced with B — one 1920-byte survivor in the list, NOT two
    // adjacent free blocks (the Phase 8 invariant).
    void* pointer = handcraft_pair(1024U, 1024U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_block_header* block_a = my_user_to_block(pointer);
    fill_pattern(pointer, 992U); // the whole old payload

    void* shrunk = my_realloc(pointer, 100U);
    MYMALLOC_CHECK(shrunk == pointer);                   // in place — no move on shrink
    MYMALLOC_CHECK(check_pattern(shrunk, 100U) == true); // prefix preserved
    MYMALLOC_CHECK(my_block_valid(block_a) == 1);
    MYMALLOC_CHECK(block_a->size == 144U); // 100 -> align 112 + 32 header
    MYMALLOC_CHECK(block_a->free == 0);
    MYMALLOC_CHECK(my_block_payload_size(block_a) >= 100U);

    my_block_header* tail = block_a->next; // the carved remainder + B
    MYMALLOC_CHECK(tail != nullptr);
    if (tail == nullptr) {
        return;
    }
    MYMALLOC_CHECK(tail->size == 1904U); // (1024 - 144) carved + 1024 absorbed
    MYMALLOC_CHECK(tail->free == 1);
    MYMALLOC_CHECK(tail->next == nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 1); // exactly one survivor listed
    MYMALLOC_CHECK(my_free_list_head() == tail);
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();

    // A lone block kept whole when the remainder would be a sliver: shrinking
    // 1024 -> 976 (total 1008, remainder 16 < the 48-byte listable minimum)
    // leaves the extent untouched, and the exact-fit 992 is a pure no-op.
    void* lone = handcraft_lone(1024U);
    MYMALLOC_CHECK(lone != nullptr);
    if (lone == nullptr) {
        return;
    }
    my_block_header* block = my_user_to_block(lone);
    fill_pattern(lone, 992U);

    void* sliver = my_realloc(lone, 976U);
    MYMALLOC_CHECK(sliver == lone);
    MYMALLOC_CHECK(block->size == 1024U);      // no split: slack stays internal
    MYMALLOC_CHECK(my_free_list_count() == 0); // no new free block appeared
    MYMALLOC_CHECK(check_pattern(sliver, 976U) == true);

    void* exact = my_realloc(sliver, 992U); // total == block->size
    MYMALLOC_CHECK(exact == lone);
    MYMALLOC_CHECK(block->size == 1024U);
    MYMALLOC_CHECK(my_free_list_count() == 0);
    MYMALLOC_CHECK(check_pattern(exact, 992U) == true);

    my_free(lone);
}

// --- grow in place: absorb a sufficient FREE upper neighbor ----------------
void test_absorb_upper_neighbor() {
    my_free_list_clear();

    // A (1024, allocated) | B (1024, free, listed). Growing A to 1600 needs
    // 1632 bytes: 1024 alone is short, but the neighbor supplies the rest.
    // The 416-byte tail left over after the merge is split off and listed.
    void* pointer = handcraft_pair(1024U, 1024U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_block_header* block_a = my_user_to_block(pointer);
    fill_pattern(pointer, 992U); // the whole old payload

    void* grown = my_realloc(pointer, 1600U);
    MYMALLOC_CHECK(grown == pointer);                   // in place — no move on absorb
    MYMALLOC_CHECK(check_pattern(grown, 992U) == true); // ALL old data kept
    MYMALLOC_CHECK(my_block_valid(block_a) == 1);
    MYMALLOC_CHECK(block_a->size == 1632U); // 1600 aligned + 32-byte header
    MYMALLOC_CHECK(block_a->free == 0);
    MYMALLOC_CHECK(my_block_payload_size(block_a) >= 1600U);

    my_block_header* tail = block_a->next;
    MYMALLOC_CHECK(tail != nullptr);
    if (tail != nullptr) {
        MYMALLOC_CHECK(tail->size == 416U); // merged 2048 - 1632 given back
        MYMALLOC_CHECK(tail->free == 1);
        MYMALLOC_CHECK(tail->next == nullptr);
    }
    MYMALLOC_CHECK(my_free_list_count() == 1); // B removed, tail listed
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();

    // Absorb without a tail split: A (512) | B (1024) grown to 1488 needs
    // 1520 == 512 + 1024 - 16, so only a 16-byte sliver would remain and the
    // whole absorbed extent stays with the allocation. The list ends EMPTY —
    // the neighbor was consumed, nothing was re-listed.
    void* wide = handcraft_pair(512U, 1024U);
    MYMALLOC_CHECK(wide != nullptr);
    if (wide == nullptr) {
        return;
    }
    my_block_header* wide_block = my_user_to_block(wide);
    fill_pattern(wide, 480U);

    void* absorbed = my_realloc(wide, 1488U);
    MYMALLOC_CHECK(absorbed == wide);
    MYMALLOC_CHECK(wide_block->size == 1536U); // 512 + 1024, no split
    MYMALLOC_CHECK(wide_block->next == nullptr);
    MYMALLOC_CHECK(my_block_valid(wide_block) == 1);
    MYMALLOC_CHECK(my_block_payload_size(wide_block) >= 1488U);
    MYMALLOC_CHECK(check_pattern(absorbed, 480U) == true);
    MYMALLOC_CHECK(my_free_list_count() == 0); // neighbor consumed whole
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();
}

// --- move when the block (and its neighbor) cannot satisfy the request ------
void test_move_when_necessary() {
    my_free_list_clear();

    // Lone 1024-byte block grown to 8000: nothing to absorb, so realloc must
    // allocate fresh, copy the overlap, and free the old block.
    void* pointer = handcraft_lone(1024U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_block_header* old_block = my_user_to_block(pointer);
    fill_pattern(pointer, 992U);

    void* moved = my_realloc(pointer, 8000U);
    MYMALLOC_CHECK(moved != nullptr);
    MYMALLOC_CHECK(moved != pointer);                   // different address = real move
    MYMALLOC_CHECK(check_pattern(moved, 992U) == true); // overlap copied
    my_block_header* new_block = my_user_to_block(moved);
    MYMALLOC_CHECK(my_block_valid(new_block) == 1);
    MYMALLOC_CHECK(my_block_is_free(new_block) == 0);
    MYMALLOC_CHECK(my_block_payload_size(new_block) >= 8000U);

    // The old block rejoined the free list through my_free.
    MYMALLOC_CHECK(my_block_is_free(old_block) == 1);
    MYMALLOC_CHECK(my_free_list_count() == 2); // old_block + new region remainder
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free(moved);
    my_free_list_clear();

    // A (512, allocated) | B (512, free, listed), grown to 4000: B alone
    // cannot bridge the 3520-byte gap, so realloc moves — and freeing A then
    // coalesces it with B into a single 1024-byte survivor (Phase 8).
    void* pair = handcraft_pair(512U, 512U);
    MYMALLOC_CHECK(pair != nullptr);
    if (pair == nullptr) {
        return;
    }
    my_block_header* pair_a = my_user_to_block(pair);
    fill_pattern(pair, 480U);

    void* resized = my_realloc(pair, 4000U);
    MYMALLOC_CHECK(resized != nullptr);
    MYMALLOC_CHECK(resized != pair); // insufficient neighbor -> move
    MYMALLOC_CHECK(check_pattern(resized, 480U) == true);
    MYMALLOC_CHECK(my_free_list_count() == 2); // A+B coalesced survivor + new region remainder
    my_block_header* survivor = my_free_list_best_fit(1024U); // the 1024-byte survivor
    MYMALLOC_CHECK(survivor == pair_a);      // survivor keeps the lowest address
    MYMALLOC_CHECK(survivor->size == 1024U); // 512 + 512 fused on free
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free(resized);
    my_free_list_clear();
}

// --- allocation failure: NULL back, original block untouched ---------------
void test_failure_leaves_original_intact() {
    my_free_list_clear();

    void* pointer = my_malloc(64U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_block_header* header = my_user_to_block(pointer);
    const std::size_t old_size = header->size;
    fill_pattern(pointer, 64U);

    // Sizes that overflow realloc's align-then-add computation.
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    MYMALLOC_CHECK(my_realloc(pointer, max_size) == nullptr);
    MYMALLOC_CHECK(my_realloc(pointer, max_size - 7U) == nullptr);
    // A size that passes the arithmetic but cannot be mapped also fails.
    MYMALLOC_CHECK(my_realloc(pointer, max_size / 8U) == nullptr);

    // C contract: the original block is still valid, allocated, and intact.
    MYMALLOC_CHECK(my_block_valid(header) == 1);
    MYMALLOC_CHECK(header->free == 0);
    MYMALLOC_CHECK(header->size == old_size);
    MYMALLOC_CHECK(my_free_list_count() == 1); // remainder from my_malloc(64), not freed
    MYMALLOC_CHECK(check_pattern(pointer, 64U) == true);

    // And the allocator keeps serving this pointer afterwards.
    void* usable = my_realloc(pointer, 128U);
    MYMALLOC_CHECK(usable != nullptr);
    MYMALLOC_CHECK(check_pattern(usable, 64U) == true);
    my_free(usable);
    my_free_list_clear(); // drop the freed block before the handcrafted pair

    // With a free upper neighbor present, a failing request must not consume
    // it: the sufficiency check runs before any metadata mutation.
    void* pair = handcraft_pair(512U, 512U);
    MYMALLOC_CHECK(pair != nullptr);
    if (pair == nullptr) {
        return;
    }
    my_block_header* pair_a = my_user_to_block(pair);
    my_block_header* pair_b = pair_a->next;
    fill_pattern(pair, 480U);

    MYMALLOC_CHECK(my_realloc(pair, max_size) == nullptr);
    MYMALLOC_CHECK(pair_a->size == 512U); // untouched
    MYMALLOC_CHECK(pair_a->free == 0);
    MYMALLOC_CHECK(pair_b->free == 1);
    MYMALLOC_CHECK(pair_a->next == pair_b); // still neighbors
    MYMALLOC_CHECK(pair_b->prev == pair_a);
    MYMALLOC_CHECK(my_free_list_count() == 1); // B still listed
    MYMALLOC_CHECK(my_free_list_head() == pair_b);
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    MYMALLOC_CHECK(check_pattern(pair, 480U) == true);

    my_free_list_clear();
}

// --- rejected inputs: foreign pointers and already-freed blocks ------------
void test_rejected_pointers() {
    my_free_list_clear();

    // A misaligned foreign address is refused before it is even dereferenced.
    MYMALLOC_CHECK(my_realloc(reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234)), 16U) ==
                   nullptr);

    // Reallocating a FREED (dangling) pointer returns NULL instead of handing
    // free memory back as a live allocation, and leaves the list untouched.
    void* pointer = my_malloc(64U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    my_free(pointer);
    MYMALLOC_CHECK(my_free_list_count() == 1);
    MYMALLOC_CHECK(my_realloc(pointer, 128U) == nullptr);
    MYMALLOC_CHECK(my_free_list_count() == 1); // survivor still listed
    MYMALLOC_CHECK(my_free_list_valid() == 1);

    my_free_list_clear();
}

// --- round trips: growth mixed with shrinks and moves ----------------------
void test_growth_round_trip() {
    my_free_list_clear();

    void* pointer = my_malloc(32U);
    MYMALLOC_CHECK(pointer != nullptr);
    if (pointer == nullptr) {
        return;
    }
    fill_pattern(pointer, 32U); // the 32 oldest bytes must survive everything

    // The sequence alternates fits, shrinks (split + coalesce), absorbs and
    // moves; the pattern prefix must hold after every step.
    const std::size_t sizes[] = {100U, 500U, 2000U, 64U, 3000U, 48U, 5000U};
    for (std::size_t step = 0; step < sizeof(sizes) / sizeof(sizes[0]); ++step) {
        pointer = my_realloc(pointer, sizes[step]);
        MYMALLOC_CHECK(pointer != nullptr);
        if (pointer == nullptr) {
            return;
        }
        MYMALLOC_CHECK(my_block_payload_size(my_user_to_block(pointer)) >= sizes[step]);
        MYMALLOC_CHECK(check_pattern(pointer, 32U) == true);
        MYMALLOC_CHECK(my_free_list_valid() == 1);
    }

    my_free(pointer);
    my_free_list_clear();
}

} // namespace

int main() {
    test_nullptr_and_zero();
    test_grow_within_slack();
    test_shrink_in_place();
    test_absorb_upper_neighbor();
    test_move_when_necessary();
    test_failure_leaves_original_intact();
    test_rejected_pointers();
    test_growth_round_trip();
    teardown();
    MYMALLOC_TEST_MAIN("realloc");
}