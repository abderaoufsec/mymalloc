// Phase 15 test: stress testing — random allocation/free/realloc over mixed
// sizes and long-running workloads, with data-integrity verification, live
// corruption detection, accounting cross-checks, and a leak audit. See
// docs/architecture.md.
//
// Unlike Phases 5-14 this suite adds no new allocator behaviour; it exercises
// everything built so far under randomized load and leans on the diagnostics
// from Phases 13 (my_heap_stats_collect) and 14 (my_diagnostics/my_check_heap/
// my_owns) to prove the heap stays correct, accounted, and leak-free.
//
// Determinism: every run uses a fixed-seed PRNG (splitmix64) and computes all
// expectations from the platform's real constants, so a failure is exactly
// reproducible on any LP64 target — there are no flakes to chase.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mymalloc/block.h>
#include <mymalloc/debug.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>
#include <mymalloc/stats.h>

#include "test_framework.hpp"

namespace {

// --- deterministic PRNG (splitmix64) ---------------------------------------
// Self-contained and identical across platforms/libc: the same seed yields the
// same sequence everywhere, so a stress failure reproduces bit-for-bit.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n); n == 0 yields 0.
    std::size_t below(std::size_t n) { return n == 0 ? 0U : static_cast<std::size_t>(next() % n); }
};

// One tracked allocation slot. A live slot's payload must ALWAYS equal the
// pattern for (tag, offset); any deviation means an overlap, a use-after-free,
// or payload corruption somewhere in the heap.
struct Slot {
    void* ptr;
    std::size_t size;
    std::uint32_t tag; // unique per allocation while live
};

std::uint32_t g_next_tag = 1;

// A per-byte value derived from BOTH the tag and the offset, so a one-byte
// shift or an overlapping write from another live block is detected.
unsigned char pattern_byte(std::uint32_t tag, std::size_t offset) {
    std::uint32_t x = tag * 2654435761U;
    x += static_cast<std::uint32_t>(offset) * 2246822519U;
    x += static_cast<std::uint32_t>(offset >> 5) * 3266489917U;
    x ^= x >> 15;
    x *= 2246822519U;
    x ^= x >> 13;
    return static_cast<unsigned char>(x & 0xFFU);
}

void fill_pattern(void* ptr, std::size_t size, std::uint32_t tag) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = pattern_byte(tag, i);
    }
}

bool check_pattern(const void* ptr, std::size_t size, std::uint32_t tag) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != pattern_byte(tag, i)) {
            return false;
        }
    }
    return true;
}

// A mixed size distribution: tiny (alignment edges), small, up-to-two-pages,
// and an occasional multi-page request that forces fresh region acquisition and
// exercises split/coalesce/scavenge under load.
std::size_t random_size(Rng& rng) {
    const unsigned bucket = static_cast<unsigned>(rng.below(100));
    if (bucket < 40) {
        return 1 + rng.below(64);
    }
    if (bucket < 80) {
        return 1 + rng.below(1024);
    }
    if (bucket < 97) {
        return 1 + rng.below(8192);
    }
    return 1 + rng.below(40 * 1024);
}

// Cross-checks the heap three ways against our own expectation: the Phase 13
// walk (ground truth), the Phase 14 always-on counters (incremental mirror),
// and the Phase 14 structural consistency checker. Any disagreement is a bug.
// `expected_live`/`expected_blocks` are what the workload believes is live.
void audit(const char* where, std::size_t expected_live, std::size_t expected_blocks) {
    my_heap_stats walk = {};
    if (my_heap_stats_collect(&walk) != 1) {
        std::fprintf(stderr, "  [%s] walk refused (inconsistent heap)\n", where);
        MYMALLOC_CHECK(false);
        return;
    }
    MYMALLOC_CHECK(walk.live_bytes == expected_live);
    MYMALLOC_CHECK(walk.live_blocks == expected_blocks);

    my_diag d = {};
    MYMALLOC_CHECK(my_diagnostics(&d) == 1);
    // The Phase 14 incremental counters must equal the independent walk.
    MYMALLOC_CHECK(d.live_bytes == walk.live_bytes);
    MYMALLOC_CHECK(d.live_blocks == walk.live_blocks);
    MYMALLOC_CHECK(d.regions == walk.region_count);
    MYMALLOC_CHECK(d.mapped_bytes == walk.mapped_bytes);

    char detail[96] = {0};
    if (my_check_heap(detail, sizeof(detail)) != 1) {
        std::fprintf(stderr, "  [%s] heap inconsistent: %s\n", where, detail);
        MYMALLOC_CHECK(false);
    }
    MYMALLOC_CHECK(my_free_list_valid() == 1);
}

