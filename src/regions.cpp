// mymalloc — managed region registry (Phase 12).
//
// Implements the {base, bytes} registry and the guarded release described in
// include/mymalloc/regions.h: growable table (my_raw_acquire, never
// my_malloc), exact-membership lookups, and a structural "fully free" check
// that must pass before any unmapping happens. See
// docs/phase12_heap_management.md for the ownership model and the
// scavenge-on-acquire policy this module serves.
#include <mymalloc/regions.h>

#include <mymalloc/block.h>
#include <mymalloc/free_list.h>
#include <mymalloc/raw_memory.h>

#include <cstdint>
#include <cstring>

namespace {

// Registry storage: entries live in a raw-acquired buffer that doubles in
// size as regions are tracked. g_count entries are live; g_capacity is the
// buffer's entry capacity (0 until the first track).
my_region* g_regions = nullptr;
std::size_t g_count = 0;
std::size_t g_capacity = 0;

// First buffer size: 64 entries (1 KiB on LP64) — small enough to stay
// cheap for one-region programs, large enough that growth is rare.
constexpr std::size_t kInitialCapacity = 64U;

// True when the region is exactly one FREE block spanning the whole extent:
// the block starts at the region base (my_block_valid confirms the pointer
// and size), is marked free, has no physical neighbors (it is the region's
// only block) and covers every tracked byte. Any live allocation anywhere in
// the region breaks one of these conditions, which is what makes releasing
// it impossible.
bool region_is_fully_free(const my_region& region) {
    const auto* block = reinterpret_cast<const my_block_header*>(region.base);
    return my_block_valid(block) == 1 && block->free == 1 && block->prev == NULL &&
           block->next == NULL && block->size == region.bytes;
}

// Grows the table so it holds at least `min_capacity` entries (roughly
// doubling). The new buffer comes from my_raw_acquire — allocator metadata
// must never recurse into my_malloc. Returns 1 on success, 0 when the OS
// refuses the new buffer (the old table stays intact and usable).
int grow_table(std::size_t min_capacity) {
    std::size_t new_capacity = g_capacity != 0 ? g_capacity : kInitialCapacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > SIZE_MAX / 2U) {
            return 0; // doubling would wrap
        }
        new_capacity *= 2U;
    }
    if (new_capacity > SIZE_MAX / sizeof(my_region)) {
        return 0; // byte size would wrap
    }

    void* buffer = my_raw_acquire(new_capacity * sizeof(my_region));
    if (buffer == NULL) {
        return 0;
    }
    std::memcpy(buffer, g_regions, g_count * sizeof(my_region)); // entries are trivially copyable
    if (g_regions != NULL) {
        my_raw_release(g_regions, g_capacity * sizeof(my_region));
    }
    g_regions = static_cast<my_region*>(buffer);
    g_capacity = new_capacity;
    return 1;
}

// Index of the entry whose base is exactly `base`, or SIZE_MAX.
std::size_t find_exact(const void* base) {
    for (std::size_t index = 0; index < g_count; ++index) {
        if (g_regions[index].base == base) {
            return index;
        }
    }
    return SIZE_MAX;
}

// Unmaps the fully-free region at slot `index` and removes the entry by
// swapping in the last one. Returns 1 when the region was released.
int release_at(std::size_t index) {
    my_region region = g_regions[index];

    // The block must leave the free list BEFORE its memory is unmapped, or
    // the list would dangle into released pages. Removal is a no-op when the
    // block was never listed (or was unlisted by my_free_list_clear).
    auto* block = reinterpret_cast<my_block_header*>(region.base);
    static_cast<void>(my_free_list_remove(block));

    g_regions[index] = g_regions[g_count - 1U]; // swap-remove keeps entries packed
    --g_count;

    my_raw_release(region.base, region.bytes);
    return 1;
}

} // namespace

int my_region_track(void* base, size_t bytes) {
    if (base == NULL || bytes == 0) {
        return 0;
    }
    // Tracked extents are always whole pages: my_malloc rounds its region
    // size up before acquiring, and my_raw_release rounds identically, so a
    // non-page multiple here would signal a bookkeeping bug.
    if ((bytes % my_raw_page_size()) != 0) {
        return 0;
    }
    // Double-tracking the same base would let it be released twice; refuse.
    if (find_exact(base) != SIZE_MAX) {
        return 0;
    }

    if (g_count == g_capacity && grow_table(g_count + 1U) != 1) {
        return 0;
    }

    g_regions[g_count] = my_region{base, bytes};
    ++g_count;
    return 1;
}

int my_region_find(const void* addr, my_region* out) {
    if (addr == NULL || out == NULL) {
        return 0;
    }
    const auto target = reinterpret_cast<std::uintptr_t>(addr);
    for (std::size_t index = 0; index < g_count; ++index) {
        const auto base = reinterpret_cast<std::uintptr_t>(g_regions[index].base);
        if (target >= base && target - base < g_regions[index].bytes) {
            *out = g_regions[index]; // copy: the table may move on growth
            return 1;
        }
    }
    return 0;
}

size_t my_region_count(void) {
    return g_count;
}

int my_region_release(void* base, size_t bytes) {
    // Guardrail 1: only an EXACTLY tracked base may be released — a pointer
    // from the middle of a region, or from a foreign raw mapping, is
    // refused without being dereferenced.
    const std::size_t index = find_exact(base);
    if (index == SIZE_MAX) {
        return 0;
    }
    // Guardrail 2: the size must match the tracked extent so the unmap can
    // never cover more (over-unmap) or less (leak) than the region.
    if (g_regions[index].bytes != bytes) {
        return 0;
    }
    // Guardrail 3: the region must be fully free — no live allocation may
    // reside in it.
    if (!region_is_fully_free(g_regions[index])) {
        return 0;
    }
    // Guardrail 4 (unlist-before-unmap) happens inside release_at.
    return release_at(index);
}

size_t my_regions_release_free(void) {
    std::size_t released = 0;
    // Swap-remove means the entry after a release slides into the current
    // slot: do not advance the index in that case.
    std::size_t index = 0;
    while (index < g_count) {
        if (region_is_fully_free(g_regions[index])) {
            static_cast<void>(release_at(index));
            ++released;
        } else {
            ++index;
        }
    }
    return released;
}