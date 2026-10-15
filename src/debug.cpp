// mymalloc — debugging, pointer validation, and diagnostics (Phase 14).
//
// Implements the public surface in <mymalloc/debug.h> and the internal hooks
// in src/debug_internal.h. See docs/phase14_debugging.md for the design.
//
// Three things live here:
//   1. Ownership + consistency: my_owns answers "is this a live allocation of
//      mine?" by locating the tracked region, validating the header and
//      checking the free flag; my_check_heap walks every region and the free
//      list and names the first structural problem it finds.
//   2. Always-on usage accounting: the allocator/regions feed block- and
//      region-size DELTAS through the my_dbg_note_* hooks, and this module
//      keeps running live/mapped totals and their peaks. The totals mirror
//      Phase 13's walk-derived numbers by construction (same block->size and
//      region extents), which the test suite cross-checks.
//   3. Opt-in canaries: while checks are enabled the allocator reserves a
//      trailing alignment unit, ARMS each handed-back block (stamping a magic
//      word at user+aligned_request and recording the block in a debug-only
//      registry) and DISARMS it on free; my_dbg_canary_verify re-reads the
//      word to catch a heap overflow. Presence is tracked EXPLICITLY by the
//      registry — not inferred from block geometry — because a page-rounded
//      fresh region leaves large payload slack whether or not checks are on,
//      so geometry alone would misread a checks-off block after a toggle. The
//      registry is backed by raw memory (my_raw_acquire, never my_malloc) and
//      is cleared whenever checks are turned off, so it only ever holds live
//      armed blocks and the 32-byte block header is untouched.
#include <mymalloc/debug.h>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>

#include "debug_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// --- opt-in state ----------------------------------------------------------

bool g_checks_enabled = false;
my_diag_hook g_hook = nullptr;

// --- always-on usage accounting --------------------------------------------
// Running totals + peaks. live/mapped mirror the heap exactly; peaks only rise.

std::size_t g_live_bytes = 0;
std::size_t g_live_blocks = 0;
std::size_t g_mapped_bytes = 0;
std::size_t g_regions = 0;

std::size_t g_peak_live_bytes = 0;
std::size_t g_peak_live_blocks = 0;
std::size_t g_peak_mapped_bytes = 0;
std::size_t g_peak_regions = 0;

// --- event counters (advance only while checks are enabled) ----------------

unsigned long g_invalid_frees = 0;
unsigned long g_double_frees = 0;
unsigned long g_corruptions = 0;
unsigned long g_bad_canaries = 0;
unsigned long g_foreign_pointers = 0;

// --- canary geometry -------------------------------------------------------
// The reserved unit is a whole alignment multiple; the magic word sits at
// user + align_up(requested), inside that reserved space.
constexpr std::uint64_t kCanaryMagic = 0x4D59414C4C4F4331ULL; // "MYALLOC1"
constexpr std::size_t kCanaryWord = sizeof(std::uint64_t);    // 8

void note_peak_usage() {
    if (g_live_bytes > g_peak_live_bytes) {
        g_peak_live_bytes = g_live_bytes;
    }
    if (g_live_blocks > g_peak_live_blocks) {
        g_peak_live_blocks = g_live_blocks;
    }
    if (g_mapped_bytes > g_peak_mapped_bytes) {
        g_peak_mapped_bytes = g_mapped_bytes;
    }
    if (g_regions > g_peak_regions) {
        g_peak_regions = g_regions;
    }
}

void fire_hook(const char* event, void* ptr) {
    if (g_checks_enabled && g_hook != nullptr) {
        g_hook(event, ptr);
    }
}

// The canary word sits IMMEDIATELY after the user's `requested` bytes (at
// user+requested) so that ANY overflow past the request — even a single byte —
// corrupts it. The offset may be misaligned, so callers read/write it with
// std::memcpy rather than through a uint64_t*. The reserved trailing space
// (an alignment multiple, 16 on x86-64) guarantees requested + kCanaryWord
// stays within the payload, since requested <= align_up(requested) and the
// payload is align_up(requested) + reserve.
unsigned char* canary_address(const my_block_header* block) {
    void* user = my_block_to_user(const_cast<my_block_header*>(block));
    return static_cast<unsigned char*>(user) + block->requested;
}

void canary_write(my_block_header* block) {
    std::memcpy(canary_address(block), &kCanaryMagic, kCanaryWord);
}

