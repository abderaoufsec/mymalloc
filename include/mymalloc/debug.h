// mymalloc — debugging, pointer validation, and diagnostics (Phase 14).
//
// Everything here is about SEEING what the allocator is doing and catching the
// classic user mistakes — freeing a pointer we never handed out, freeing the
// same pointer twice, a payload that overran its allocation — rather than
// changing how the allocator places memory. Phases 5-13 already REFUSE an
// invalid or double free silently; this phase makes those refusals observable
// and adds opt-in overflow canaries.
//
// Three independent surfaces:
//
//   1. Consistency + ownership (ALWAYS available, no setup):
//        my_check_heap()  walks every tracked region and the free list and
//                         reports whether the whole heap is structurally
//                         consistent; on failure it names the first problem.
//        my_owns(ptr)     reports whether `ptr` is a LIVE allocation this
//                         allocator handed out — the "is this really mine?"
//                         question behind every free/query API.
//
//   2. A counters snapshot (peaks + events):
//        my_diagnostics() returns a by-value my_diag with peak live
//                         bytes/blocks, peak mapped bytes/regions, the current
//                         live/mapped mirror, and the counts of invalid frees,
//                         double frees, detected corruptions, failed canaries
//                         and rejected foreign pointers. The PEAK and CURRENT
//                         usage fields are maintained ALWAYS (a few adds and
//                         compares per alloc/free, no walk); the EVENT counters
//                         and the verbose callback only advance while checks
//                         are enabled via my_enable_checks().
//
//   3. Opt-in safety checks (OFF by default):
//        my_enable_checks(1) turns on per-allocation CANARIES (a magic word
//                         stamped in space RESERVED just past each request, so
//                         my_canary_ok can catch a heap OVERFLOW) and makes the
//                         event counters and the verbose hook live. With checks
//                         off (the default) the allocator reserves nothing and
//                         stamps nothing, so release builds keep the exact
//                         Phase 5-13 behaviour, block sizes and speed.
//
// Diagnostics never allocate on a failing path and never abort: they report
// and let the caller decide. Not thread-safe (same policy as every phase).
#ifndef MYMALLOC_DEBUG_H
#define MYMALLOC_DEBUG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walks every tracked region and the free list verifying the structural
// invariants: blocks validate, tile their region exactly, the chain never
// escapes a region, and the free-list count agrees with the heap. Returns 1
// when the whole heap is consistent, 0 otherwise — writing a short
// human-readable summary into `detail` (bounded by `detail_size`, always
// NUL-terminated when detail_size > 0) naming the first problem found.
// `detail` may be NULL to check without reporting. O(blocks): a diagnostic.
int my_check_heap(char* detail, size_t detail_size);

// Reports whether `ptr` is a LIVE allocation THIS allocator handed out: it
// lies inside a tracked region, its block header validates, and the block is
// allocated (not free). Returns 1 when it does, 0 otherwise (NULL, foreign,
// interior, already freed, or a corrupt header). Never dereferences past a
// validated header. This is the ownership question behind my_free. While
// checks are enabled, a rejected foreign pointer also bumps the
// foreign_pointers counter.
int my_owns(const void* ptr);

// --- counters snapshot (peaks/current always maintained) -------------------

typedef struct my_diag {
    // Peak usage since the last my_diag_reset (maintained always):
    size_t peak_live_bytes;   // largest live_bytes seen
    size_t peak_live_blocks;  // largest live_blocks seen
    size_t peak_mapped_bytes; // largest mapped_bytes seen (before scavenge)
    size_t peak_regions;      // largest region count seen

    // Current "now" mirror of the heap (cheap; read from maintained state):
    size_t live_bytes;   // current allocated bytes (headers included)
    size_t live_blocks;  // current allocated blocks
    size_t mapped_bytes; // current tracked extents
    size_t regions;      // current tracked region count

    // Event counters — these only advance while checks are ENABLED:
    unsigned long invalid_frees;    // my_free of a foreign/invalid pointer
    unsigned long double_frees;     // my_free of an already-free pointer
    unsigned long corruptions;      // my_check_heap found the heap corrupt
    unsigned long bad_canaries;     // my_canary_ok (or a checked free) failed
    unsigned long foreign_pointers; // my_owns/my_canary_ok rejected a pointer
} my_diag;

// Fills *out with the current snapshot. Returns 1 on success, 0 when out is
// NULL. Cheap: reads maintained state, walks nothing.
int my_diagnostics(my_diag* out);

// Zeroes the event counters and re-baselines the PEAKS to the current usage
// (so a peak is never below "now"). Does not touch allocation state.
void my_diag_reset(void);

// --- opt-in safety checks (off by default) ---------------------------------

// Enables (1) or disables (0) the optional safety features. Turning checks ON
// is what makes my_malloc reserve and stamp a canary, so a later overflow can
// be caught by my_canary_ok and the event counters/hook come alive; turning
// them OFF again stops stamping. Peak/current usage tracking is unaffected (it
// is always on). Canaries only exist for allocations made while enabled, so
// toggling mid-run is allowed. Returns the previous state.
int my_enable_checks(int enable);

// Current state of the opt-in checks: 1 when enabled, 0 otherwise.
int my_checks_enabled(void);

// Verifies the trailing canary of a live allocation: returns 1 when `ptr` is
// an owned, allocated block whose reserved canary word is intact, 0 otherwise
// (NULL, foreign pointer, freed block, a canary broken by a heap OVERFLOW, or
// checks disabled / the block was allocated with checks off so it has no
// canary). When a check fails the bad_canaries / foreign_pointers counters and
// the verbose callback fire. Only meaningful while checks are enabled.
int my_canary_ok(void* ptr);

// Optional verbose sink: called for every detected anomaly while checks are
// enabled (invalid free, double free, corruption, bad canary). Pass NULL to
// clear. The sink receives a NUL-terminated event name and the offending
// pointer (which may be NULL). Not invoked for consistency/ownership queries
// that simply return 0 (those are ordinary API results, not events).
typedef void (*my_diag_hook)(const char* event, void* ptr);
void my_set_diag_hook(my_diag_hook hook);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_DEBUG_H
