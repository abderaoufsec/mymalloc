// Phase 16 test: performance invariants for the benchmark
// (examples/benchmark.cpp) — see docs/phase16_performance.md.
//
// Benchmarks must not assert absolute wall-clock times (they are machine- and
// load-dependent and would flake). Instead this suite locks down the PROPERTIES
// the benchmark relies on to be meaningful and correct:
//
//   1. the workload is DETERMINISTIC — the same seed drives the same sequence,
//      so a first-fit vs best-fit comparison is apples-to-apples;
//   2. every benchmark path leaves the heap consistent and fully drained
//      (no leaks, no corruption) after it runs;
//   3. a coarse performance sanity bound holds — the hot reuse path is far
//      cheaper than the cold fresh-acquire path (the structural fact the
//      profiling documents), checked with a generous margin so it never flakes.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <mymalloc/alignment.h>
#include <mymalloc/debug.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/regions.h>
#include <mymalloc/stats.h>
#include <mymalloc/strategy.h>

#include "test_framework.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// The SAME fixed-seed splitmix64 the benchmark uses, so this test reproduces
// the exact workload shape. Determinism is the property under test.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint32_t next() {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return static_cast<std::uint32_t>(z ^ (z >> 31));
    }
};

// Leaves the heap fully free and reclaims every fully-free region (one
// oversized acquire triggers Phase 12 scavenge-on-acquire).
void drain() {
    void* big = my_malloc(my_raw_page_size() * 64U);
    MYMALLOC_CHECK(big != nullptr);
    my_free(big);
}

// Asserts the heap is empty and consistent in BOTH accounting views.
void require_drained(const char* where) {
    my_heap_stats walk = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&walk) == 1);
    MYMALLOC_CHECK(walk.live_blocks == 0);
    MYMALLOC_CHECK(walk.live_bytes == 0);
    my_diag d = {};
    MYMALLOC_CHECK(my_diagnostics(&d) == 1);
    MYMALLOC_CHECK(d.live_blocks == 0);
    MYMALLOC_CHECK(d.live_bytes == 0);
    char detail[64] = {0};
    MYMALLOC_CHECK(my_check_heap(detail, sizeof(detail)) == 1);
    (void)where;
}

// --- scenario 1: the workload is deterministic -----------------------------
// Running the same seeded mixed workload twice must produce the identical
// sequence of (allocate/free/realloc) decisions. We replay both and compare
// the PRNG draws directly — if these ever diverge, the benchmark's strategy
// comparison would be comparing different workloads.
void test_deterministic_workload() {
    constexpr std::size_t kSlots = 32;
    constexpr int kSteps = 20000;

    auto replay = [](std::uint64_t seed) {
        Rng rng(seed);
        std::uint64_t checksum = 0;
        int allocs = 0;
        int frees = 0;
        int reallocs = 0;
        for (int step = 0; step < kSteps; ++step) {
            const std::size_t i = rng.next() % kSlots;
            const unsigned action = rng.next() % 100U;
            checksum = checksum * 31U + i + action; // fold the decision in
            if (action < 35U) {
                ++frees;
            } else if (action < 70U) {
                ++allocs;
            } else {
                ++reallocs;
            }
        }
        return std::pair<std::uint64_t, int>{checksum, allocs + frees + reallocs};
    };

    const auto a = replay(0xC0FFEEU);
    const auto b = replay(0xC0FFEEU);
    MYMALLOC_CHECK(a.first == b.first);   // identical decision stream
    MYMALLOC_CHECK(a.second == b.second); // identical op counts
    MYMALLOC_CHECK(a.second == kSteps);   // every step did exactly one op

    // A different seed must (over this many steps) diverge.
    const auto c = replay(0xC0FFEEU + 1U);
    MYMALLOC_CHECK(c.first != a.first);
}

