// mymalloc Phase 4 — block metadata experiment.
//
// Carves three adjacent blocks into one raw region (allocated / free /
// allocated), prints every header field and the header <-> user-pointer
// conversions, runs the validator at each step, then deliberately corrupts
// a header to show detection and restores it.
//
// See docs/phase4_block_metadata.md for the design.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/raw_memory.h>

namespace {

void print_block(const char* label, my_block_header* block) {
    std::printf("%-6s block=%p size=%zu free=%d user=%p payload=%zu valid=%s\n", label,
                static_cast<void*>(block), block->size, my_block_is_free(block),
                my_block_to_user(block), my_block_payload_size(block),
                my_block_valid(block) ? "yes" : "NO");
}

} // namespace

int main() {
    const std::size_t block_size = 1024U;
    const std::size_t region_bytes = my_raw_page_size() * 2U;
    void* region = my_raw_acquire(region_bytes);
    if (region == nullptr) {
        std::printf("raw acquire FAILED\n");
        return 1;
    }

    std::printf("=== mymalloc Phase 4: block metadata experiment ===\n");
    std::printf("default alignment: %zu   header: %zu bytes   user offset: %zu bytes\n\n",
                my_default_alignment(), sizeof(my_block_header), my_block_user_offset());

    auto* base = static_cast<unsigned char*>(region);
    my_block_header* b1 = reinterpret_cast<my_block_header*>(base);
    my_block_header* b2 = reinterpret_cast<my_block_header*>(base + block_size);
    my_block_header* b3 = reinterpret_cast<my_block_header*>(base + 2U * block_size);
    my_block_init(b1, block_size, 0, nullptr, b2);
    my_block_init(b2, block_size, 1, b1, b3);
    my_block_init(b3, block_size, 0, b2, nullptr);

    std::printf("--- block chain (adjacent in one region) ---\n");
    print_block("b1", b1);
    print_block("b2", b2);
    print_block("b3", b3);

    std::printf("\n--- conversions ---\n");
    void* user2 = my_block_to_user(b2);
    std::printf("b2 user pointer            %p  (aligned: %s)\n", user2,
                reinterpret_cast<uintptr_t>(user2) % my_default_alignment() == 0U ? "yes" : "NO");
    std::printf("round trip user_to_block   %s\n", my_user_to_block(user2) == b2 ? "ok" : "BROKEN");

    std::printf("\n--- payload write leaves metadata intact ---\n");
    std::memset(user2, 0x5A, my_block_payload_size(b2));
    std::printf("b1 valid=%s  b2 valid=%s  b3 valid=%s\n", my_block_valid(b1) ? "yes" : "NO",
                my_block_valid(b2) ? "yes" : "NO", my_block_valid(b3) ? "yes" : "NO");

    std::printf("\n--- corruption detection ---\n");
    const std::size_t saved = b2->size;
    b2->size += 8U;
    std::printf("corrupt b2.size (+8)      -> b2 valid=%s (detected: %s)\n",
                my_block_valid(b2) ? "yes" : "NO", my_block_valid(b2) ? "no" : "yes");
    b2->size = saved;
    std::printf("restored                  -> b2 valid=%s\n", my_block_valid(b2) ? "yes" : "NO");

    std::printf("\n--- neighbors ---\n");
    std::printf("b2->prev == b1: %s   b2->next == b3: %s\n", my_block_prev(b2) == b1 ? "yes" : "NO",
                my_block_next(b2) == b3 ? "yes" : "NO");

    my_raw_release(region, region_bytes);
    std::printf("\nregion released.\n");
    return 0;
}