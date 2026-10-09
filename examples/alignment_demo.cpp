// mymalloc Phase 3 — alignment experiment.
//
// Shows the default alignment my_malloc will guarantee, demonstrates the
// overflow-safe rounding helpers on representative and boundary values, and
// aligns an interior pointer inside a real Phase 2 raw region.
//
// See docs/phase3_alignment.md for the full study.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include <mymalloc/alignment.h>
#include <mymalloc/raw_memory.h>

namespace {

void print_round(const char* label, std::size_t value, std::size_t alignment) {
    std::size_t result = 0;
    if (my_align_up_size(value, alignment, &result) == 1) {
        std::printf("%-28s align_up(%6zu, %6zu) = %6zu  (+%zu)\n", label, value, alignment, result,
                    result - value);
    } else {
        std::printf("%-28s align_up(%zu, %zu)  = FAILED (overflow/invalid)\n", label, value,
                    alignment);
    }
}

} // namespace

int main() {
    const std::size_t base = my_default_alignment();
    std::printf("=== mymalloc Phase 3: alignment experiment ===\n");
    std::printf("default alignment (my_malloc guarantee): %zu bytes\n", base);
    std::printf("power of two: %s\n\n", my_is_power_of_two(base) ? "yes" : "NO");

    std::printf("--- rounding table ---\n");
    print_round("exact multiple", 64, 16);
    print_round("just below", 63, 16);
    print_round("just above", 65, 16);
    print_round("one byte", 1, 8);
    print_round("page boundary", 4096, 4096);
    print_round("one past page", 4097, 4096);
    print_round("zero", 0, 16);

    std::printf("\n--- overflow protection ---\n");
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    print_round("SIZE_MAX - 7", max_size - 7U, 8); // fits exactly
    print_round("SIZE_MAX - 6", max_size - 6U, 8); // must fail, not wrap to 0!
    print_round("SIZE_MAX", max_size, 4096);
    print_round("invalid align 3", 16, 3);

    std::printf("\n--- aligning inside a raw region ---\n");
    const std::size_t page = my_raw_page_size();
    void* block = my_raw_acquire(page);
    if (block == nullptr) {
        std::printf("raw acquire FAILED\n");
        return 1;
    }
    const uintptr_t raw = reinterpret_cast<uintptr_t>(block);
    uintptr_t interior = 0;
    if (my_align_up_address(raw + 1U, base, &interior) == 1) {
        std::printf("region      %p\n", block);
        std::printf("raw + 1     %p (unaligned)\n", reinterpret_cast<void*>(raw + 1U));
        std::printf("aligned up  %p (offset +%zu, %% %zu == %zu)\n",
                    reinterpret_cast<void*>(interior), interior - raw, base, interior % base);
    }
    my_raw_release(block, page);

    std::printf("\nall helpers behaved as documented.\n");
    return 0;
}