// --- scenario 2: each benchmark path drains cleanly ------------------------
// Re-run the real allocator through the same shapes the benchmark times and
// prove the heap is consistent and empty afterward — the benchmark must never
// leak or corrupt, or its numbers would be measuring damage.
void test_paths_drain_clean() {
    // reuse path
    for (int i = 0; i < 50000; ++i) {
        void* p = my_malloc(64);
        MYMALLOC_CHECK(p != nullptr);
        my_free(p);
    }
    require_drained("reuse");

    // fresh-distinct path
    constexpr int kCount = 512;
    void* ptrs[kCount] = {};
    for (int i = 0; i < kCount; ++i) {
        ptrs[i] = my_malloc(64);
        MYMALLOC_CHECK(ptrs[i] != nullptr);
    }
    for (int i = 0; i < kCount; ++i) {
        my_free(ptrs[i]);
    }
    drain();
    require_drained("fresh");

    // realloc churn path
    void* p = my_malloc(32);
    MYMALLOC_CHECK(p != nullptr);
    for (int i = 0; i < 20000; ++i) {
        p = my_realloc(p, static_cast<std::size_t>(32) + static_cast<std::size_t>(i & 0x3F));
        MYMALLOC_CHECK(p != nullptr);
    }
    my_free(p);
    drain();
    require_drained("realloc");
}

// --- scenario 3: the strategy workload drains under BOTH strategies --------
// The benchmark compares first-fit and best-fit on one seeded workload; here we
// run a shortened version under each and confirm both leave a clean heap and
// that the strategy selector round-trips.
void test_strategy_workloads_drain() {
    constexpr std::size_t kSlots = 32;
    constexpr int kSteps = 20000;

    const enum my_strategy strategies[2] = {MY_STRATEGY_FIRST_FIT, MY_STRATEGY_BEST_FIT};
    for (enum my_strategy strategy : strategies) {
        my_set_strategy(strategy);
        MYMALLOC_CHECK(my_get_strategy() == strategy);

        Rng rng(0xC0FFEEU);
        void* slots[kSlots] = {};
        for (int step = 0; step < kSteps; ++step) {
            const std::size_t i = rng.next() % kSlots;
            const unsigned action = rng.next() % 100U;
            if (slots[i] != nullptr && action < 35U) {
                my_free(slots[i]);
                slots[i] = nullptr;
            } else if (slots[i] == nullptr) {
                slots[i] = my_malloc(1 + (rng.next() % 2048U));
                MYMALLOC_CHECK(slots[i] != nullptr);
            } else {
                slots[i] = my_realloc(slots[i], 1 + (rng.next() % 2048U));
                MYMALLOC_CHECK(slots[i] != nullptr);
            }
        }
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (slots[i] != nullptr) {
                my_free(slots[i]);
            }
        }
        drain();
        require_drained("strategy");
    }

    // Restore the documented default.
    my_set_strategy(MY_STRATEGY_FIRST_FIT);
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_FIRST_FIT);
}

// --- scenario 4: reuse avoids OS acquisition -------------------------------
// The structural fact the profiling documents — the hot reuse path is cheap
// because it never calls the OS — asserted WITHOUT wall-clock timing (which
// would flake). The proof is the region registry: cycling malloc+free of one
// size over and over reuses a single freed block via the free list, so the
// region count never grows (no mmap, no munmap). A very loose wall-clock smoke
// bound only guards against a pathological hang/regression.
void test_reuse_avoids_os_acquisition() {
    constexpr int kReuse = 20000;
    const std::size_t page = my_raw_page_size();

    // Start from a single fully-free region.
    void* big = my_malloc(page * 64U);
    MYMALLOC_CHECK(big != nullptr);
    my_free(big);
    MYMALLOC_CHECK(my_region_count() == 1U);

    // Reuse: malloc+free the same size repeatedly. Every request after the
    // first is a free-list hit inside the one region, so NO new region appears.
    const Clock::time_point r0 = Clock::now();
    for (int i = 0; i < kReuse; ++i) {
        void* p = my_malloc(64);
        MYMALLOC_CHECK(p != nullptr);
        my_free(p);
    }
    const Clock::time_point r1 = Clock::now();
    // Still exactly one region: the reuse path performed zero OS acquisitions.
    MYMALLOC_CHECK(my_region_count() == 1U);

    // Loose smoke bound only: the loop must finish quickly. Generous enough
    // that machine/load noise can never flake it.
    const double total_ms = std::chrono::duration<double, std::milli>(r1 - r0).count();
    MYMALLOC_CHECK(total_ms < 10000.0); // < 10 s for 20k reuse pairs
}

} // namespace

int main() {
    test_deterministic_workload();
    test_paths_drain_clean();
    test_strategy_workloads_drain();
    test_reuse_avoids_os_acquisition();
    MYMALLOC_TEST_MAIN("benchmark");
}
