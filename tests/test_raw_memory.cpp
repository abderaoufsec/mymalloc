// Phase 2 test: verify the raw OS memory layer (<mymalloc/raw_memory.h>)
// against the invariants documented in docs/phase2_raw_memory.md:
// page size sanity, page alignment, writability of the whole region,
// zero-initialization, graceful failure (zero / overflow / OS refusal),
// non-overlap of live regions, and repeated acquire/release cycles.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <mymalloc/raw_memory.h>

#include "test_framework.hpp"

namespace {

std::uintptr_t address_of(void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

} // namespace

int main() {
    // --- page size sanity --------------------------------------------------
    const std::size_t page = my_raw_page_size();
    MYMALLOC_CHECK(page >= 256U);
    MYMALLOC_CHECK(page <= 16U * 1024U * 1024U);
    MYMALLOC_CHECK((page & (page - 1U)) == 0U);

    // --- single page: success, alignment, zero-fill, writability ----------
    void* block = my_raw_acquire(page);
    MYMALLOC_CHECK(block != nullptr);
    MYMALLOC_CHECK(address_of(block) % page == 0U);
    const unsigned char* bytes = static_cast<unsigned char*>(block);
    MYMALLOC_CHECK(bytes[0] == 0);
    MYMALLOC_CHECK(bytes[page - 1] == 0);
    std::memset(block, 0xAB, page);
    MYMALLOC_CHECK(bytes[0] == 0xAB);
    MYMALLOC_CHECK(bytes[page - 1] == 0xAB);
    my_raw_release(block, page);

    // --- multi-page region: whole range writable --------------------------
    block = my_raw_acquire(page * 8U);
    MYMALLOC_CHECK(block != nullptr);
    std::memset(block, 0x5A, page * 8U);
    const unsigned char* multi = static_cast<unsigned char*>(block);
    MYMALLOC_CHECK(multi[page * 8U - 1U] == 0x5A);
    my_raw_release(block, page * 8U);

    // --- small request rounds up to a whole page --------------------------
    block = my_raw_acquire(1U);
    MYMALLOC_CHECK(block != nullptr);
    std::memset(block, 0x11, page); // the whole page must be writable
    my_raw_release(block, 1U);

    // --- fresh regions are zero-initialized -------------------------------
    block = my_raw_acquire(page);
    MYMALLOC_CHECK(block != nullptr);
    const unsigned char* fresh = static_cast<unsigned char*>(block);
    std::size_t nonzero = 0;
    for (std::size_t index = 0; index < page; ++index) {
        if (fresh[index] != 0) {
            ++nonzero;
        }
    }
    MYMALLOC_CHECK(nonzero == 0);
    my_raw_release(block, page);

    // --- failure: zero size, rounding overflow, absurd sizes --------------
    MYMALLOC_CHECK(my_raw_acquire(0) == nullptr);
    MYMALLOC_CHECK(my_raw_acquire(std::numeric_limits<std::size_t>::max()) == nullptr);
    MYMALLOC_CHECK(my_raw_acquire(std::numeric_limits<std::size_t>::max() - 100U) == nullptr);
    // Half the address space exceeds any user-space mapping limit (x86-64
    // user VA space is 128 TiB), so the OS must refuse this.
    MYMALLOC_CHECK(my_raw_acquire(std::numeric_limits<std::size_t>::max() / 2U) == nullptr);

    // --- release(nullptr, ...) is a documented no-op ----------------------
    my_raw_release(nullptr, 0);
    my_raw_release(nullptr, page);

    // --- live regions do not overlap --------------------------------------
    const std::size_t region_size = page * 2U;
    std::vector<void*> regions;
    for (int index = 0; index < 4; ++index) {
        void* region = my_raw_acquire(region_size);
        MYMALLOC_CHECK(region != nullptr);
        regions.push_back(region);
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
        for (std::size_t j = i + 1U; j < regions.size(); ++j) {
            const std::uintptr_t a = address_of(regions[i]);
            const std::uintptr_t b = address_of(regions[j]);
            const bool disjoint = a + region_size <= b || b + region_size <= a;
            MYMALLOC_CHECK(disjoint);
        }
    }
    for (void* region : regions) {
        my_raw_release(region, region_size);
    }

    // --- repeated acquire/release cycles ----------------------------------
    for (int cycle = 0; cycle < 200; ++cycle) {
        void* region = my_raw_acquire(page * 3U);
        if (region == nullptr) {
            MYMALLOC_CHECK(region != nullptr);
            break;
        }
        std::memset(region, static_cast<int>(cycle & 0xFF), page * 3U);
        const unsigned char* check = static_cast<unsigned char*>(region);
        if (check[0] != static_cast<unsigned char>(cycle & 0xFF) ||
            check[page * 3U - 1U] != static_cast<unsigned char>(cycle & 0xFF)) {
            MYMALLOC_CHECK(false);
            break;
        }
        my_raw_release(region, page * 3U);
    }

    MYMALLOC_TEST_MAIN("raw_memory");
}