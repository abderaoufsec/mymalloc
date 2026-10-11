// Phase 12 demo: region ownership — my_malloc tracks every raw region it
// acquires, multiple regions coexist, and fully-free regions are returned to
// the OS just before fresh memory is needed (scavenge-on-acquire). The last
// section exercises the release guardrails. See
// docs/phase12_heap_management.md.
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>

#include <cstdio>
#include <cstring>

namespace {

void print_state(const char* label) {
    std::printf("%-52s regions=%zu  free_blocks=%zu\n", label, my_region_count(),
                my_free_list_count());
}

} // namespace

int main() {
    print_state("start");

    // --- multiple regions ---------------------------------------------------
    void* a = my_malloc(64);
    void* b = my_malloc(64);
    void* c = my_malloc(64);
    std::printf("a=%p  b=%p  c=%p (each request acquired its own region)\n", a, b, c);
    print_state("after three my_malloc(64)");
    std::memset(a, 0xA1, 64);
    std::memset(b, 0xB2, 64);
    std::memset(c, 0xC3, 64);

    // --- free does NOT shrink the heap --------------------------------------
    my_free(a);
    my_free(b);
    my_free(c);
    print_state("after freeing all three [free never unmaps]");

    // --- scavenge-on-acquire ------------------------------------------------
    // 1 MiB cannot fit any 4 KiB free block, so my_malloc must acquire:
    // it first reclaims ALL fully-free regions, then maps one new one.
    void* big = my_malloc(1024 * 1024);
    print_state("after my_malloc(1 MiB) [scavenge reclaimed A+B+C]");
    my_free(big);
    print_state("after my_free(1 MiB) [still tracked]");

    // --- guardrails: invalid unmapping is refused ---------------------------
    std::printf("\nguardrails:\n");
    my_region found{};
    (void)my_region_find(big, &found); // freed, but still tracked
    std::printf("release with wrong size      -> %d (refused, nothing unmapped)\n",
                my_region_release(found.base, found.bytes / 2));
    int foreign = 0;
    std::printf("release of an untracked base -> %d (refused)\n",
                my_region_release(&foreign, sizeof(foreign)));

    // Force a fresh acquire: the fully-free 1 MiB region is reclaimed now.
    void* fresh = my_malloc(2 * 1024 * 1024);
    print_state("after fresh 2 MiB acquire [1 MiB region reclaimed]");
    // `fresh` is live, so its region is NOT fully free and cannot be released.
    (void)my_region_find(fresh, &found);
    std::printf("release while allocations are live -> %d (refused: not fully free)\n",
                my_region_release(found.base, found.bytes));

    // --- long cycles stay bounded -------------------------------------------
    my_free(fresh); // 2 MiB region stays listed: the loop below just reuses it
    for (int i = 0; i < 10000; ++i) {
        void* p = my_malloc(48);
        my_free(p);
    }
    print_state("after 10000 alloc/free cycles [reused, no growth]");

    std::printf("\n%zu region(s) still mapped at exit (normal: free never shrinks).\n",
                my_region_count());
    std::printf("\nregions: tracking, multi-region, guarded release, scavenge-on-acquire.\n");
    return 0;
}