// 1 when the stored word equals the magic, 0 otherwise.
int canary_matches(my_block_header* block) {
    std::uint64_t stored = 0;
    std::memcpy(&stored, canary_address(block), kCanaryWord);
    return stored == kCanaryMagic ? 1 : 0;
}

// 1 when `block` has room for a canary at user+requested (requested + word
// fits the payload). A request so large the header field is clamped to the
// HUGE sentinel has no reliable position, so it is treated as having no room.
int block_canary_room(const my_block_header* block) {
    if (block->requested == MY_BLOCK_REQUESTED_HUGE) {
        return 0;
    }
    const std::size_t payload = my_block_payload_size(block);
    const std::size_t requested = block->requested;
    return requested <= payload && payload - requested >= kCanaryWord ? 1 : 0;
}

// --- canary registry (debug-only, raw-memory backed) -----------------------
// A growable set of armed block pointers. Linear search: membership ops are
// O(live armed blocks), acceptable for a debug-only facility. Cleared on
// disable so it never outlives the checks that populated it.

my_block_header** g_armed = nullptr;
std::size_t g_armed_count = 0;
std::size_t g_armed_capacity = 0;

constexpr std::size_t kArmedInitialCapacity = 64U;

bool armed_contains(my_block_header* block) {
    for (std::size_t i = 0; i < g_armed_count; ++i) {
        if (g_armed[i] == block) {
            return true;
        }
    }
    return false;
}

// Grows the armed set (roughly doubling). Buffer comes from my_raw_acquire —
// allocator metadata must never recurse into my_malloc. Returns 1 on success.
int armed_grow() {
    const std::size_t new_capacity =
        g_armed_capacity != 0 ? g_armed_capacity * 2U : kArmedInitialCapacity;
    if (new_capacity > SIZE_MAX / sizeof(my_block_header*)) {
        return 0; // byte size would wrap
    }
    void* buffer = my_raw_acquire(new_capacity * sizeof(my_block_header*));
    if (buffer == NULL) {
        return 0;
    }
    if (g_armed != NULL && g_armed_count > 0) {
        std::memcpy(buffer, static_cast<const void*>(g_armed),
                    g_armed_count * sizeof(my_block_header*));
    }
    if (g_armed != NULL) {
        my_raw_release(static_cast<void*>(g_armed), g_armed_capacity * sizeof(my_block_header*));
    }
    g_armed = static_cast<my_block_header**>(buffer);
    g_armed_capacity = new_capacity;
    return 1;
}

void armed_add(my_block_header* block) {
    if (armed_contains(block)) {
        return; // defensive: never double-arm
    }
    if (g_armed_count == g_armed_capacity && armed_grow() != 1) {
        return; // registry out of memory: skip arming — verify then treats the
                // block as unarmed (safe, just unverified)
    }
    g_armed[g_armed_count++] = block;
}

void armed_remove(my_block_header* block) {
    for (std::size_t i = 0; i < g_armed_count; ++i) {
        if (g_armed[i] == block) {
            g_armed[i] = g_armed[g_armed_count - 1U]; // swap-remove keeps it packed
            --g_armed_count;
            return;
        }
    }
}

// Frees the registry buffer entirely (used when checks are turned off).
void armed_clear() {
    if (g_armed != NULL) {
        my_raw_release(static_cast<void*>(g_armed), g_armed_capacity * sizeof(my_block_header*));
        g_armed = nullptr;
    }
    g_armed_count = 0;
    g_armed_capacity = 0;
}

} // namespace

// ===========================================================================
// internal hooks: accounting (always on)
// ===========================================================================

extern "C" void my_dbg_note_alloc(size_t block_total) {
    g_live_bytes += block_total;
    g_live_blocks += 1U;
    note_peak_usage();
}

extern "C" void my_dbg_note_free(size_t block_total) {
    g_live_bytes -= block_total; // allocator guarantees a matching prior alloc
    g_live_blocks -= 1U;
}

extern "C" void my_dbg_note_resize(size_t old_total, size_t new_total) {
    g_live_bytes += new_total;
    g_live_bytes -= old_total;
    note_peak_usage();
}

extern "C" void my_dbg_note_region_add(size_t bytes) {
    g_mapped_bytes += bytes;
    g_regions += 1U;
    note_peak_usage();
}

