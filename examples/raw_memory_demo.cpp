// mymalloc Phase 2 — raw memory demo.
//
// Acquires a few page-granular regions through the OS layer, prints their
// addresses and page alignment, writes into every page, then releases them.
//
// See docs/phase2_raw_memory.md for the strategy decision (mmap/munmap on
// POSIX, VirtualAlloc/VirtualFree on Windows).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mymalloc/raw_memory.h>

namespace {

void print_region(const char* label, void* region, std::size_t bytes) {
    const std::size_t page = my_raw_page_size();
    std::printf("%-12s %p  size=%zu  page-aligned=%s\n", label, region, bytes,
                reinterpret_cast<std::uintptr_t>(region) % page == 0U ? "yes" : "NO");
}

} // namespace

int main() {
    const std::size_t page = my_raw_page_size();
    std::printf("=== mymalloc Phase 2: raw memory experiment ===\n");
    std::printf("page size: %zu bytes\n\n", page);

    void* one_page = my_raw_acquire(page);
    void* eight_pages = my_raw_acquire(page * 8U);
    void* tiny = my_raw_acquire(100U); // rounds up to one page

    print_region("acquire(1p)", one_page, page);
    print_region("acquire(8p)", eight_pages, page * 8U);
    print_region("acquire(100)", tiny, 100U);

    std::printf("\n--- write probe (first + last byte of each page) ---\n");
    void* blocks[3] = {one_page, eight_pages, tiny};
    std::size_t sizes[3] = {page, page * 8U, page};
    for (std::size_t b = 0; b < 3; ++b) {
        if (blocks[b] == nullptr) {
            std::printf("block %zu: FAILED to acquire\n", b);
            continue;
        }
        unsigned char* bytes = static_cast<unsigned char*>(blocks[b]);
        for (std::size_t offset = 0; offset < sizes[b]; offset += page) {
            bytes[offset] = 0xDE;
        }
        std::printf("block %zu: touched %zu page(s), readback ok: %s\n", b, sizes[b] / page,
                    bytes[sizes[b] - page] == 0xDE ? "yes" : "NO");
    }

    std::printf("\n--- failure demonstration ---\n");
    std::printf("acquire(0)              -> %s\n", my_raw_acquire(0) == nullptr ? "NULL" : "?");
    std::printf("acquire(SIZE_MAX)       -> %s\n",
                my_raw_acquire(~static_cast<std::size_t>(0)) == nullptr ? "NULL" : "?");
    std::printf("acquire(SIZE_MAX / 2)   -> %s\n",
                my_raw_acquire(~static_cast<std::size_t>(0) / 2U) == nullptr ? "NULL" : "?");

    my_raw_release(one_page, page);
    my_raw_release(eight_pages, page * 8U);
    my_raw_release(tiny, 100U);
    my_raw_release(nullptr, page); // documented no-op

    std::printf("\nall regions released.\n");
    return 0;
}