// mymalloc — heap statistics (Phase 13).
//
// Implements the walk-derived snapshot declared in include/mymalloc/stats.h:
// every tracked region's physical block chain is walked and validated, and
// both fragmentation metrics fall out of the same pass. The walk is the
// authority — no counters are maintained on the hot paths, so the numbers
// cannot desynchronize from the heap even under future allocator changes.
// See docs/phase13_fragmentation.md for the definitions and design notes.
#include <mymalloc/stats.h>

#include <mymalloc/alignment.h>
#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/regions.h>

#include "debug_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// part/whole in permille, truncated, guarded against overflow (whole == 0
// yields 0). Callers keep part <= whole; the guard only matters for heaps so
// large that part * 1000 would wrap, where dividing first trades one
// permille of precision for exactness.
unsigned int ratio_permille(std::size_t part, std::size_t whole) {
    if (whole == 0) {
        return 0U;
    }
    if (part <= SIZE_MAX / 1000U) {
        return static_cast<unsigned int>((part * 1000U) / whole);
    }
    const std::size_t coarse_divisor = whole / 1000U != 0 ? whole / 1000U : 1U;
    return static_cast<unsigned int>(part / coarse_divisor);
}

// Walks ONE region's physical block chain and adds what it sees to `stats`.
// Returns 1 only when the chain is fully consistent: every block validates
// (my_block_valid), stays strictly inside the region, and the last block
// ends exactly at the region end (blocks tile a region completely — a chain
// that stops short or runs past means corruption). A bound on the block
// count makes a corrupt size unable to loop the walk forever.
int walk_region(const my_region& region, my_heap_stats* stats) {
    const auto base = reinterpret_cast<std::uintptr_t>(region.base);
    const auto limit = base + region.bytes; // real extents cannot wrap
    const std::size_t offset = my_block_user_offset();
    // At most one block per header-sized span could exist; a chain longer
    // than that is corrupt.
    const std::size_t max_blocks = region.bytes / offset + 1U;

    auto* block = reinterpret_cast<my_block_header*>(region.base);
    auto last_end = base;
    std::size_t walked = 0;
    while (block != NULL) {
        if (walked >= max_blocks || my_block_valid(block) != 1) {
            return 0;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(block);
        if (address < base || address >= limit || block->size > limit - address) {
            return 0; // block starts outside the region or runs past its end
        }
        if (stats->block_count == SIZE_MAX) {
            return 0; // defensive: counter would wrap
        }
        ++stats->block_count;
        if (block->free == 1) {
            ++stats->free_blocks;
            if (stats->free_bytes > SIZE_MAX - block->size) {
                return 0; // defensive: real extents cannot wrap
            }
            stats->free_bytes += block->size;
            if (block->size > stats->largest_free_bytes) {
                stats->largest_free_bytes = block->size;
            }
        } else {
            ++stats->live_blocks;
            if (stats->live_bytes > SIZE_MAX - block->size) {
                return 0;
            }
            stats->live_bytes += block->size;
            const std::size_t payload = my_block_payload_size(block);
            const std::size_t requested = my_block_requested(block); // clamped
            stats->requested_bytes += requested;                     // <= payload per block, so
            stats->payload_bytes += payload;                         // both sums stay consistent
        }
        last_end = address + block->size;
        block = block->next;
        ++walked;
    }
    return last_end == limit ? 1 : 0; // blocks must tile the region exactly
}

} // namespace

int my_heap_stats_collect(my_heap_stats* out) {
    if (out == NULL) {
        return 0;
    }

    allocator_lock lock;

    my_heap_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    stats.region_count = my_region_count_locked();

    for (std::size_t index = 0; index < stats.region_count; ++index) {
        my_region region = {};
        if (my_region_at_locked(index, &region) != 1) {
            return 0; // cannot happen: count and entries are one registry
        }
        if (region.bytes > SIZE_MAX - stats.mapped_bytes) {
            return 0; // defensive: tracked extents cannot wrap
        }
        stats.mapped_bytes += region.bytes;
        if (walk_region(region, &stats) != 1) {
            return 0; // inconsistency: fail without writing
        }
    }

    // Cross-check: the walk's free blocks must be exactly the listed ones.
    // (A mismatch means the free list and the heap disagree about reality.)
    if (stats.free_blocks != my_free_list_count()) {
        return 0;
    }

    // Derived fields. Safe by construction: every live block contributes
    // >= offset bytes and <= payload requested bytes, so the subtractions
    // cannot underflow (walk_region's per-block clamps guarantee it).
    const std::size_t offset = my_block_user_offset();
    stats.payload_bytes = stats.live_bytes - stats.live_blocks * offset;
    stats.internal_slack_bytes = stats.payload_bytes - stats.requested_bytes;
    stats.overhead_bytes = stats.block_count * offset;
    stats.external_frag_permille =
        ratio_permille(stats.free_bytes - stats.largest_free_bytes, stats.free_bytes);
    stats.internal_frag_permille = ratio_permille(stats.internal_slack_bytes, stats.payload_bytes);

    *out = stats;
    return 1;
}
