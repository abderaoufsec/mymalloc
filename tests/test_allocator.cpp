// Phase 5 test: verify the first allocator (<mymalloc/mymalloc.h>)
// against the contract documented in docs/phase5_allocator.md:
// aligned, writable, isolated allocations; free(nullptr) no-op; block
// validity of every returned pointer; zero-size and overflow failure;
// OS-refusal failure; and a batch of mixed-size allocations that are
// pairwise distinct and non-overlapping.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>

#include "test_framework.hpp"

namespace {

// Writes `pattern` across [ptr, ptr+size) and verifies it back.
bool write_and_verify(void* ptr, std::size_t size, unsigned char pattern) {
    std::memset(ptr, pattern, size);
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    return bytes[0] == pattern && bytes[size - 1U] == pattern;
}

} // namespace

int main() {
    const std::size_t alignment = my_default_alignment();

    // --- free(nullptr) is a no-op -----------------------------------------
    my_free(nullptr);

    // --- zero-sized requests fail (documented decision) -------------------
    MYMALLOC_CHECK(my_malloc(0) == nullptr);

    // --- single allocation: aligned, writable, valid block ----------------
    void* block_ptr = my_malloc(64);
    MYMALLOC_CHECK(block_ptr != nullptr);
    MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(block_ptr) % alignment == 0U);
    MYMALLOC_CHECK(write_and_verify(block_ptr, 64, 0xA5) == true);
    my_block_header* header = my_user_to_block(block_ptr);
    MYMALLOC_CHECK(my_block_valid(header) == 1);
    MYMALLOC_CHECK(my_block_is_free(header) == 0);
    MYMALLOC_CHECK(my_block_payload_size(header) >= 64U);
    my_free(block_ptr);
    MYMALLOC_CHECK(my_block_is_free(my_user_to_block(block_ptr)) == 1);
    // Freeing again only re-marks; Phase 14 adds double-free diagnostics.
    my_free(block_ptr);

    // --- odd sizes still come back aligned and fully usable ---------------
    for (std::size_t size : {1U, 15U, 17U, 100U, 4095U, 4096U, 4097U}) {
        void* pointer = my_malloc(size);
        MYMALLOC_CHECK(pointer != nullptr);
        MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(pointer) % alignment == 0U);
        MYMALLOC_CHECK(write_and_verify(pointer, size, 0x3C) == true);
        my_free(pointer);
    }

    // --- multiple allocations: distinct, non-overlapping, isolated --------
    const std::size_t sizes[] = {1U, 16U, 100U, 512U, 1000U, 4096U, 5000U, 8192U, 12345U};
    const std::size_t count = sizeof(sizes) / sizeof(sizes[0]);
    std::vector<void*> pointers;
    for (std::size_t index = 0; index < count; ++index) {
        void* pointer = my_malloc(sizes[index]);
        MYMALLOC_CHECK(pointer != nullptr);
        MYMALLOC_CHECK(reinterpret_cast<uintptr_t>(pointer) % alignment == 0U);
        pointers.push_back(pointer);
        // Distinct from every pointer handed out so far.
        for (std::size_t earlier = 0; earlier < pointers.size() - 1U; ++earlier) {
            MYMALLOC_CHECK(pointers[earlier] != pointer);
        }
    }
    // Isolation: every allocation holds its own pattern while the others
    // are written.
    for (std::size_t index = 0; index < count; ++index) {
        std::memset(pointers[index], static_cast<int>(0x10U + index), sizes[index]);
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto* bytes = static_cast<const unsigned char*>(pointers[index]);
        MYMALLOC_CHECK(bytes[0] == static_cast<unsigned char>(0x10U + index));
        MYMALLOC_CHECK(bytes[sizes[index] - 1U] == static_cast<unsigned char>(0x10U + index));
    }
    // Non-overlap of the requested ranges.
    std::vector<std::pair<uintptr_t, std::size_t>> ranges;
    for (std::size_t index = 0; index < count; ++index) {
        ranges.emplace_back(reinterpret_cast<uintptr_t>(pointers[index]), sizes[index]);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        MYMALLOC_CHECK(ranges[index - 1U].first + ranges[index - 1U].second <= ranges[index].first);
    }
    for (std::size_t index = 0; index < count; ++index) {
        my_free(pointers[index]);
    }

    // --- allocation failures ----------------------------------------------
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    MYMALLOC_CHECK(my_malloc(max_size) == nullptr);       // request rounding overflows
    MYMALLOC_CHECK(my_malloc(max_size - 31U) == nullptr); // payload + header overflows
    // Half the address space aligns fine but the OS must refuse the region.
    MYMALLOC_CHECK(my_malloc(max_size / 2U) == nullptr);
    MYMALLOC_CHECK(my_malloc(max_size / 4U) == nullptr);

    // --- the allocator keeps working after failures -----------------------
    void* after_failure = my_malloc(256);
    MYMALLOC_CHECK(after_failure != nullptr);
    MYMALLOC_CHECK(write_and_verify(after_failure, 256, 0x7E) == true);
    my_free(after_failure);

    // --- stress-ish loop: repeated alloc/free cycles ----------------------
    for (int cycle = 0; cycle < 100; ++cycle) {
        void* pointer = my_malloc(static_cast<std::size_t>(cycle % 50) + 1U);
        if (pointer == nullptr) {
            MYMALLOC_CHECK(pointer != nullptr);
            break;
        }
        std::memset(pointer, 0x99, static_cast<std::size_t>(cycle % 50) + 1U);
        my_free(pointer);
    }

    MYMALLOC_TEST_MAIN("allocator");
}