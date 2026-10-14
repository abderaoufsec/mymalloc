// Phase 14 test: debugging, pointer validation, and diagnostics
// (<mymalloc/debug.h>) against docs/phase14_debugging.md. Exercises the three
// surfaces: always-available ownership + heap-consistency, the always-on usage
// accounting (cross-checked against Phase 13's independent walk), and the
// opt-in canaries / event counters / verbose hook.
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/debug.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>
#include <mymalloc/stats.h>

#include "test_framework.hpp"

namespace {

// --- verbose hook capture (for the event-callback scenario) ----------------

int g_hook_events = 0;
char g_last_event[64] = {0};

void counting_hook(const char* event, void* /*ptr*/) {
    ++g_hook_events;
    std::snprintf(g_last_event, sizeof(g_last_event), "%s", event);
}

// --- fresh heap per scenario ---------------------------------------------------
// Each scenario starts from an empty heap so the accounting totals (which are
// process-global and monotone in their peaks) can be checked from a known
// baseline. We drain by freeing everything we allocated; a fully-freed region
// is reclaimed on the next acquire (Phase 12), and we reset the diagnostics.

// Frees every allocation tracked in `ptrs` and clears the vector.
template <std::size_t N> void free_all(void* (&ptrs)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (ptrs[i] != nullptr) {
            my_free(ptrs[i]);
            ptrs[i] = nullptr;
        }
    }
}

// The independent ground truth from Phase 13 (walk-derived), used to prove the
// always-on counters in my_diag mirror the heap exactly.
my_heap_stats collect() {
    my_heap_stats s = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&s) == 1);
    return s;
}

} // namespace