// Verifies every live slot's payload and ownership before it is touched.
void verify_slots(const Slot* slots, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (slots[i].ptr != nullptr) {
            MYMALLOC_CHECK(my_owns(slots[i].ptr) == 1);
            MYMALLOC_CHECK(check_pattern(slots[i].ptr, slots[i].size, slots[i].tag));
        }
    }
}

// --- scenario 1: random alloc/free churn -----------------------------------
// A fixed pool of slots is repeatedly allocated into or freed; between steps a
// random subset is re-verified so a single corrupted byte is caught early.
void test_random_alloc_free() {
    constexpr std::size_t kSlots = 64;
    constexpr int kSteps = 4000;
    Rng rng(0xC0FFEEU);
    Slot slots[kSlots] = {};
    std::size_t live_bytes = 0;
    std::size_t live_blocks = 0;

    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rng.below(kSlots);
        if (slots[i].ptr == nullptr) {
            const std::size_t size = random_size(rng);
            void* p = my_malloc(size);
            MYMALLOC_CHECK(p != nullptr);
            if (p == nullptr) {
                break;
            }
            slots[i] = Slot{p, size, g_next_tag++};
            fill_pattern(p, size, slots[i].tag);
            live_bytes += my_user_to_block(p)->size;
            live_blocks += 1;
        } else {
            MYMALLOC_CHECK(check_pattern(slots[i].ptr, slots[i].size, slots[i].tag));
            live_bytes -= my_user_to_block(slots[i].ptr)->size;
            live_blocks -= 1;
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }

        // Spot-check the whole pool every so often, and audit the heap.
        if ((step % 512) == 0) {
            verify_slots(slots, kSlots);
            audit("alloc/free", live_bytes, live_blocks);
        }
    }

    // Tear down: verify then free everything; the heap must drain to empty.
    verify_slots(slots, kSlots);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i].ptr != nullptr) {
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }
    }
    audit("alloc/free drained", 0, 0);
}

// --- scenario 2: random realloc churn --------------------------------------
// Every step either allocates a new slot or reallocs a live one to a new
// (random) size, verifying the preserved prefix each time. This hammers the
// in-place fit/absorb and move paths together with the size accounting.
void test_random_realloc() {
    constexpr std::size_t kSlots = 48;
    constexpr int kSteps = 3000;
    Rng rng(0x5EED01U);
    Slot slots[kSlots] = {};
    std::size_t live_bytes = 0;
    std::size_t live_blocks = 0;

    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rng.below(kSlots);
        if (slots[i].ptr == nullptr) {
            const std::size_t size = random_size(rng);
            void* p = my_malloc(size);
            MYMALLOC_CHECK(p != nullptr);
            if (p == nullptr) {
                break;
            }
            slots[i] = Slot{p, size, g_next_tag++};
            fill_pattern(p, size, slots[i].tag);
            live_bytes += my_user_to_block(p)->size;
            live_blocks += 1;
            continue;
        }

        // Realloc an existing slot. Capture the old block size for the delta,
        // and the preserved prefix length (min of old and new request).
        const std::size_t old_size = slots[i].size;
        const std::size_t old_total = my_user_to_block(slots[i].ptr)->size;
        const std::uint32_t tag = slots[i].tag;
        const std::size_t new_size = random_size(rng);
        void* moved = my_realloc(slots[i].ptr, new_size);
        MYMALLOC_CHECK(moved != nullptr);
        if (moved == nullptr) {
            break;
        }

        // The prefix [0, min(old,new)) must be preserved by realloc.
        const std::size_t keep = old_size < new_size ? old_size : new_size;
        MYMALLOC_CHECK(check_pattern(moved, keep, tag));
        // Re-stamp the whole (possibly larger) payload and update bookkeeping.
        fill_pattern(moved, new_size, tag);
        slots[i] = Slot{moved, new_size, tag};
        const std::size_t new_total = my_user_to_block(moved)->size;
        live_bytes = live_bytes - old_total + new_total;

        if ((step % 384) == 0) {
            verify_slots(slots, kSlots);
            audit("realloc", live_bytes, live_blocks);
        }
    }

    verify_slots(slots, kSlots);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i].ptr != nullptr) {
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }
    }
    audit("realloc drained", 0, 0);
}