extern "C" void my_dbg_note_region_remove(size_t bytes) {
    g_mapped_bytes -= bytes;
    g_regions -= 1U;
}

// ===========================================================================
// internal hooks: canaries (no-ops unless checks are enabled)
// ===========================================================================

extern "C" size_t my_dbg_canary_reserve(void) {
    if (!g_checks_enabled) {
        return 0;
    }
    std::size_t aligned = 0;
    if (my_align_up_size(kCanaryWord, my_default_alignment(), &aligned) != 1) {
        return 0; // unreachable: kCanaryWord is tiny
    }
    return aligned;
}

extern "C" void my_dbg_canary_arm(my_block_header* block) {
    if (!g_checks_enabled || block == nullptr) {
        return;
    }
    // The canary word lives at user+requested and must fit in the payload; a
    // request so large the header field is clamped has no room, so it is left
    // unarmed (verify then never checks it). The offset may be misaligned, so
    // the word is written with memcpy, not through a uint64_t*.
    if (!block_canary_room(block)) {
        return;
    }
    canary_write(block);
    armed_add(block);
}

extern "C" void my_dbg_canary_disarm(my_block_header* block) {
    if (!g_checks_enabled || block == nullptr) {
        return;
    }
    armed_remove(block);
}

extern "C" void my_dbg_canary_restamp(my_block_header* block) {
    if (!g_checks_enabled || block == nullptr || !armed_contains(block)) {
        return; // only re-stamp a block that is actually armed
    }
    if (!block_canary_room(block)) {
        return; // no longer room after the resize: leave it (now unverifiable)
    }
    canary_write(block);
}

extern "C" int my_dbg_canary_verify(my_block_header* block) {
    if (!g_checks_enabled || block == nullptr || !armed_contains(block)) {
        return 1; // disabled, or no canary to check: not a failure
    }
    if (canary_matches(block) != 0) {
        return 1;
    }
    g_bad_canaries += 1U;
    fire_hook("bad canary", my_block_to_user(block));
    return 0;
}

extern "C" int my_dbg_canary_is_armed(my_block_header* block) {
    if (!g_checks_enabled || block == nullptr) {
        return 0;
    }
    return armed_contains(block) ? 1 : 0;
}

// ===========================================================================
// public: ownership
// ===========================================================================

extern "C" int my_owns(const void* ptr) {
    if (ptr == nullptr) {
        return 0;
    }
    // Locate the tracked region containing the pointer. A foreign or interior
    // pointer lands in no tracked region (or in one but not at a block start),
    // which the header validation below then rejects.
    my_region region = {};
    if (my_region_find(ptr, &region) != 1) {
        if (g_checks_enabled) {
            g_foreign_pointers += 1U;
        }
        return 0;
    }
    my_block_header* block = my_user_to_block(const_cast<void*>(ptr));
    if (my_block_valid(block) != 1) {
        if (g_checks_enabled) {
            g_foreign_pointers += 1U;
        }
        return 0;
    }
    // The user pointer must be exactly this block's payload start (not an
    // interior offset), and the block must be allocated, not free.
    if (my_block_to_user(block) != const_cast<void*>(ptr)) {
        if (g_checks_enabled) {
            g_foreign_pointers += 1U;
        }
        return 0;
    }
    return my_block_is_free(block) == 1 ? 0 : 1;
}

// ===========================================================================
// public: consistency
// ===========================================================================

