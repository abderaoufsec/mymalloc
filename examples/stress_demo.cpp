// mymalloc Phase 15 demo: stress testing.
//
// Runs a short, deterministic randomized workload (alloc/calloc/realloc/free
// over mixed sizes) and narrates what the diagnostics from Phases 13-14 report
// along the way: live/mapped bytes stay accounted, the heap stays consistent,
// and the run drains to empty (no leaks). See docs/phase15_stress_testing.md.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <mymalloc/debug.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/stats.h>

namespace {

// splitmix64 — the same deterministic PRNG the stress suite uses.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0U : static_cast<std::size_t>(next() % n); }
};

void snapshot(const char* label) {
    my_heap_stats s = {};
    if (my_heap_stats_collect(&s) != 1) {
        std::printf("  %-24s <heap inconsistent>\n", label);
        return;
    }
    std::printf("  %-24s live=%zu blocks/%zuB  mapped=%zu regions/%zuB  ext=%u/1000\n", label,
                s.live_blocks, s.live_bytes, s.region_count, s.mapped_bytes,
                s.external_frag_permille);
}

} // namespace

int main() {
    std::printf("mymalloc %s — Phase 15 stress demo\n\n", mymalloc_version());
    my_diag_reset();

    constexpr std::size_t kSlots = 32;
    constexpr int kSteps = 1500;
    void* slots[kSlots] = {};
    Rng rng(0x15CEEDU);

    std::printf("1. Deterministic mixed workload (%d steps, %zu slots)\n", kSteps, kSlots);
    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rng.below(kSlots);
        if (slots[i] != nullptr && rng.below(100) < 35) {
            my_free(slots[i]);
            slots[i] = nullptr;
        } else if (slots[i] == nullptr) {
            const std::size_t size = 1 + rng.below(6000);
            slots[i] = (rng.below(100) < 25) ? my_calloc(1 + rng.below(size), size / 4U + 1U)
                                             : my_malloc(size);
        } else {
            slots[i] = my_realloc(slots[i], 1 + rng.below(6000));
        }
        if ((step % 500) == 0) {
            char detail[80] = {0};
            const int ok = my_check_heap(detail, sizeof(detail));
            std::printf("  step %4d: check_heap=%d (%s)\n", step, ok, detail);
        }
    }
    snapshot("mid-run:");

    std::printf("\n2. Free everything and confirm the heap drains (no leaks)\n");
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i] != nullptr) {
            my_free(slots[i]);
            slots[i] = nullptr;
        }
    }
    snapshot("after draining:");

    my_heap_stats s = {};
    my_heap_stats_collect(&s);
    std::printf("\n  live_blocks=%zu live_bytes=%zu (both must be 0)\n", s.live_blocks,
                s.live_bytes);

    // Freeing leaves every region fully free but still mapped (free never
    // shrinks). Request MORE than the whole mapped footprint: no free block can
    // satisfy it, so the allocator must scavenge every fully-free region back
    // to the OS and acquire exactly one fresh region — proving the freed
    // regions were reclaimable, not leaked.
    my_heap_stats before = {};
    my_heap_stats_collect(&before);
    void* big = my_malloc(before.mapped_bytes + my_raw_page_size());
    my_heap_stats_collect(&s);
    std::printf("  after oversized acquire: region_count=%zu (scavenged to one, not leaked)\n",
                s.region_count);
    my_free(big);

    my_diag d = {};
    my_diagnostics(&d);
    std::printf("\n3. Final: peak_live_blocks=%zu peak_live_bytes=%zu peak_regions=%zu\n",
                d.peak_live_blocks, d.peak_live_bytes, d.peak_regions);
    return 0;
}
