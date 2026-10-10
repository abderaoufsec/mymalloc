// mymalloc — allocation strategy selection (Phase 9).
//
// A placement strategy decides WHICH free block my_malloc hands out for a
// request. This phase keeps the pure structure (the free list) unchanged and
// adds only the CHOICE of fit, plus a selectable global so the two can be
// compared and the default documented (see docs/phase9_allocation_strategies.md).
//
// Default: MY_STRATEGY_FIRST_FIT (unchanged since Phase 6). Best fit is
// optional and set explicitly; it is not the default because its whole-list
// scan costs O(n) per allocation, which is not worth it for the region-per-
// block model where free lists stay short.
//
// Not thread-safe (same policy as every phase so far): the selector is a
// single process-wide variable with no synchronization.
#ifndef MYMALLOC_STRATEGY_H
#define MYMALLOC_STRATEGY_H

#include <stddef.h>

// Placement strategies understood by my_malloc's reuse path.
//
// The underlying type is unsigned int (rather than the compiler-chosen int)
// so that static_cast from an arbitrary unsigned value is well-defined: the
// ignore-unknown-value path in my_set_strategy is tested with out-of-range
// casts, and with a fixed unsigned underlying type such a cast yields a
// valid (if unnamed) enumerator instead of undefined behavior.
enum my_strategy : unsigned int {
    MY_STRATEGY_FIRST_FIT = 0, // lowest-address block that fits (default)
    MY_STRATEGY_BEST_FIT = 1   // smallest block that fits (least leftover)
};

// Sets the global placement strategy. Unknown values are ignored (the current
// strategy is kept) so a bogus cast cannot silently disable reuse.
void my_set_strategy(enum my_strategy strategy);

// Returns the current global placement strategy.
enum my_strategy my_get_strategy(void);

#endif // MYMALLOC_STRATEGY_H