extern "C" int my_check_heap(char* detail, size_t detail_size) {
    auto fail = [&](const char* message) {
        if (detail != nullptr && detail_size > 0) {
            std::snprintf(detail, detail_size, "%s", message);
        }
        if (g_checks_enabled) {
            g_corruptions += 1U;
            fire_hook("heap corruption", nullptr);
        }
        return 0;
    };

    const std::size_t region_count = my_region_count();
    std::size_t walked_free = 0;
    for (std::size_t index = 0; index < region_count; ++index) {
        my_region region = {};
        if (my_region_at(index, &region) != 1) {
            return fail("region entry unreadable");
        }
        const auto base = reinterpret_cast<std::uintptr_t>(region.base);
        const auto limit = base + region.bytes;
        const std::size_t offset = my_block_user_offset();
        const std::size_t max_blocks = region.bytes / offset + 1U;

        auto* block = reinterpret_cast<my_block_header*>(region.base);
        auto last_end = base;
        std::size_t walked = 0;
        while (block != nullptr) {
            if (walked >= max_blocks || my_block_valid(block) != 1) {
                return fail("invalid block header in region");
            }
            const auto address = reinterpret_cast<std::uintptr_t>(block);
            if (address < base || address >= limit || block->size > limit - address) {
                return fail("block escapes its region");
            }
            if (my_block_is_free(block) == 1) {
                ++walked_free;
            }
            last_end = address + block->size;
            block = my_block_next(block);
            ++walked;
        }
        if (last_end != limit) {
            return fail("region blocks do not tile the extent");
        }
    }

    if (walked_free != my_free_list_count()) {
        return fail("free-list count disagrees with the heap");
    }
    if (my_free_list_valid() != 1) {
        return fail("free list is inconsistent");
    }

    if (detail != nullptr && detail_size > 0) {
        std::snprintf(detail, detail_size, "heap consistent: %zu region(s)", region_count);
    }
    return 1;
}

// ===========================================================================
// public: diagnostics snapshot
// ===========================================================================

extern "C" int my_diagnostics(my_diag* out) {
    if (out == nullptr) {
        return 0;
    }
    out->peak_live_bytes = g_peak_live_bytes;
    out->peak_live_blocks = g_peak_live_blocks;
    out->peak_mapped_bytes = g_peak_mapped_bytes;
    out->peak_regions = g_peak_regions;
    out->live_bytes = g_live_bytes;
    out->live_blocks = g_live_blocks;
    out->mapped_bytes = g_mapped_bytes;
    out->regions = g_regions;
    out->invalid_frees = g_invalid_frees;
    out->double_frees = g_double_frees;
    out->corruptions = g_corruptions;
    out->bad_canaries = g_bad_canaries;
    out->foreign_pointers = g_foreign_pointers;
    return 1;
}

extern "C" void my_diag_reset(void) {
    g_invalid_frees = 0;
    g_double_frees = 0;
    g_corruptions = 0;
    g_bad_canaries = 0;
    g_foreign_pointers = 0;
    // Re-baseline peaks to "now" so a peak is never below current usage.
    g_peak_live_bytes = g_live_bytes;
    g_peak_live_blocks = g_live_blocks;
    g_peak_mapped_bytes = g_mapped_bytes;
    g_peak_regions = g_regions;
}

// ===========================================================================
// public: opt-in checks
// ===========================================================================

extern "C" int my_enable_checks(int enable) {
    const bool previous = g_checks_enabled;
    if (enable != 0) {
        g_checks_enabled = true;
    } else {
        // Turning checks off drops the registry so it never outlives the
        // arming that populated it; blocks armed earlier are simply no longer
        // verified (safe — verify treats an unarmed block as "no canary").
        armed_clear();
        g_checks_enabled = false;
    }
    return previous ? 1 : 0;
}

extern "C" int my_checks_enabled(void) {
    return g_checks_enabled ? 1 : 0;
}

extern "C" int my_canary_ok(void* ptr) {
    if (!g_checks_enabled || ptr == nullptr) {
        return 0;
    }
    if (my_owns(ptr) != 1) {
        return 0; // foreign/freed/NULL: my_owns already counted a foreign reject
    }
    my_block_header* block = my_user_to_block(ptr);
    // A block with no canary (allocated with checks off, or too large to arm)
    // reports 0 — there is nothing intact to vouch for.
    if (my_dbg_canary_is_armed(block) != 1) {
        return 0;
    }
    return my_dbg_canary_verify(block);
}

extern "C" void my_set_diag_hook(my_diag_hook hook) {
    g_hook = hook;
}

// ===========================================================================
// internal: free-path event reporting
//
// The allocator's my_free detects a foreign/invalid pointer and an
// already-free (double) pointer precisely; it reports each through these so
// the event counters and the verbose hook advance without the allocator
// reaching into this module's private state.
// ===========================================================================

extern "C" void my_dbg_note_invalid_free(void* ptr) {
    if (g_checks_enabled) {
        g_invalid_frees += 1U;
        fire_hook("invalid free", ptr);
    }
}

extern "C" void my_dbg_note_double_free(void* ptr) {
    if (g_checks_enabled) {
        g_double_frees += 1U;
        fire_hook("double free", ptr);
    }
}
