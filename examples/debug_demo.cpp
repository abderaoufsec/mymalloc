// Phase 14 demo: debugging, pointer validation, and diagnostics
// (<mymalloc/debug.h>) — see docs/architecture.md. Walks through the three
// surfaces live: the always-on usage accounting, ownership/consistency checks,
// and the opt-in canaries + event counters that catch invalid/double frees and
// heap overflows.
#include <cstdio>
#include <cstring>

#include <mymalloc/debug.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/stats.h>

namespace {

void print_diag(const char* label) {
    my_diag d = {};
    if (my_diagnostics(&d) != 1) {
        std::printf("  %-22s <snapshot failed>\n", label);
        return;
    }
    std::printf("  %-22s live=%zu blocks  mapped=%zu regions  peaks: %zuB/%zu/%zu\n", label,
                d.live_bytes, d.regions, d.peak_live_bytes, d.peak_live_blocks, d.peak_regions);
}

void hook(const char* event, void* ptr) {
    std::printf("    [hook] event=%-14s ptr=%p\n", event, ptr);
}

} // namespace

int main() {
    std::printf("mymalloc %s — Phase 14 debugging demo\n\n", mymalloc_version());

    // --- always-on accounting (checks OFF) ---------------------------------
    std::printf("1. Usage accounting (checks off): peaks always tracked\n");
    my_diag_reset();
    print_diag("empty:");
    void* a = my_malloc(64);
    void* b = my_malloc(4096);
    void* c = my_calloc(100, 8);
    print_diag("after 3 allocs:");

    my_heap_stats walk = {};
    my_heap_stats_collect(&walk);
    my_diag d = {};
    my_diagnostics(&d);
    std::printf("  cross-check vs Phase 13 walk: live_bytes %s (%zu vs %zu)\n",
                d.live_bytes == walk.live_bytes ? "match" : "MISMATCH", d.live_bytes,
                walk.live_bytes);
    my_free(a);
    print_diag("after free(a):");

    // --- ownership + consistency -------------------------------------------
    std::printf("\n2. Ownership + heap consistency (always available)\n");
    char detail[96] = {0};
    std::printf("  my_owns(b)=%d  my_owns(stack)=%d\n", my_owns(b),
                my_owns(detail)); // detail is a stack buffer: foreign
    std::printf("  my_check_heap -> %d: \"%s\"\n", my_check_heap(detail, sizeof(detail)), detail);

    // --- invalid + double free (checks ON) ---------------------------------
    std::printf("\n3. Invalid & double free detection (checks on)\n");
    my_enable_checks(1);
    my_set_diag_hook(hook);
    my_diag_reset();
    my_free(detail); // foreign pointer -> invalid free
    void* p = my_malloc(128);
    my_free(p);
    my_free(p); // double free
    my_diagnostics(&d);
    std::printf("  invalid_frees=%lu  double_frees=%lu\n", d.invalid_frees, d.double_frees);
    my_set_diag_hook(nullptr);

    // --- canaries catch an overflow (checks ON) ----------------------------
    std::printf("\n4. Canaries catch a heap overflow (checks on)\n");
    void* q = my_malloc(24);
    std::printf("  my_canary_ok(q)=%d (fresh allocation)\n", my_canary_ok(q));
    // Overflow: scribble past the 24-byte request into the reserved canary.
    std::memset(static_cast<unsigned char*>(q) + 24, 0xAB, 8);
    std::printf("  my_canary_ok(q)=%d (after overflow)\n", my_canary_ok(q));
    my_diagnostics(&d);
    std::printf("  bad_canaries=%lu\n", d.bad_canaries);
    my_free(q); // a checked free also reports the broken canary

    // --- clean shutdown ----------------------------------------------------
    std::printf("\n5. Disable checks, free the rest, verify consistency\n");
    my_enable_checks(0);
    my_free(b);
    my_free(c);
    std::printf("  my_check_heap -> %d: \"%s\"\n", my_check_heap(detail, sizeof(detail)), detail);
    my_diagnostics(&d);
    std::printf("  final: live=%zu blocks, peak_live_bytes=%zu\n", d.live_bytes, d.peak_live_bytes);
    return 0;
}
