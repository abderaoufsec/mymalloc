// mymalloc Phase 6 — free list / first-fit reuse demo.
//
// Demonstrates what changed since Phase 5: freed blocks now go on a
// free-list (address-ordered, links overlaid on the payload) and are reused
// by first-fit BEFORE any new OS region is acquired. Prints list state,
// shows same-size reuse, shows a too-small list entry being bypassed by a
// large request, and exercises the consistency audit.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

#include <cstdint>
#include <cstdio>

namespace {

void dump_list(const char* tag) {
    std::printf("%s: count=%zu valid=%d\n", tag, my_free_list_count(), my_free_list_valid());
    std::size_t i = 0;
    for (my_block_header* b = my_free_list_head(); b != nullptr;) {
        my_free_links* links = my_free_list_links(b);
        std::printf("  [%zu] block=%p size=%zu free=%u\n", i, static_cast<const void*>(b), b->size,
                    static_cast<unsigned>(b->free));
        b = links->next != nullptr ? my_user_to_block(links->next) : nullptr;
        ++i;
        if (i > 64) {
            std::printf("  ... (unexpected list length)\n");
            break;
        }
    }
}

} // namespace

int main() {
    std::printf("page size = %zu, default alignment = %zu\n", my_raw_page_size(),
                my_default_alignment());
    dump_list("initial");

    // 1. Allocate, free, allocate the same size -> the block is REUSED.
    void* a = my_malloc(64);
    void* b = my_malloc(64);
    std::printf("fresh:   a=%p b=%p (a != b: %d)\n", a, b, a != b);
    my_free(a);
    dump_list("after free(a)");
    void* a2 = my_malloc(64);
    std::printf("reused:  a2=%p == a? %d (no new region needed)\n", a2, a2 == a);
    dump_list("after re-alloc");

    // 2. A request larger than every listed block bypasses the list.
    my_free(b);
    dump_list("after free(b)");
    void* huge = my_malloc(2 * 1024 * 1024); // 2 MiB > both listed blocks
    std::printf("huge=%p (fresh region; list untouched)\n", huge);
    dump_list("after huge alloc");

    // 3. Interleaved frees: list stays address-ordered.
    void* c = my_malloc(128);
    void* d = my_malloc(32);
    void* e = my_malloc(4096);
    my_free(e);
    my_free(c);
    my_free(d);
    dump_list("three interleaved frees (address-ordered)");
    my_free(d); // double free: guarded no-op, count unchanged

    // 4. Reuse across mixed sizes keeps the audit green.
    void* f = my_malloc(1000);
    void* g = my_malloc(32);
    std::printf("mixed reuse: f=%p g=%p\n", f, g);
    my_free(f);
    my_free(g);
    my_free(b);
    dump_list("final");

    my_free_list_clear();
    dump_list("after clear (blocks untouched, just unlisted)");
    std::printf("done\n");
    return 0;
}