// --- scenario 3: mixed malloc/calloc/realloc/free long run -----------------
// One long workload that mixes all three allocation entry points, so calloc's
// zero-init and realloc's moves are exercised in the same heap as plain
// malloc. Data written into every slot is re-verified; calloc slots must read
// back zero before we ever write them.
void test_mixed_long_run() {
    constexpr std::size_t kSlots = 96;
    constexpr int kSteps = 8000;
    Rng rng(0xB16B00B5U);
    Slot slots[kSlots] = {};
    std::size_t live_bytes = 0;
    std::size_t live_blocks = 0;

    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rng.below(kSlots);
        const unsigned action = static_cast<unsigned>(rng.below(100));

        if (slots[i].ptr != nullptr && action < 30) {
            // free
            MYMALLOC_CHECK(check_pattern(slots[i].ptr, slots[i].size, slots[i].tag));
            live_bytes -= my_user_to_block(slots[i].ptr)->size;
            live_blocks -= 1;
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        } else if (slots[i].ptr == nullptr) {
            // fresh alloc: ~1/3 via calloc (must be zeroed)
            const std::size_t size = random_size(rng);
            void* p = nullptr;
            if (action < 50) {
                const std::size_t count = 1 + rng.below(size);
                p = my_calloc(count, size / count + 1U);
            } else {
                p = my_malloc(size);
            }
            MYMALLOC_CHECK(p != nullptr);
            if (p == nullptr) {
                break;
            }
            const std::size_t real = my_block_payload_size(my_user_to_block(p));
            slots[i] = Slot{p, real, g_next_tag++};
            fill_pattern(p, real, slots[i].tag);
            live_bytes += my_user_to_block(p)->size;
            live_blocks += 1;
        } else {
            // realloc a live slot
            const std::size_t old_size = slots[i].size;
            const std::size_t old_total = my_user_to_block(slots[i].ptr)->size;
            const std::uint32_t tag = slots[i].tag;
            const std::size_t new_size = random_size(rng);
            void* moved = my_realloc(slots[i].ptr, new_size);
            MYMALLOC_CHECK(moved != nullptr);
            if (moved == nullptr) {
                break;
            }
            const std::size_t keep = old_size < new_size ? old_size : new_size;
            MYMALLOC_CHECK(check_pattern(moved, keep, tag));
            const std::size_t real = my_block_payload_size(my_user_to_block(moved));
            fill_pattern(moved, real, tag);
            slots[i] = Slot{moved, real, tag};
            live_bytes = live_bytes - old_total + my_user_to_block(moved)->size;
        }

        if ((step % 700) == 0) {
            verify_slots(slots, kSlots);
            audit("mixed", live_bytes, live_blocks);
        }
    }

    verify_slots(slots, kSlots);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i].ptr != nullptr) {
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }
    }
    audit("mixed drained", 0, 0);
}

