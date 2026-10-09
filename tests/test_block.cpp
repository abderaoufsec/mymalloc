// Phase 4 test: verify the block metadata layer (<mymalloc/block.h>)
// against the invariants documented in docs/phase4_block_metadata.md:
// header/user-pointer conversions, initialization rules, payload
// non-interference, neighbor tracking, and corruption detection.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/raw_memory.h>

#include "test_framework.hpp"

namespace {

// Carves `count` adjacent, equally sized blocks into one raw region and
// links them bidirectionally. Region size must be pages (guaranteeing the
// base is alignment-aligned).
struct BlockChain {
    void* region = nullptr;
    std::size_t region_bytes = 0;
    my_block_header* first = nullptr;
    my_block_header* blocks[4] = {};
    std::size_t block_size = 0;
};

bool build_chain(BlockChain* chain, std::size_t block_size, int count) {
    chain->region_bytes = my_raw_page_size() * 2U;
    chain->region = my_raw_acquire(chain->region_bytes);
    chain->block_size = block_size;
    if (chain->region == nullptr) {
        return false;
    }
    auto* base = static_cast<unsigned char*>(chain->region);
    for (int index = 0; index < count; ++index) {
        chain->blocks[index] =
            reinterpret_cast<my_block_header*>(base + block_size * static_cast<std::size_t>(index));
    }
    for (int index = 0; index < count; ++index) {
        my_block_header* prev = index > 0 ? chain->blocks[index - 1] : nullptr;
        my_block_header* next = index + 1 < count ? chain->blocks[index + 1] : nullptr;
        if (my_block_init(chain->blocks[index], block_size, index == 1 ? 1 : 0, prev, next) != 1) {
            return false;
        }
    }
    chain->first = chain->blocks[0];
    return true;
}

void destroy_chain(BlockChain* chain) {
    my_raw_release(chain->region, chain->region_bytes);
}

} // namespace

