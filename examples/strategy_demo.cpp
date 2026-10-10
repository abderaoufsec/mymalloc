// mymalloc Phase 9 — allocation strategy demo (first fit vs best fit).
//
// Phase 6-8 gave the allocator a working reuse path (first fit). This phase
// adds a *choice* of placement strategy without touching the free-list
// structure: my_set_strategy / my_get_strategy switch a process-wide
// selector, and my_malloc dispatches reuse through it.
//
// The demo:
//   1. shows the default is FIRST_FIT,
//   2. frees several different-sized blocks and allocates a small request
//      under each strategy, printing which block each one picks,
//   3. shows best fit PRESERVES a large block for a later large request
//      where first fit would have consumed it.
#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/strategy.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

const char* strategy_name(enum my_strategy s) {
    return s == MY_STRATEGY_FIRST_FIT ? "FIRST_FIT" : "BEST_FIT";
}

// Allocate three blocks of clearly different region sizes, free them all
// (three independent free-list entries — separate mmaps never coalesce),
// then allocate a small request and report which block was taken.
void run_cycle(const char* label, enum my_strategy strategy) {
    my_free_list_clear();
    my_set_strategy(strategy);

    const std::size_t page = my_raw_page_size();
    void* big = my_malloc(page);  // -> 2-page region (8192)
    void* mid = my_malloc(64U);   // -> 1-page region (4096)
    void* small = my_malloc(64U); // -> 1-page region (4096)
    std::printf("[%s] fresh: big=%p (>=2p)  mid=%p  small=%p\n", label, big, mid, small);

    my_free(big);
    my_free(mid);
    my_free(small);
    std::printf("[%s] free list after 3 frees: count=%zu valid=%d\n", label, my_free_list_count(),
                my_free_list_valid());

    // A tiny request that fits EVERY listed block: the strategy decides which.
    void* picked = my_malloc(64U);
    std::printf("[%s] malloc(64) -> %p", label, picked);
    if (picked == mid) {
        std::printf("  (took the 1-page 'mid' block)\n");
    } else if (picked == small) {
        std::printf("  (took the 1-page 'small' block)\n");
    } else if (picked == big) {
        std::printf("  (took the big block)\n");
    } else {
        std::printf("  (fresh region)\n");
    }

    // Did a >= 2-page block survive? Best fit should keep it; first fit may not.
    my_block_header* survivor = my_free_list_best_fit(2U * page);
    std::printf("[%s] a >=2-page free block survives? %s\n", label,
                survivor != nullptr ? "yes" : "no");

    // Drain the rest so each cycle starts clean.
    my_free(picked);
    if (survivor != nullptr) {
        void* drain = my_malloc(2U * page);
        my_free(drain);
    }
    my_free_list_clear();
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 9: allocation strategies ===\n");
    std::printf("default alignment=%zu  page=%zu\n\n", my_default_alignment(), my_raw_page_size());

    std::printf("default strategy at startup: %s\n", strategy_name(my_get_strategy()));

    // Selecting an unknown value is ignored (reuse never silently disables).
    my_set_strategy(static_cast<enum my_strategy>(99));
    std::printf("after set(99): still %s (unknown ignored)\n\n", strategy_name(my_get_strategy()));

    run_cycle("first-fit", MY_STRATEGY_FIRST_FIT);
    std::printf("\n");
    run_cycle("best-fit", MY_STRATEGY_BEST_FIT);

    // Restore the documented default.
    my_set_strategy(MY_STRATEGY_FIRST_FIT);
    std::printf("\nrestored default: %s\n", strategy_name(my_get_strategy()));
    return 0;
}
