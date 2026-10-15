// mymalloc — internal debug hooks (Phase 14). NOT a public header: included
// only by the library's own translation units (src/debug.cpp, src/allocator.cpp,
// src/regions.cpp). The public surface lives in <mymalloc/debug.h>; this header
// carries the narrow coupling that lets the allocator feed the always-on usage
// accounting and drive the opt-in canaries without exposing any of it to users.
//
// Two responsibilities:
//
//   1. Always-on accounting. After every successful alloc/free/resize the
//      allocator reports the block-size DELTA, and after every region
//      track/release it reports the mapped-bytes DELTA, so src/debug.cpp can
//      keep running live/mapped totals and their peaks exact with no walk.
//      These hooks are ALWAYS called (peaks are always maintained) and only
//      ever touch counters — never memory. Kept consistent with Phase 13's
//      walk-derived numbers by construction (same block->size totals).
//
//   2. Opt-in canaries. While checks are enabled the allocator (a) asks how
//      many EXTRA payload bytes to reserve so a trailing canary fits, (b) ARMS
//      a block when it hands a payload back (stamps the canary and records the
//      block in a debug-only registry), (c) DISARMS it on free, and (d) RE-STAMPS
//      after an in-place resize moved the canary. Every canary hook is a no-op
//      when checks are disabled, so the Phase 5-13 block layout, sizes and
//      behaviour are byte-for-byte unchanged in release builds.
//
//      Presence is tracked EXPLICITLY (a registry of armed block pointers)
//      rather than inferred from block geometry: a page-rounded fresh region
//      leaves large payload slack whether or not checks are on, so geometry
//      alone cannot tell an armed block from a plain one and would misread a
//      checks-off block after toggling. The registry is backed by raw memory
//      (my_raw_acquire — never my_malloc) and exists only while checks are on.
#ifndef MYMALLOC_DEBUG_INTERNAL_H
#define MYMALLOC_DEBUG_INTERNAL_H

#include <atomic>
#include <stddef.h>

#include <mymalloc/block.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- thread safety: process-wide spinlock (atomic_flag, zero-allocation) ---

// Global spinlock initialized once to ATOMIC_FLAG_INIT (unlocked).
// Allocation entry points acquire/release this lock so that concurrent
// my_malloc/my_free calls from multiple threads do not corrupt internal
// metadata (free-list links, region registry, canary registry).
// The lock is never held across my_malloc/my_free calls from different
// threads in a way that would cause deadlock: the lock is very short-held
// (only the critical section of header/free-list update) and the lock
// ordering is simple (one global lock, no nested locks).
inline void lock_allocator(void) {
    // Myers's algorithm / simple test-and-set via atomic_flag.
    // C++11 std::atomic_flag is lock-free on every mainstream platform.
    g_alloc_lock.test_and_set(std::memory_order_acquire);
}

inline void unlock_allocator(void) {
    g_alloc_lock.clear(std::memory_order_release);
}

// Global spinlock (default: unlocked = clear).
inline bool g_alloc_lock_initialized = false;
// Note: g_alloc_lock itself is implicitly zero-initialized (ATOMIC_FLAG_INIT),
// so the lock starts in the unlocked state without any explicit init call.

// --- always-on accounting (called on the allocator happy paths) ------------

// A block of `block_total` bytes (header included, i.e. block->size) just
// became live: +block_total live bytes, +1 live block; peaks updated.
void my_dbg_note_alloc(size_t block_total);

// A live block of `block_total` bytes just became free: -block_total live
// bytes, -1 live block (peaks only ever rise, so no update here).
void my_dbg_note_free(size_t block_total);

// A live block was resized in place from `old_total` to `new_total` bytes;
// the live block count is unchanged, live bytes move by new_total-old_total.
void my_dbg_note_resize(size_t old_total, size_t new_total);

// A tracked region of `bytes` was adopted: +bytes mapped, +1 region.
void my_dbg_note_region_add(size_t bytes);

// A tracked region of `bytes` was released: -bytes mapped, -1 region.
void my_dbg_note_region_remove(size_t bytes);

// --- opt-in canaries (no-ops unless checks are enabled) --------------------

// Extra payload bytes the allocator MUST reserve beyond the aligned request so
// a trailing canary fits. An alignment multiple; 0 when checks are disabled.
size_t my_dbg_canary_reserve(void);

// Arms an allocated `block`: reserves room was already added by the caller, so
// this stamps the canary at user+aligned_request and records the block in the
// debug registry. No-op when checks are disabled. Called right after a payload
// is handed back (my_malloc).
void my_dbg_canary_arm(my_block_header* block);

// Disarms a `block` being freed: drops it from the registry so its bytes are
// never verified again. No-op when checks are disabled or the block was never
// armed. Called by my_free before coalescing.
void my_dbg_canary_disarm(my_block_header* block);

// Re-stamps the canary of an already-armed `block` after an in-place resize
// moved its requested size (realloc fits/absorb). No-op when checks are
// disabled or the block is not armed.
void my_dbg_canary_restamp(my_block_header* block);

// Verifies the canary of an allocated `block`. Returns 1 when checks are
// disabled, when the block is not armed (no canary), or when the canary is
// intact; returns 0 when the block is armed but its canary is broken (a heap
// OVERFLOW) — bumping bad_canaries and firing the hook when checks enabled.
// NOTE: "not armed" is NOT a failure here (a checked free of a checks-off
// block must not report corruption); the public my_canary_ok layers the
// stricter "0 when there is no canary at all" rule on top via
// my_dbg_canary_is_armed.
int my_dbg_canary_verify(my_block_header* block);

// Reports whether `block` is currently armed (has a stamped, tracked canary).
// 0 when checks are disabled or the block was never armed. Used by the public
// my_canary_ok to distinguish "no canary present" from "canary intact".
int my_dbg_canary_is_armed(my_block_header* block);

// --- free-path event reporting ---------------------------------------------

// Reports that my_free was given a foreign or structurally invalid pointer
// (not something this allocator handed out). Bumps invalid_frees and fires
// the hook while checks are enabled; a no-op otherwise.
void my_dbg_note_invalid_free(void* ptr);

// Reports that my_free was given a pointer whose block is already free (a
// double free). Bumps double_frees and fires the hook while checks are
// enabled; a no-op otherwise.
void my_dbg_note_double_free(void* ptr);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_DEBUG_INTERNAL_H
