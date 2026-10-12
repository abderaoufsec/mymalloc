// mymalloc Phase 16 — performance benchmarks and profiling.
//
// A self-contained benchmark that times the allocator's hot paths and compares
// its two placement strategies on an identical workload. It prints ns/op and
// Mops/s for each path and the fragmentation each strategy leaves behind, then
// documents (see docs/phase16_performance.md) WHERE the time goes and why no
// optimization is applied yet — the roadmap rule is "optimize only after
// profiling", and this is the profiling.
//
// Timing uses std::chrono::steady_clock (monotonic, not wall-clock). The
// workloads are deterministic (fixed-seed splitmix64) so first-fit and best-fit
// see byte-for-byte the same sequence of requests — the only difference is
// WHICH free block each placement picks. Numbers are machine-dependent; the
// example reports them, the test suite (test_benchmark) asserts the invariants
// the benchmark relies on rather than any absolute time.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <mymalloc/alignment.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/stats.h>
#include <mymalloc/strategy.h>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ns(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration<double, std::nano>(stop - start).count();
}

// Prints one result line: ns per operation and millions of operations/second.
void report(const char* name, long long ops, double total_ns) {
    const double per_op = ops > 0 ? total_ns / static_cast<double>(ops) : 0.0;
    const double mops = per_op > 0.0 ? 1000.0 / per_op : 0.0;
    std::printf("  %-32s %9.1f ns/op  %8.2f Mops/s  (%lld ops)\n", name, per_op, mops, ops);
}

// Leaves the heap fully free AND reclaims every fully-free region back to the
// OS (one oversized acquire triggers Phase 12 scavenge-on-acquire), so each
// benchmark starts from an identical, empty, single-fresh-region state.
void drain() {
    void* big = my_malloc(my_raw_page_size() * 64U);
    my_free(big);
}

// --- individual path benchmarks --------------------------------------------

// The common reuse path: allocate one block and free it, repeatedly. After the
// first iteration the same freed block is reused via the free list (no mmap),
// so this measures placement + split + free-list bookkeeping, not OS cost.
void bench_alloc_free_reuse() {
    constexpr long long kIters = 500000;
    const Clock::time_point t0 = Clock::now();
    for (long long i = 0; i < kIters; ++i) {
        void* p = my_malloc(64);
        my_free(p);
    }
    const Clock::time_point t1 = Clock::now();
    report("alloc+free reuse (64B)", kIters, elapsed_ns(t0, t1));
}

// The cold path: many DISTINCT live blocks, each forcing a fresh page-rounded
// region (an mmap + a registry entry). Measured separately from the reuse path
// because OS acquisition dominates it.
void bench_fresh_regions() {
    constexpr int kCount = 4096;
    void* ptrs[kCount] = {};
    const Clock::time_point t0 = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        ptrs[i] = my_malloc(64);
    }
    const Clock::time_point t1 = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        my_free(ptrs[i]);
    }
    const Clock::time_point t2 = Clock::now();
    report("fresh acquire (64B, distinct)", kCount, elapsed_ns(t0, t1));
    report("free distinct (64B)", kCount, elapsed_ns(t1, t2));
}

// Resize churn across the split threshold: sizes oscillate so some steps fit in
// place, some shrink, and some must move — the full realloc decision matrix.
void bench_realloc() {
    constexpr long long kIters = 100000;
    const Clock::time_point t0 = Clock::now();
    void* p = my_malloc(32);
    for (long long i = 0; i < kIters; ++i) {
        const std::size_t size = static_cast<std::size_t>(32 + (i & 0x3F));
        p = my_realloc(p, size);
    }
    const Clock::time_point t1 = Clock::now();
    my_free(p);
    report("realloc churn (32..95B)", kIters, elapsed_ns(t0, t1));
}

// --- strategy comparison on an identical deterministic workload ------------
// A small pool of slots is churned by a fixed-seed PRNG; first-fit and best-fit
// receive the SAME sequence of requests, so the only variable is placement. We
// report both the time and the fragmentation each leaves in the live set.
std::uint64_t g_state = 0;

std::uint32_t rnd() {
    g_state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = g_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<std::uint32_t>(z ^ (z >> 31));
}

void bench_strategy(const char* label, enum my_strategy strategy) {
    constexpr std::size_t kSlots = 64;
    constexpr int kSteps = 200000;
    void* slots[kSlots] = {};

    my_set_strategy(strategy);
    g_state = 0xC0FFEEU; // identical workload for both strategies

    const Clock::time_point t0 = Clock::now();
    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rnd() % kSlots;
        const unsigned action = rnd() % 100U;
        if (slots[i] != nullptr && action < 35U) {
            my_free(slots[i]);
            slots[i] = nullptr;
        } else if (slots[i] == nullptr) {
            slots[i] = my_malloc(1 + (rnd() % 2048U));
        } else {
            slots[i] = my_realloc(slots[i], 1 + (rnd() % 2048U));
        }
    }
    const Clock::time_point t1 = Clock::now();

    // Snapshot the fragmentation of the LIVE set before tearing it down.
    my_heap_stats s = {};
    const int ok = my_heap_stats_collect(&s);

    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i] != nullptr) {
            my_free(slots[i]);
        }
    }
    drain(); // back to a single fresh region for the next benchmark

    report(label, kSteps, elapsed_ns(t0, t1));
    if (ok == 1) {
        std::printf(
            "      -> live set: ext_frag=%u/1000  int_frag=%u/1000  regions=%zu  free_bytes=%zu\n",
            s.external_frag_permille, s.internal_frag_permille, s.region_count, s.free_bytes);
    }
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 16: performance & profiling ===\n");
    std::printf("alignment=%zu  page=%zu  default strategy=%s\n\n", my_default_alignment(),
                my_raw_page_size(),
                my_get_strategy() == MY_STRATEGY_FIRST_FIT ? "FIRST_FIT" : "BEST_FIT");

    std::printf("[hot paths]\n");
    bench_alloc_free_reuse();
    drain();
    bench_fresh_regions();
    drain();
    bench_realloc();
    drain();

    std::printf("\n[strategy comparison — identical workload]\n");
    bench_strategy("first-fit mixed churn", MY_STRATEGY_FIRST_FIT);
    bench_strategy("best-fit  mixed churn", MY_STRATEGY_BEST_FIT);

    // Restore the documented default and leave a clean heap.
    my_set_strategy(MY_STRATEGY_FIRST_FIT);
    drain();
    std::printf("\nrestored default strategy: %s\n",
                my_get_strategy() == MY_STRATEGY_FIRST_FIT ? "FIRST_FIT" : "BEST_FIT");
    return 0;
}