int main() {
    // Drain any allocations earlier suites may have left (each test is its own
    // process, so this is just defensive) and start from a clean diagnostic
    // baseline. Checks begin disabled — the default.
    MYMALLOC_CHECK(my_checks_enabled() == 0);
    my_diag_reset();

    char detail[128] = {0};

    // === Scenario 1: default state — no checks, heap consistent ============
    {
        void* a = my_malloc(64);
        void* b = my_malloc(200);
        MYMALLOC_CHECK(a != nullptr);
        MYMALLOC_CHECK(b != nullptr);

        // With checks off, canaries do not exist and cannot be queried.
        MYMALLOC_CHECK(my_canary_ok(a) == 0);

        // Ownership works with checks OFF too (it is always available).
        MYMALLOC_CHECK(my_owns(a) == 1);
        MYMALLOC_CHECK(my_owns(b) == 1);
        int stack_local = 0;
        MYMALLOC_CHECK(my_owns(&stack_local) == 0); // foreign pointer
        MYMALLOC_CHECK(my_owns(nullptr) == 0);
        MYMALLOC_CHECK(my_owns(static_cast<unsigned char*>(a) + 1) == 0); // interior offset

        // A consistent heap reports success and a summary.
        MYMALLOC_CHECK(my_check_heap(detail, sizeof(detail)) == 1);
        MYMALLOC_CHECK(std::strstr(detail, "consistent") != nullptr);

        my_free(a);
        my_free(b);
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);
    }

    // === Scenario 2: always-on accounting mirrors the Phase 13 walk ========
    {
        my_diag_reset();
        void* p[6] = {};
        p[0] = my_malloc(1);
        p[1] = my_malloc(16);
        p[2] = my_malloc(100);
        p[3] = my_malloc(1000);
        p[4] = my_malloc(4096);
        p[5] = my_calloc(10, 40);
        for (void* q : p) {
            MYMALLOC_CHECK(q != nullptr);
        }

        my_heap_stats walk = collect();
        my_diag d = {};
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);

        // The incremental counters must equal the independent walk exactly.
        MYMALLOC_CHECK(d.live_blocks == walk.live_blocks);
        MYMALLOC_CHECK(d.live_bytes == walk.live_bytes);
        MYMALLOC_CHECK(d.regions == walk.region_count);
        MYMALLOC_CHECK(d.mapped_bytes == walk.mapped_bytes);

        // Peaks are at least the current usage and were reached here.
        MYMALLOC_CHECK(d.peak_live_blocks >= d.live_blocks);
        MYMALLOC_CHECK(d.peak_live_bytes >= d.live_bytes);
        MYMALLOC_CHECK(d.peak_regions >= d.regions);
        MYMALLOC_CHECK(d.peak_mapped_bytes >= d.mapped_bytes);

        // Freeing half drops the live totals but never the peaks.
        const std::size_t before_blocks = d.live_blocks;
        const std::size_t peak_blocks = d.peak_live_blocks;
        const std::size_t peak_bytes = d.peak_live_bytes;
        my_free(p[0]);
        my_free(p[2]);
        my_free(p[4]);
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.live_blocks == before_blocks - 3);
        MYMALLOC_CHECK(d.peak_live_blocks == peak_blocks); // unchanged
        MYMALLOC_CHECK(d.peak_live_bytes == peak_bytes);

        walk = collect();
        MYMALLOC_CHECK(d.live_blocks == walk.live_blocks);
        MYMALLOC_CHECK(d.live_bytes == walk.live_bytes);

        // realloc accounting: growing in place or moving keeps the walk in step.
        p[1] = my_realloc(p[1], 300);
        MYMALLOC_CHECK(p[1] != nullptr);
        walk = collect();
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.live_bytes == walk.live_bytes);
        MYMALLOC_CHECK(d.live_blocks == walk.live_blocks);

        free_all(p);
        walk = collect();
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.live_blocks == 0);
        MYMALLOC_CHECK(d.live_blocks == walk.live_blocks);
        MYMALLOC_CHECK(d.live_bytes == walk.live_bytes);
    }

    // === Scenario 3: invalid & double free detection (checks on) ===========
    {
        MYMALLOC_CHECK(my_enable_checks(1) == 0); // was off
        MYMALLOC_CHECK(my_checks_enabled() == 1);
        my_set_diag_hook(counting_hook);
        g_hook_events = 0;
        my_diag_reset();

        // A foreign free is an invalid free. Free an INTERIOR pointer of a
        // real allocation rather than a foreign stack address: it is a pointer
        // my_malloc never handed out, so my_free must reject it, but it still
        // lies inside the allocator's mapped memory. (A foreign stack address
        // would make the header validator read outside the heap — undefined
        // behavior that AddressSanitizer flags as a stack-buffer-underflow.)
        // The +1 byte offset keeps the computed block pointer misaligned, so
        // my_block_valid rejects it on the alignment check without dereferencing.
        void* q = my_malloc(64);
        MYMALLOC_CHECK(q != nullptr);
        my_free(static_cast<unsigned char*>(q) + 1); // interior byte: invalid free
        my_diag d = {};
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.invalid_frees == 1);
        MYMALLOC_CHECK(g_hook_events == 1);
        MYMALLOC_CHECK(std::strcmp(g_last_event, "invalid free") == 0);
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1); // still consistent
        my_free(q); // release the real block (no leak under ASan)

        // A double free is counted separately and does not corrupt the heap.
        void* p = my_malloc(128);
        MYMALLOC_CHECK(p != nullptr);
        my_free(p);
        my_free(p); // second free of the same pointer
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.double_frees == 1);
        MYMALLOC_CHECK(d.invalid_frees == 1); // unchanged
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);
        MYMALLOC_CHECK(std::strstr(g_last_event, "free") != nullptr);

        // free(nullptr) is a no-op and is NOT an invalid free.
        const unsigned long before = d.double_frees;
        my_free(nullptr);
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.double_frees == before);
        MYMALLOC_CHECK(d.invalid_frees == 1);

        my_set_diag_hook(nullptr);
        MYMALLOC_CHECK(my_enable_checks(0) == 1); // was on
    }

    // === Scenario 4: canaries catch a heap overflow (checks on) ============
    {
        MYMALLOC_CHECK(my_enable_checks(1) == 0);
        my_diag_reset();

        // A small request gets a reserved trailing canary that verifies clean.
        void* p = my_malloc(20);
        MYMALLOC_CHECK(p != nullptr);
        MYMALLOC_CHECK(my_canary_ok(p) == 1);
        MYMALLOC_CHECK(my_owns(p) == 1);

        // Writing past the request corrupts the canary: verify fails. The
        // canary sits IMMEDIATELY after the request (user+requested), so even
        // a single byte at offset `requested` hits it — no padding blind spot.
        unsigned char* bytes = static_cast<unsigned char*>(p);
        const std::size_t canary_at = 20; // == requested: canary starts here
        bytes[canary_at] ^= 0xFFU;        // flip one canary byte
        MYMALLOC_CHECK(my_canary_ok(p) == 0);
        my_diag d = {};
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.bad_canaries >= 1);

        // A checked free also reports the broken canary, then frees normally.
        const unsigned long before = d.bad_canaries;
        my_free(p);
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.bad_canaries == before + 1);
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);

        // A canary on an untouched allocation verifies, and freeing it cleanly
        // leaves no new bad-canary event.
        void* q = my_malloc(64);
        MYMALLOC_CHECK(my_canary_ok(q) == 1);
        const unsigned long clean_before = d.bad_canaries;
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        my_free(q);
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.bad_canaries == clean_before);

        // A foreign pointer has no canary to check.
        int stack_local = 0;
        MYMALLOC_CHECK(my_canary_ok(&stack_local) == 0);
        MYMALLOC_CHECK(my_canary_ok(nullptr) == 0);

        MYMALLOC_CHECK(my_enable_checks(0) == 1);
    }

    // === Scenario 5: heap-consistency checker detects metadata corruption ===
    {
        MYMALLOC_CHECK(my_enable_checks(1) == 0);
        my_set_diag_hook(counting_hook);
        g_hook_events = 0;
        my_diag_reset();

        void* p = my_malloc(256);
        MYMALLOC_CHECK(p != nullptr);
        MYMALLOC_CHECK(my_check_heap(detail, sizeof(detail)) == 1);

        // Corrupt the block header's size to something that breaks the tiling,
        // and the checker must fail with a message (and fire the hook + count).
        my_block_header* block = my_user_to_block(p);
        const std::size_t saved = block->size;
        block->size = saved + 8; // no longer an alignment multiple / tiles wrong
        MYMALLOC_CHECK(my_check_heap(detail, sizeof(detail)) == 0);
        MYMALLOC_CHECK(detail[0] != '\0'); // a problem was named
        my_diag d = {};
        MYMALLOC_CHECK(my_diagnostics(&d) == 1);
        MYMALLOC_CHECK(d.corruptions >= 1);
        MYMALLOC_CHECK(g_hook_events >= 1);

        // Restore: the heap is consistent again (proves the failure was real).
        block->size = saved;
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);

        my_free(p);
        my_set_diag_hook(nullptr);
        MYMALLOC_CHECK(my_enable_checks(0) == 1);
    }

    // === Scenario 6: detail buffer is bounded & NUL-terminated =============
    {
        MYMALLOC_CHECK(my_enable_checks(1) == 0);
        void* p = my_malloc(32);
        MYMALLOC_CHECK(p != nullptr);

        char tiny[8] = {0};
        MYMALLOC_CHECK(my_check_heap(tiny, sizeof(tiny)) == 1);
        // A success summary is truncated to fit but always terminated.
        MYMALLOC_CHECK(tiny[sizeof(tiny) - 1] == '\0');

        // A zero-size buffer writes nothing and does not overrun.
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);

        my_free(p);
        MYMALLOC_CHECK(my_enable_checks(0) == 1);
    }

    // === Scenario 7: toggling checks off drops the canary state ============
    {
        void* p = my_malloc(48);
        MYMALLOC_CHECK(p != nullptr);
        // Allocated with checks OFF: no canary was ever stamped.
        MYMALLOC_CHECK(my_enable_checks(1) == 0);
        MYMALLOC_CHECK(my_canary_ok(p) == 0); // not armed: nothing to verify
        // Enabling then disabling clears the registry cleanly.
        MYMALLOC_CHECK(my_enable_checks(0) == 1);
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);
        my_free(p);

        // Round-trip: an allocation made with checks ON verifies; disabling
        // then freeing it does not trip a stale canary.
        MYMALLOC_CHECK(my_enable_checks(1) == 0);
        void* q = my_malloc(48);
        MYMALLOC_CHECK(my_canary_ok(q) == 1);
        MYMALLOC_CHECK(my_enable_checks(0) == 1);
        my_free(q);
        MYMALLOC_CHECK(my_check_heap(nullptr, 0) == 1);
    }

    MYMALLOC_TEST_MAIN("test_debug");
}
