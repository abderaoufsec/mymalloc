// Phase 10 test: verify my_calloc (<mymalloc/mymalloc.h>) against the
// contract documented in docs/phase10_calloc.md: zero-initialized memory,
// multiplication-overflow rejection, zero-count/zero-size failure, and —
// crucially — that a REUSED block is still zeroed (not left stale).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>

#include "test_framework.hpp"

namespace {

// True when every byte in [ptr, ptr+size) is zero.
bool all_zero(const void* ptr, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const std::size_t alignment = my_default_alignment();

    // --- zero count or zero size fails (documented decision) --------------
    MYMALLOC_CHECK(my_calloc(0, 16) == nullptr);
    MYMALLOC_CHECK(my_calloc(16, 0) == nullptr);
    MYMALLOC_CHECK(my_calloc(0, 0) == nullptr);

    // --- basic zero-initialization ----------------------------------------
    void* pointer = my_calloc(10, 16);
    MYMALLOC_CHECK(pointer != nullptr);
    MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(pointer) % alignment == 0U);
    MYMALLOC_CHECK(all_zero(pointer, 10U * 16U) == true);
    my_block_header* header = my_user_to_block(pointer);
    MYMALLOC_CHECK(my_block_valid(header) == 1);
    MYMALLOC_CHECK(my_block_is_free(header) == 0);
    MYMALLOC_CHECK(my_block_payload_size(header) >= 10U * 16U);
    // Writable after zeroing.
    std::memset(pointer, 0x5A, 10U * 16U);
    my_free(pointer);

    // --- odd shapes: 1x1, many-by-1, 7 x 1000 ----------------------------
    void* one = my_calloc(1, 1);
    MYMALLOC_CHECK(one != nullptr);
    MYMALLOC_CHECK(all_zero(one, 1) == true);
    my_free(one);

    void* many = my_calloc(4096, 1);
    MYMALLOC_CHECK(many != nullptr);
    MYMALLOC_CHECK(all_zero(many, 4096) == true);
    my_free(many);

    void* big_elem = my_calloc(7, 1000); // 7000 bytes, not page-aligned
    MYMALLOC_CHECK(big_elem != nullptr);
    MYMALLOC_CHECK(all_zero(big_elem, 7000) == true);
    my_free(big_elem);

    // --- the key property: a REUSED block is still zeroed -----------------
    // malloc, dirty it, free it, then calloc the same size — the reuse path
    // (Phases 6-7) hands back stale bytes that calloc must clear.
    {
        const std::size_t bytes = 512U;
        void* dirty = my_malloc(bytes);
        MYMALLOC_CHECK(dirty != nullptr);
        std::memset(dirty, 0xFF, bytes); // fill with non-zero garbage
        my_free(dirty);

        void* clean = my_calloc(1, bytes); // likely reuses the same block
        MYMALLOC_CHECK(clean != nullptr);
        MYMALLOC_CHECK(all_zero(clean, bytes) == true);
        my_free(clean);
    }

    // --- multiplication overflow rejection --------------------------------
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    MYMALLOC_CHECK(my_calloc(max_size, 2) == nullptr); // product overflows
    MYMALLOC_CHECK(my_calloc(2, max_size) == nullptr); // symmetric
    MYMALLOC_CHECK(my_calloc(max_size, max_size) == nullptr);
    MYMALLOC_CHECK(my_calloc(max_size / 2U, 4U) == nullptr); // wraps by one bit
    // A product that fits size_t but exceeds what the OS will map also fails.
    MYMALLOC_CHECK(my_calloc(max_size / 8U, 1U) == nullptr);

    // --- the allocator keeps working after overflow failures --------------
    void* after = my_calloc(64, 8);
    MYMALLOC_CHECK(after != nullptr);
    MYMALLOC_CHECK(all_zero(after, 512) == true);
    my_free(after);

    // --- stress-ish loop: repeated calloc/free cycles ---------------------
    for (int cycle = 0; cycle < 100; ++cycle) {
        const std::size_t count = static_cast<std::size_t>(cycle % 20) + 1U;
        const std::size_t size = 8U;
        void* loop_ptr = my_calloc(count, size);
        if (loop_ptr == nullptr) {
            MYMALLOC_CHECK(loop_ptr != nullptr);
            break;
        }
        MYMALLOC_CHECK(all_zero(loop_ptr, count * size) == true);
        my_free(loop_ptr);
    }

    MYMALLOC_TEST_MAIN("calloc");
}
