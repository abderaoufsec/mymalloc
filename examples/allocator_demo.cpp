// mymalloc Phase 5 — first allocator experiment.
//
// Uses the real target API (my_malloc / my_free) for the first time:
// allocates blocks of several sizes, prints their alignment and backing
// block metadata, writes patterns, frees them, and demonstrates the
// documented failure cases.
//
// See docs/phase5_allocator.md for the design and its Phase 6-8 follow-ups.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>

namespace {

void print_allocation(const char* label, std::size_t size) {
    void* pointer = my_malloc(size);
    if (pointer == nullptr) {
        std::printf("%-12s size=%-6zu -> NULL\n", label, size);
        return;
    }
    std::memset(pointer, 0xAB, size);
    my_block_header* block = my_user_to_block(pointer);
    std::printf("%-12s size=%-6zu -> %p  aligned=%s  block_valid=%s  payload=%zu\n", label, size,
                pointer,
                reinterpret_cast<uintptr_t>(pointer) % my_default_alignment() == 0U ? "yes" : "NO",
                my_block_valid(block) == 1 ? "yes" : "NO", my_block_payload_size(block));
    my_free(pointer);
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 5: first allocator experiment ===\n");
    std::printf("default alignment: %zu   header offset: %zu\n\n", my_default_alignment(),
                my_block_user_offset());

    std::printf("--- allocations (each one gets its own page-rounded region) ---\n");
    print_allocation("tiny", 1);
    print_allocation("small", 16);
    print_allocation("medium", 1000);
    print_allocation("page-ish", 4096);
    print_allocation("large", 50000);

    std::printf("\n--- simultaneous allocations stay isolated ---\n");
    void* a = my_malloc(64);
    void* b = my_malloc(64);
    std::printf("a=%p  b=%p  distinct=%s\n", a, b, a != b ? "yes" : "NO");
    std::memset(a, 0x11, 64);
    std::memset(b, 0x22, 64);
    std::printf("a[0]=0x%02X (expect 0x11)  b[0]=0x%02X (expect 0x22)\n",
                static_cast<unsigned>(*static_cast<unsigned char*>(a)),
                static_cast<unsigned>(*static_cast<unsigned char*>(b)));
    my_free(a);
    my_free(b);

    std::printf("\n--- documented failure cases ---\n");
    const std::size_t max_size = ~static_cast<std::size_t>(0);
    std::printf("my_malloc(0)           -> %s\n", my_malloc(0) == nullptr ? "NULL" : "?");
    std::printf("my_malloc(SIZE_MAX)    -> %s\n", my_malloc(max_size) == nullptr ? "NULL" : "?");
    std::printf("my_malloc(SIZE_MAX/2)  -> %s\n",
                my_malloc(max_size / 2U) == nullptr ? "NULL" : "?");

    std::printf("\nfree(NULL) is a no-op: ");
    my_free(nullptr);
    std::printf("ok\n");
    return 0;
}