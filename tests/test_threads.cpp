// Thread-safety suite: concurrent allocation, reallocation, and the
// read-only introspection entry points (stats, checker, diagnostics, ownership)
// running while other threads mutate the heap.
//
// Every public entry point takes one process-wide lock, so the guarantees
// under test are: no metadata corruption, no lost/duplicated accounting, and
// no deadlock or self-deadlock when an introspection call races an allocator
// call. Each scenario joins all threads before auditing the final heap.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <mymalloc/block.h>
#include <mymalloc/debug.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/stats.h>
#include <mymalloc/strategy.h>

#include "test_framework.hpp"

namespace {

constexpr int kThreads = 4;
constexpr int kIterations = 400;

// Per-(tag, offset) byte so overlapping writes or a shifted payload are caught.
unsigned char pattern_byte(std::uint32_t tag, std::size_t offset) {
    std::uint32_t x = tag * 2654435761U;
    x += static_cast<std::uint32_t>(offset) * 2246822519U;
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

std::size_t request_size(int thread, int iteration) {
    return 1U +
           (static_cast<std::size_t>(thread) * 37U + static_cast<std::size_t>(iteration) * 13U) %
               512U;
}

// Post-join audit: the heap is quiescent, so the lock-free internal walks are
// safe to call directly. Every region must be consistent and nothing may be
// left live.
void audit_drained(const char* where) {
    my_heap_stats stats = {};
    const int collected = my_heap_stats_collect(&stats);
    MYMALLOC_CHECK(collected == 1);
    if (collected == 1) {
        MYMALLOC_CHECK(stats.live_blocks == 0);
        MYMALLOC_CHECK(stats.live_bytes == 0);
        MYMALLOC_CHECK(stats.requested_bytes == 0);
    }
    MYMALLOC_CHECK(my_free_list_valid() == 1);
    char detail[96] = {0};
    const int consistent = my_check_heap(detail, sizeof(detail));
    MYMALLOC_CHECK(consistent == 1);
    if (consistent != 1) {
        std::fprintf(stderr, "  [%s] heap inconsistent: %s\n", where, detail);
    }
}

// --- scenario 1: concurrent malloc/free ------------------------------------
void test_concurrent_malloc_free() {
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &failures] {
            for (int i = 0; i < kIterations; ++i) {
                const std::size_t size = request_size(t, i);
                void* p = my_malloc(size);
                if (p == nullptr) {
                    failures.fetch_add(1);
                    continue;
                }
                const auto tag = static_cast<std::uint32_t>(t * 100000 + i + 1);
                fill_pattern(p, size, tag);
                if (!check_pattern(p, size, tag)) {
                    failures.fetch_add(1);
                }
                my_free(p);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    MYMALLOC_CHECK(failures.load() == 0);
    audit_drained("malloc/free");
}

// --- scenario 2: concurrent realloc ----------------------------------------
void test_concurrent_realloc() {
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &failures] {
            for (int i = 0; i < kIterations; ++i) {
                const std::size_t first = request_size(t, i);
                const auto tag = static_cast<std::uint32_t>(t * 100000 + i + 1);
                void* p = my_malloc(first);
                if (p == nullptr) {
                    failures.fetch_add(1);
                    continue;
                }
                fill_pattern(p, first, tag);
                const std::size_t second = request_size(t, i + 7);
                void* moved = my_realloc(p, second);
                if (moved == nullptr) {
                    failures.fetch_add(1);
                    my_free(p);
                    continue;
                }
                const std::size_t keep = first < second ? first : second;
                if (!check_pattern(moved, keep, tag)) {
                    failures.fetch_add(1);
                }
                my_free(moved);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    MYMALLOC_CHECK(failures.load() == 0);
    audit_drained("realloc");
}

// --- scenario 3: introspection racing mutation -----------------------------
// A reader thread hammers the locked query entry points while writers churn
// the heap. The reader's own sentinel keeps my_owns on a stable address; the
// rest only reads shared state, which the lock must make safe.
void test_introspection_races_writers() {
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &failures, &stop] {
            for (int i = 0; i < kIterations && !stop.load(); ++i) {
                const std::size_t size = request_size(t, i);
                void* p = my_malloc(size);
                if (p == nullptr) {
                    failures.fetch_add(1);
                    continue;
                }
                fill_pattern(p, size, static_cast<std::uint32_t>(t + 1));
                my_free(p);
            }
        });
    }

    std::thread reader([&failures, &stop] {
        void* sentinel = my_malloc(64);
        if (sentinel == nullptr) {
            failures.fetch_add(1);
            stop.store(true);
            return;
        }
        for (int i = 0; i < kIterations; ++i) {
            my_heap_stats stats = {};
            if (my_heap_stats_collect(&stats) != 1) {
                failures.fetch_add(1);
            }
            my_diag diag = {};
            if (my_diagnostics(&diag) != 1) {
                failures.fetch_add(1);
            }
            if (my_owns(sentinel) != 1) {
                failures.fetch_add(1);
            }
            char detail[96] = {0};
            if (my_check_heap(detail, sizeof(detail)) != 1) {
                failures.fetch_add(1);
            }
            static_cast<void>(my_checks_enabled());
        }
        my_free(sentinel);
        stop.store(true);
    });

    for (auto& worker : workers) {
        worker.join();
    }
    reader.join();
    MYMALLOC_CHECK(failures.load() == 0);
    audit_drained("introspection");
}

// --- scenario 4: strategy toggling while allocating ------------------------
// The strategy accessors take the same lock as the allocator, so toggling the
// policy mid-flight must be safe and must always leave a valid value behind.
void test_strategy_toggle_races_allocations() {
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};

    std::thread toggler([&failures, &stop] {
        for (int i = 0; i < kIterations && !stop.load(); ++i) {
            my_set_strategy((i & 1) == 0 ? MY_STRATEGY_BEST_FIT : MY_STRATEGY_FIRST_FIT);
            const my_strategy current = my_get_strategy();
            if (current != MY_STRATEGY_BEST_FIT && current != MY_STRATEGY_FIRST_FIT) {
                failures.fetch_add(1);
            }
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &failures, &stop] {
            for (int i = 0; i < kIterations && !stop.load(); ++i) {
                const std::size_t size = request_size(t, i);
                void* p = my_malloc(size);
                if (p == nullptr) {
                    failures.fetch_add(1);
                    continue;
                }
                fill_pattern(p, size, static_cast<std::uint32_t>(t + 1));
                my_free(p);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }
    stop.store(true);
    toggler.join();
    MYMALLOC_CHECK(failures.load() == 0);

    my_set_strategy(MY_STRATEGY_FIRST_FIT); // restore the documented default
    MYMALLOC_CHECK(my_get_strategy() == MY_STRATEGY_FIRST_FIT);
    audit_drained("strategy");
}

} // namespace

int main() {
    test_concurrent_malloc_free();
    test_concurrent_realloc();
    test_introspection_races_writers();
    test_strategy_toggle_races_allocations();
    MYMALLOC_TEST_MAIN("threads");
}
