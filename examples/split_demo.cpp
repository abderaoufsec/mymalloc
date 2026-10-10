// mymalloc Phase 7 — splitting oversized free blocks experiment.
//
// Shows what my_malloc gains from splitting (my_block_split): reusing an
// oversized free block now returns just the requested front and keeps a
// worthwhile remainder listed for reuse, instead of absorbing all the slack.
//
// See docs/phase7_splitting.md for the split policy and its boundary rules.
#include <cstddef>
#include <cstdio>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>

namespace {

void print_block(const char* label, my_block_header* block) {
    if (block == nullptr) {
        std::printf("%-10s (null)\n", label);
        return;
    }
    std::printf("%-10s block=%p size=%-5zu free=%d user=%p payload=%-5zu valid=%s\n", label,
                static_cast<void*>(block), block->size, my_block_is_free(block),
                my_block_to_user(block), my_block_payload_size(block),
                my_block_valid(block) == 1 ? "yes" : "NO");
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 7: splitting oversized free blocks ===\n");
    std::printf("default alignment: %zu   header offset: %zu bytes\n\n", my_default_alignment(),
                my_block_user_offset());

    // --- create one oversized free block ------------------------------------
    // A whole page region (4096 bytes) is a block whose payload is 4064 bytes.
    std::printf("--- create an oversized free block ---\n");
    void* big = my_malloc(4096U - my_block_user_offset());
    print_block("oversized", my_user_to_block(big));
    my_free(big);
    std::printf("(freed; the whole 4096-byte block is now listed for reuse)\n\n");

    // --- reuse it for a small request: the block is SPLIT -------------------
    std::printf("--- small request splits the oversized block ---\n");
    void* front = my_malloc(16U);
    my_block_header* fb = my_user_to_block(front);
    print_block("front(16)", fb);
    my_block_header* remainder = fb != nullptr ? fb->next : nullptr;
    print_block("remainder", remainder);
    std::printf("front+remainder cover the region: %s\n",
                fb != nullptr && remainder != nullptr && fb->size + remainder->size == 4096U
                    ? "yes"
                    : "NO");
    std::printf("list has %zu free block(s), valid=%s\n\n", my_free_list_count(),
                my_free_list_valid() == 1 ? "yes" : "NO");

    // --- the remainder is reusable without new memory -----------------------
    std::printf("--- the remainder is reused for the next request ---\n");
    void* next = my_malloc(64U);
    print_block("reused(64)", my_user_to_block(next));
    std::printf("list has %zu free block(s), valid=%s\n\n", my_free_list_count(),
                my_free_list_valid() == 1 ? "yes" : "NO");

    std::printf("--- a remainder too small to be a block is kept as slack ---\n");
    my_free(front);
    my_free(next);
    void* big2 = my_malloc(8192U - my_block_user_offset());
    my_free(big2);
    const std::size_t min_remainder = my_block_user_offset() + sizeof(my_free_links);
    void* almost = my_malloc(8192U - min_remainder + 8U); // leftover < threshold
    print_block("whole-reuse", my_user_to_block(almost));
    std::printf("no remainder block created (block->next == NULL): %s\n",
                my_user_to_block(almost)->next == nullptr ? "yes" : "NO");
    my_free(almost);

    return 0;
}