int main() {
    const std::size_t base_alignment = my_default_alignment();

    // --- user offset sanity -----------------------------------------------
    const std::size_t offset = my_block_user_offset();
    MYMALLOC_CHECK(offset >= sizeof(my_block_header));
    MYMALLOC_CHECK(offset % base_alignment == 0U);
    MYMALLOC_CHECK(offset <= 64U);

    // --- NULL conversions are safe ----------------------------------------
    MYMALLOC_CHECK(my_block_to_user(nullptr) == nullptr);
    MYMALLOC_CHECK(my_user_to_block(nullptr) == nullptr);

    // --- init failures -----------------------------------------------------
    BlockChain scratch{};
    void* region = my_raw_acquire(my_raw_page_size());
    MYMALLOC_CHECK(region != nullptr);
    auto* base = static_cast<unsigned char*>(region);
    MYMALLOC_CHECK(my_block_init(nullptr, 1024, 0, nullptr, nullptr) == 0);
    MYMALLOC_CHECK(
        my_block_init(reinterpret_cast<my_block_header*>(base), 8, 0, nullptr, nullptr) == 0);
    MYMALLOC_CHECK(
        my_block_init(reinterpret_cast<my_block_header*>(base), 1032, 0, nullptr, nullptr) == 0);
    MYMALLOC_CHECK(
        my_block_init(reinterpret_cast<my_block_header*>(base), 1024, 2, nullptr, nullptr) == 0);
    MYMALLOC_CHECK(my_block_init(reinterpret_cast<my_block_header*>(base + 1U), 1024, 0, nullptr,
                                 nullptr) == 0);
    // ...and nothing above should have produced a valid block.
    MYMALLOC_CHECK(my_block_valid(reinterpret_cast<my_block_header*>(base)) == 0);

    // --- a properly initialized block is valid; conversions round-trip ----
    const std::size_t block_size = 1024U;
    MYMALLOC_CHECK(my_block_init(reinterpret_cast<my_block_header*>(base), block_size, 0, nullptr,
                                 nullptr) == 1);
    auto* block = reinterpret_cast<my_block_header*>(base);
    MYMALLOC_CHECK(my_block_valid(block) == 1);
    void* user = my_block_to_user(block);
    MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(user) % base_alignment == 0U);
    MYMALLOC_CHECK(my_user_to_block(user) == block);
    MYMALLOC_CHECK(my_block_payload_size(block) == block_size - offset);
    MYMALLOC_CHECK(my_block_is_free(block) == 0);
    my_raw_release(region, my_raw_page_size());
    (void)scratch;

    // --- chain: neighbor tracking, adjacency, payload non-interference ----
    BlockChain chain{};
    MYMALLOC_CHECK(build_chain(&chain, block_size, 3) == true);
    MYMALLOC_CHECK(my_block_valid(chain.blocks[0]) == 1);
    MYMALLOC_CHECK(my_block_valid(chain.blocks[1]) == 1);
    MYMALLOC_CHECK(my_block_valid(chain.blocks[2]) == 1);

    MYMALLOC_CHECK(my_block_prev(chain.blocks[0]) == nullptr);
    MYMALLOC_CHECK(my_block_next(chain.blocks[0]) == chain.blocks[1]);
    MYMALLOC_CHECK(my_block_prev(chain.blocks[1]) == chain.blocks[0]);
    MYMALLOC_CHECK(my_block_next(chain.blocks[1]) == chain.blocks[2]);
    MYMALLOC_CHECK(my_block_next(chain.blocks[2]) == nullptr);
    MYMALLOC_CHECK(my_block_is_free(chain.blocks[1]) == 1);
    MYMALLOC_CHECK(my_block_is_free(chain.blocks[0]) == 0);

    // Physical adjacency: each block ends exactly where the next begins.
    for (int index = 0; index < 2; ++index) {
        const auto end =
            reinterpret_cast<uintptr_t>(chain.blocks[index]) + chain.blocks[index]->size;
        MYMALLOC_CHECK(end == reinterpret_cast<uintptr_t>(chain.blocks[index + 1]));
    }

    // Writing a full payload must not disturb headers or neighbors.
    void* payload = my_block_to_user(chain.blocks[1]);
    std::memset(payload, 0x42, my_block_payload_size(chain.blocks[1]));
    MYMALLOC_CHECK(my_block_valid(chain.blocks[0]) == 1);
    MYMALLOC_CHECK(my_block_valid(chain.blocks[1]) == 1);
    MYMALLOC_CHECK(my_block_valid(chain.blocks[2]) == 1);
    MYMALLOC_CHECK(chain.blocks[1]->size == block_size);
    MYMALLOC_CHECK(*static_cast<unsigned char*>(payload) == 0x42);

    // --- corruption detection ---------------------------------------------
    my_block_header* const b2 = chain.blocks[1];
    my_block_header* const b3 = chain.blocks[2];

    b2->size += 8U; // no longer an alignment multiple
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b2->size -= 8U;
    MYMALLOC_CHECK(my_block_valid(b2) == 1);

    b2->size = 8U; // smaller than the header
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b2->size = block_size;
    MYMALLOC_CHECK(my_block_valid(b2) == 1);

    b2->free = 5; // illegal state
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b2->free = 1;
    MYMALLOC_CHECK(my_block_valid(b2) == 1);

    b2->next = chain.blocks[0]; // wrong next (not at block+size)
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b2->next = b3;
    MYMALLOC_CHECK(my_block_valid(b2) == 1);

    b2->prev = b3; // prev at a higher address
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b2->prev = chain.blocks[0];
    MYMALLOC_CHECK(my_block_valid(b2) == 1);

    b3->prev = nullptr; // reciprocity broken from the other side
    MYMALLOC_CHECK(my_block_valid(b2) == 0);
    b3->prev = b2;
    MYMALLOC_CHECK(my_block_valid(b2) == 1);
    MYMALLOC_CHECK(my_block_valid(b3) == 1);

    // Misaligned block pointer: rejected without dereferencing (no crash).
    MYMALLOC_CHECK(my_block_valid(reinterpret_cast<my_block_header*>(base + 1U)) == 0);
    MYMALLOC_CHECK(my_block_valid(nullptr) == 0);

    destroy_chain(&chain);
    MYMALLOC_TEST_MAIN("block");
}