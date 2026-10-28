// mymalloc Phase 11 — my_realloc experiment.
//
// realloc has two special cases and three successful outcomes:
//   * realloc(NULL, n) is malloc(n); realloc(p, 0) frees and returns NULL,
//   * FITS    — the request fits the current block (shrink, same size, or
//               grow within existing slack): SAME pointer, header-level
//               resize, worthwhile slack split off and listed (coalesced
//               through the Phase 8 path),
//   * ABSORB  — a sufficient FREE upper neighbor is merged in place:
//               SAME pointer again, data untouched,
//   * MOVE    — nothing nearby can satisfy the request: allocate fresh,
//               copy min(old payload, new size), free the old block,
//   * failure — NULL back with the original block untouched.
//
// The demo makes each outcome visible through pointer identity, block
// headers, free-list counts, and payload bytes. See docs/architecture.md.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/mymalloc.h>
#include <mymalloc/raw_memory.h>

namespace {

// True when the first `size` bytes follow the pattern byte i == (i & 0xFF).
bool pattern_ok(const void* ptr, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != static_cast<unsigned char>(i & 0xFFU)) {
            return false;
        }
    }
    return true;
}

void paint(void* ptr, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>(i & 0xFFU);
    }
}

// Builds "A allocated | B free (listed)" at the start of a raw region so the
// absorb path (which the public API cannot place on demand) can be shown.
void* craft_pair(std::size_t size_a, std::size_t size_b, my_block_header** out_b) {
    void* region = my_raw_acquire(my_raw_page_size() * 64U);
    if (region == nullptr) {
        *out_b = nullptr;
        return nullptr;
    }
    auto* block_a = reinterpret_cast<my_block_header*>(region);
    auto* block_b =
        reinterpret_cast<my_block_header*>(reinterpret_cast<unsigned char*>(region) + size_a);
    static_cast<void>(my_block_init(block_a, size_a, 0 /* allocated */, nullptr, block_b));
    static_cast<void>(my_block_init(block_b, size_b, 1 /* free */, block_a, nullptr));
    static_cast<void>(my_free_list_insert(block_b));
    *out_b = block_b;
    return my_block_to_user(block_a);
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 11: my_realloc experiment ===\n\n");

    // --- 1. the two special cases -------------------------------------------
    void* from_null = my_realloc(nullptr, 64U);
    std::printf("realloc(NULL, 64)      -> %p (malloc behaviour)  free=%d\n", from_null,
                my_user_to_block(from_null)->free);
    void* zeroed = my_realloc(from_null, 0U);
    std::printf("realloc(p, 0)          -> %s (block freed, listed once)\n",
                zeroed == nullptr ? "NULL" : "?");

    // --- 2. grow within existing slack: same pointer ------------------------
    // A fresh region is page-sized, so a mid-size grow needs no neighbor.
    void* slack = my_malloc(64U);
    paint(slack, 64U);
    void* grown = my_realloc(slack, 2048U);
    std::printf("grow 64 -> 2048:       same=%s  data=%s\n", grown == slack ? "yes" : "NO",
                pattern_ok(grown, 64U) ? "kept" : "LOST");

    // --- 3. shrink: slack is split off, coalesced and listed ----------------
    // A page-sized block shrunk to 100 bytes: same pointer, prefix kept, and
    // the leftover becomes ONE listed free block (not a sliver chain).
    void* big = my_malloc(4000U);
    paint(big, 4000U);
    void* small = my_realloc(big, 100U);
    my_block_header* small_header = my_user_to_block(small);
    std::printf("shrink 4000 -> 100:    same=%s  data=%s  block=%zu B  free_list=%zu\n",
                small == big ? "yes" : "NO", pattern_ok(small, 100U) ? "kept" : "LOST",
                small_header->size, my_free_list_count());
    my_free(small);
    my_free(grown);
    my_free_list_clear();

    // --- 4. absorb a sufficient FREE upper neighbor in place ----------------
    my_block_header* neighbor = nullptr;
    void* pair = craft_pair(1024U, 1024U, &neighbor);
    paint(pair, 992U);
    void* absorbed = my_realloc(pair, 1600U); // 1024 short, + 1024 free = enough
    my_block_header* absorbed_header = my_user_to_block(absorbed);
    std::printf("grow 1024 + free 1024: same=%s  data=%s  block=%zu B  free_list=%zu\n",
                absorbed == pair ? "yes" : "NO", pattern_ok(absorbed, 992U) ? "kept" : "LOST",
                absorbed_header->size, my_free_list_count());
    my_free_list_clear(); // the tail lives in the handcrafted region

    // --- 5. move when nothing nearby fits -----------------------------------
    // Lone page block grown to 20000: no neighbor to absorb, so realloc
    // allocates fresh, copies the overlap and frees the old block.
    void* lone = my_malloc(4000U);
    paint(lone, 4000U);
    void* moved = my_realloc(lone, 20000U);
    std::printf("grow 4000 -> 20000:    same=%s  data=%s\n",
                moved == lone ? "yes (in place)" : "no (moved)",
                pattern_ok(moved, 4000U) ? "kept" : "LOST");
    my_free(moved);

    // --- 6. failure leaves the original intact ------------------------------
    void* keep = my_malloc(64U);
    paint(keep, 64U);
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    void* failed = my_realloc(keep, max);
    my_block_header* keep_header = my_user_to_block(keep);
    std::printf("realloc(p, SIZE_MAX)   -> %s  original intact=%s (size=%zu, free=%d)\n",
                failed == nullptr ? "NULL" : "?", pattern_ok(keep, 64U) ? "yes" : "NO",
                keep_header->size, keep_header->free);
    my_free(keep);

    // --- 7. a growth round trip: every step keeps the oldest bytes ----------
    void* p = my_malloc(32U);
    paint(p, 32U);
    const std::size_t steps[] = {100U, 500U, 2000U, 64U, 3000U, 48U, 5000U};
    std::printf("\nround trip (32-byte pattern must survive every step):\n");
    for (std::size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        void* before = p;
        p = my_realloc(p, steps[i]);
        std::printf("  -> %-5zu  %-9s data=%s\n", steps[i], p == before ? "same" : "moved",
                    pattern_ok(p, 32U) ? "kept" : "LOST");
    }
    my_free(p);
    my_free_list_clear();

    std::printf("\nrealloc: special cases, in-place fits/absorb, moves, failures.\n");
    return 0;
}