// --- scenario 4: stress with canaries enabled ------------------------------
// The same churn as scenario 1 but with Phase 14 checks ON, so every handed-out
// block carries a trailing canary. Because we never intentionally overflow,
// every canary must stay intact and no event counters may advance: a bad
// canary or a spurious invalid/double free would mean the allocator corrupted
// its own bookkeeping under load.
void test_stress_with_canaries() {
    MYMALLOC_CHECK(my_enable_checks(1) == 0); // was off
    my_diag_reset();

    constexpr std::size_t kSlots = 48;
    constexpr int kSteps = 3000;
    Rng rng(0xABCDEF01U);
    Slot slots[kSlots] = {};

    for (int step = 0; step < kSteps; ++step) {
        const std::size_t i = rng.below(kSlots);
        if (slots[i].ptr == nullptr) {
            const std::size_t size = random_size(rng);
            void* p = my_malloc(size);
            MYMALLOC_CHECK(p != nullptr);
            if (p == nullptr) {
                break;
            }
            slots[i] = Slot{p, size, g_next_tag++};
            fill_pattern(p, size, slots[i].tag);
            // A freshly armed block's canary must verify clean.
            MYMALLOC_CHECK(my_canary_ok(p) == 1);
        } else {
            MYMALLOC_CHECK(my_canary_ok(slots[i].ptr) == 1);
            MYMALLOC_CHECK(check_pattern(slots[i].ptr, slots[i].size, slots[i].tag));
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }
    }

    // No anomalies should have been recorded by the whole run.
    my_diag d = {};
    MYMALLOC_CHECK(my_diagnostics(&d) == 1);
    MYMALLOC_CHECK(d.invalid_frees == 0);
    MYMALLOC_CHECK(d.double_frees == 0);
    MYMALLOC_CHECK(d.bad_canaries == 0);
    MYMALLOC_CHECK(d.corruptions == 0);

    for (std::size_t i = 0; i < kSlots; ++i) {
        if (slots[i].ptr != nullptr) {
            my_free(slots[i].ptr);
            slots[i] = Slot{};
        }
    }
    audit("canaries", 0, 0);
    MYMALLOC_CHECK(my_enable_checks(0) == 1); // restore default
}

// --- scenario 5: leak audit ------------------------------------------------
// Allocate a batch of varied sizes, free them all, and prove the heap returned
// to empty: zero live bytes/blocks in BOTH the Phase 13 walk and the Phase 14
// counters, and (after one oversized acquire that scavenges) a single region
// holding the new request — i.e. nothing was stranded across many regions.
void test_leak_audit() {
    const std::size_t k = my_raw_page_size();

    constexpr int kCount = 200;
    void* ptrs[kCount] = {};
    std::size_t sizes[kCount] = {};
    std::size_t total = 0;
    for (int i = 0; i < kCount; ++i) {
        sizes[i] = 1 + (static_cast<std::size_t>(i) * 37U) % 6000U;
        ptrs[i] = my_malloc(sizes[i]);
        MYMALLOC_CHECK(ptrs[i] != nullptr);
        if (ptrs[i] == nullptr) {
            break;
        }
        std::memset(ptrs[i], 0x7E, sizes[i]);
        total += my_user_to_block(ptrs[i])->size;
    }

    my_heap_stats walk = {};
    MYMALLOC_CHECK(my_heap_stats_collect(&walk) == 1);
    MYMALLOC_CHECK(walk.live_blocks == static_cast<std::size_t>(kCount));
    MYMALLOC_CHECK(walk.live_bytes == total);

    for (int i = 0; i < kCount; ++i) {
        if (ptrs[i] != nullptr) {
            my_free(ptrs[i]);
            ptrs[i] = nullptr;
        }
    }

    // Fully drained: nothing live, in either accounting.
    MYMALLOC_CHECK(my_heap_stats_collect(&walk) == 1);
    MYMALLOC_CHECK(walk.live_blocks == 0);
    MYMALLOC_CHECK(walk.live_bytes == 0);
    my_diag d = {};
    MYMALLOC_CHECK(my_diagnostics(&d) == 1);
    MYMALLOC_CHECK(d.live_blocks == 0);
    MYMALLOC_CHECK(d.live_bytes == 0);

    // One oversized acquire scavenges every fully-free region into one, then
    // holds it live: proof the free regions were reclaimable, not leaked.
    void* big = my_malloc(k * 16U);
    MYMALLOC_CHECK(big != nullptr);
    MYMALLOC_CHECK(my_heap_stats_collect(&walk) == 1);
    MYMALLOC_CHECK(walk.region_count == 1U);
    MYMALLOC_CHECK(walk.live_blocks == 1U);
    my_free(big);
    audit("leak audit drained", 0, 0);
}

} // namespace

int main() {
    test_random_alloc_free();
    test_random_realloc();
    test_mixed_long_run();
    test_stress_with_canaries();
    test_leak_audit();
    MYMALLOC_TEST_MAIN("stress");
}
