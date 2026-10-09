// Phase 3 test: verify the alignment helpers (<mymalloc/alignment.h>)
// against the invariants documented in docs/phase3_alignment.md:
// power-of-two detection, a sane default alignment, exact and rounded-up
// results at boundaries, overflow failures that leave the out-parameter
// untouched, a many-sizes/many-alignments sweep, and integration with the
// Phase 2 raw layer (aligning an interior pointer of a real region).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <mymalloc/alignment.h>
#include <mymalloc/raw_memory.h>

#include "test_framework.hpp"

namespace {

void check_align_up_size_case(std::size_t value, std::size_t alignment) {
    std::size_t result = 0xDEAD;
    if (my_align_up_size(value, alignment, &result) != 1) {
        MYMALLOC_CHECK(false);
        return;
    }
    MYMALLOC_CHECK(result >= value);
    MYMALLOC_CHECK(result % alignment == 0U);
    MYMALLOC_CHECK(result - value < alignment);
}

void check_align_up_address_case(uintptr_t address, std::size_t alignment) {
    uintptr_t result = 0xDEAD;
    if (my_align_up_address(address, alignment, &result) != 1) {
        MYMALLOC_CHECK(false);
        return;
    }
    MYMALLOC_CHECK(result >= address);
    MYMALLOC_CHECK(result % alignment == 0U);
    MYMALLOC_CHECK(result - address < alignment);
}

} // namespace

int main() {
    // --- power-of-two detection -------------------------------------------
    MYMALLOC_CHECK(my_is_power_of_two(1) == 1);
    MYMALLOC_CHECK(my_is_power_of_two(2) == 1);
    MYMALLOC_CHECK(my_is_power_of_two(4) == 1);
    MYMALLOC_CHECK(my_is_power_of_two(4096) == 1);
    MYMALLOC_CHECK(my_is_power_of_two(std::numeric_limits<std::size_t>::max() / 2U + 1U) == 1);
    MYMALLOC_CHECK(my_is_power_of_two(0) == 0);
    MYMALLOC_CHECK(my_is_power_of_two(3) == 0);
    MYMALLOC_CHECK(my_is_power_of_two(5) == 0);
    MYMALLOC_CHECK(my_is_power_of_two(4095) == 0);
    MYMALLOC_CHECK(my_is_power_of_two(std::numeric_limits<std::size_t>::max()) == 0);

    // --- default alignment sanity -----------------------------------------
    const std::size_t base = my_default_alignment();
    MYMALLOC_CHECK(my_is_power_of_two(base) == 1);
    MYMALLOC_CHECK(base >= 8U);
    MYMALLOC_CHECK(base <= 64U);

    // --- exact multiples stay unchanged; others round up ------------------
    check_align_up_size_case(0, 8);
    check_align_up_size_case(8, 8);  // exact
    check_align_up_size_case(16, 8); // exact, larger
    check_align_up_size_case(1, 8);  // rounds to 8
    check_align_up_size_case(7, 8);  // just below
    check_align_up_size_case(9, 8);  // just above
    check_align_up_size_case(4095, 4096);
    check_align_up_size_case(4096, 4096);  // exact page
    check_align_up_size_case(4097, 4096);  // one past page
    check_align_up_size_case(12345, base); // default alignment

    // --- address rounding --------------------------------------------------
    check_align_up_address_case(0, 16);
    check_align_up_address_case(0x1000, 0x1000); // exact page
    check_align_up_address_case(0x1001, 0x1000); // one past -> next page
    check_align_up_address_case(0x100F, 16);     // just below 16
    check_align_up_address_case(0x1010, 16);     // exact 16

    // --- overflow: last valid values succeed, one more fails --------------
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    std::size_t out = 0xDEAD;
    MYMALLOC_CHECK(my_align_up_size(max_size - 7U, 8, &out) == 1);
    MYMALLOC_CHECK(out == max_size - 7U);
    out = 0xDEAD;
    MYMALLOC_CHECK(my_align_up_size(max_size - 6U, 8, &out) == 0);
    MYMALLOC_CHECK(out == 0xDEAD); // untouched on failure
    MYMALLOC_CHECK(my_align_up_size(max_size, 4096, &out) == 0);
    MYMALLOC_CHECK(out == 0xDEAD);

    const uintptr_t max_addr = static_cast<uintptr_t>(-1);
    uintptr_t addr_out = 0xDEAD;
    MYMALLOC_CHECK(my_align_up_address(max_addr - 7U, 8, &addr_out) == 1);
    MYMALLOC_CHECK(addr_out == max_addr - 7U);
    addr_out = 0xDEAD;
    MYMALLOC_CHECK(my_align_up_address(max_addr - 6U, 8, &addr_out) == 0);
    MYMALLOC_CHECK(addr_out == 0xDEAD); // untouched on failure

    // --- invalid alignments and NULL out never write ----------------------
    out = 0xDEAD;
    MYMALLOC_CHECK(my_align_up_size(16, 0, &out) == 0);
    MYMALLOC_CHECK(my_align_up_size(16, 3, &out) == 0);
    MYMALLOC_CHECK(my_align_up_size(16, 1023, &out) == 0);
    MYMALLOC_CHECK(out == 0xDEAD);
    MYMALLOC_CHECK(my_align_up_size(16, 8, nullptr) == 0);
    MYMALLOC_CHECK(my_align_up_address(16, 8, nullptr) == 0);

    // --- sweep: many sizes x many alignments ------------------------------
    const std::size_t alignments[] = {1U, 2U, 4U, 8U, 16U, 32U, 4096U};
    for (std::size_t alignment : alignments) {
        for (std::size_t value = 0; value < 300U; ++value) {
            check_align_up_size_case(value, alignment);
        }
        // Boundaries around multiples of the alignment.
        check_align_up_size_case(alignment - 1U, alignment);
        check_align_up_size_case(alignment, alignment);
        check_align_up_size_case(alignment + 1U, alignment);
        check_align_up_size_case(1024U * alignment - 1U, alignment);
    }

    // --- integration: align an interior pointer of a raw region -----------
    const std::size_t page = my_raw_page_size();
    void* block = my_raw_acquire(page * 2U);
    MYMALLOC_CHECK(block != nullptr);
    const uintptr_t raw = reinterpret_cast<uintptr_t>(block);
    uintptr_t aligned = 0;
    MYMALLOC_CHECK(my_align_up_address(raw + 1U, base, &aligned) == 1);
    MYMALLOC_CHECK(aligned % base == 0U);
    MYMALLOC_CHECK(aligned >= raw);
    MYMALLOC_CHECK(aligned < raw + page * 2U); // still inside the region
    // The aligned interior pointer must be usable memory.
    std::memset(reinterpret_cast<void*>(aligned), 0x77, base);
    MYMALLOC_CHECK(*reinterpret_cast<unsigned char*>(aligned) == 0x77);
    my_raw_release(block, page * 2U);

    MYMALLOC_TEST_MAIN("alignment");
}