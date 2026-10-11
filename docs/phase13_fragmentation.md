# Phase 13 — Fragmentation: Measurement and Statistics

## Problem

Phases 5–12 built an allocator that *manages* fragmentation (splitting in
Phase 7, coalescing in Phase 8, strategy choice in Phase 9, region release in
Phase 12) — but nothing could *observe* it. A user had no way to answer:

- how much of the heap the program is actually using vs. just holding;
- how much of each allocated block is header/padding overhead;
- whether free memory is fragmented across many small holes.

Phase 13 closes that gap by adding a **read-only statistics snapshot** of the
whole heap, with two standard fragmentation metrics. It changes no allocation
behaviour — it only measures the behaviour the earlier phases already
implement.

## Definitions

Both metrics fall out of one physical walk of the heap. Every tracked region
is page-rounded and its blocks **tile it exactly** (a Phase 4 invariant), so:

```
live_bytes + free_bytes == mapped_bytes          (bytes tile the regions)
block_count      == live_blocks + free_blocks    (blocks tile the regions)
```

**Internal fragmentation** — bytes inside *allocated* blocks the caller
cannot use:

```
internal_slack_bytes = payload_bytes - requested_bytes
```

where `payload_bytes = live_bytes - live_blocks * header_offset` is every
allocated block's usable bytes and `requested_bytes` is the sum of the caller
requests recorded in the headers. The slack is exactly the per-block **header
+ alignment padding + split slack** (a block rounded up to alignment, and the
sub-`min_split_remainder` tail that Phase 7 declines to carve off). It is the
waste the program *holds* but never touches.

**External fragmentation** — *free* bytes that exist but cannot serve one
large request because they are spread over several blocks:

```
external = free_bytes - largest_free_bytes
```

`largest_free_bytes` is the biggest single free block: that much of the free
memory **can** serve any one request, so the difference is the part stranded
in holes too small (or too scattered) to use.

Both are reported **twice**: as raw byte counts and as an integer **permille**
ratio (0..1000, truncated):

```
external_frag_permille = 1000 * (free_bytes - largest_free_bytes) / free_bytes
internal_frag_permille = 1000 * internal_slack_bytes / payload_bytes
```

Permille (not percent) keeps the numbers exact and C-portable for tests: a
2 KiB hole in 8 KiB of free space is precisely 250, not 25.0. A ratio is 0
when its denominator is 0 (nothing free / nothing live).

## Design: derive-from-ground-truth

The obvious way to add statistics — bump counters on every malloc/free — was
rejected. Incremental counters **desynchronize** from the heap under exactly
the operations this allocator performs most (split, coalesce, region
scavenge): every one of those would need its own increment/decrement, and a
missed update silently corrupts every future reading. There is no hot path
that reads these numbers, so there is nothing to optimize.

Instead `my_heap_stats_collect()` **walks** the tracked regions (the Phase 12
registry) and their physical block chains and derives every field in one
pass. The walk is the authority; there are no counters to drift. As a bonus,
a heap that cannot be walked consistently is a corrupt heap, so the same walk
doubles as a coarse **consistency check**.

### The walk (src/stats.cpp)

For each region `my_region_at(index)` returns (new accessor, see below):

1. validate every block with `my_block_valid` (the Phase 4 validator, now
   extended with the Phase 13 requested-field invariant);
2. confirm the block **starts inside** the region and **does not run past its
   end** (`size <= limit - address`, overflow-safe);
3. accumulate the block into `live_*` or `free_*` and, for free blocks, track
   `largest_free_bytes`; for live blocks add `requested` and `payload`;
4. stop only at the region end — a chain that stops short or runs past is
   corruption (blocks must tile the region exactly).

A bound of `region.bytes / header_offset + 1` blocks makes a corrupt size
unable to loop the walk forever. After all regions, the walk's `free_blocks`
is **cross-checked against `my_free_list_count()`**: the physical heap and the
free list must agree on how many free blocks exist.

**The contract:** `my_heap_stats_collect` returns 1 and fills `*out` on a
consistent heap; it returns **0 and writes nothing** when `out` is NULL or any
inconsistency is found (invalid block, chain escaping or undershooting its
region, free-list disagreement). Callers can use the return value as a
heap-health check — collection is O(blocks), a diagnostic, not a hot path.

## Recording the request (block header)

Internal fragmentation needs the caller's *request*, which the Phase 4 header
did not store. Phase 13 adds one field:

```c
struct my_block_header {
    size_t size;
    my_block_header* prev;
    my_block_header* next;
    int free;
    unsigned int requested; // live request; 0 for free blocks
};
```

On LP64 `requested` lands in the padding that already followed `free`, so the
header is **still 32 bytes on x86-64** — no alignment or size change, no
Phase 4/7 test churn. It is meaningful only while the block is allocated;
free blocks ignore it (and `my_block_init`/`my_block_split` zero it on fresh
headers).

- `my_block_note_requested(block, bytes)` records the request. Requests
  `< MY_BLOCK_REQUESTED_HUGE` (2^31) are stored exactly; larger ones store the
  sentinel, because the field holds 31 usable bits. The allocator calls it on
  every path where a block becomes allocated for a known request: `my_malloc`
  (which `my_calloc` routes through, so calloc's request is `count * size`),
  and `my_realloc`'s in-place shrink and grow.
- `my_block_requested(block)` reads it back: the exact value, or
  `my_block_payload_size(block)` when the request was huge (sentinel) or the
  field is corrupt/oversized — **clamped so `payload - requested` can never
  underflow**.
- `my_block_valid` now rejects an allocated block whose recorded request does
  not fit its payload (the sentinel always fits by definition). Free blocks
  are exempt.

**Approximation, documented:** a request ≥ 2^31 bytes is stored as "exact
value unknown", and its internal fragmentation reads as 0 (payload ==
requested). Such a single allocation is a whole fresh region whose payload is
almost entirely the request, so the error is negligible; the alternative
(widening the header to a 64-bit request) would grow every block on 32-bit
targets for a case the permille metric cannot meaningfully resolve anyway.

## New region accessor

`my_region_at(index, out)` copies the registry entry at `index` into `*out`
(1 on success, 0 when `out` is NULL or out of range). Entries are packed but
`my_region_release` swap-removes, so **order is unspecified** — iteration must
not assume address order. It exists purely so diagnostics can walk every
tracked region: Phase 13's collector now, Phase 14's consistency checker next.

## API (include/mymalloc/stats.h)

```c
typedef struct my_heap_stats {
    size_t region_count, mapped_bytes;             // tracked regions
    size_t block_count, live_blocks, live_bytes;   // allocated blocks
    size_t free_blocks, free_bytes, largest_free_bytes;
    size_t requested_bytes, payload_bytes;
    size_t internal_slack_bytes, overhead_bytes;   // user view / overhead
    unsigned int external_frag_permille;           // 0..1000
    unsigned int internal_frag_permille;           // 0..1000
} my_heap_stats;

int my_heap_stats_collect(my_heap_stats* out); // 1 + filled, or 0 + untouched
```

No existing public API changed. `my_malloc`/`my_free`/`my_calloc`/`my_realloc`
behave exactly as in Phases 5–12; they merely *record* the request now.

## Tests (tests/test_fragmentation.cpp, 8 scenarios)

Every expected number is computed from the platform's real constants (header
offset, alignment, page size), so the suite holds on any LP64 target — not
just x86-64's 32/32/4096. Each snapshot is first put through
`check_invariants` (accounting adds up, ratios in range, ratios match their
definitions).

1. **empty heap + refusals** — all zeros; `collect(NULL)` refused and writes
   nothing; a *foreign* raw region is invisible (Phase 12 ownership).
2. **single allocation page math** — `my_malloc(1)` on a fresh region carries
   nearly a full page of internal slack (the Phase 5 model), requested == 1.
3. **padding sweep** — requests across alignment boundaries (1,15,31,32,33,
   63,64,...): slack is exactly `align_up(req) - req`, live/free bytes tile the
   region, free merges back to one block.
4. **fragmentation workload** — eight 512-byte blocks tile the page; freeing
   every other one strands the free space and drives the external ratio
   **750 → 600 → 333 → 142 → 0** as coalescing recovers, with `largest_free`
   tracked exactly at each step.
5. **realloc tracks the request** — shrink then grow keep `requested_bytes`
   honest through split and absorb; fully-free isolated regions read as
   external fragmentation until scavenge.
6. **scavenge updates stats** — an oversized acquire collapses free regions;
   `region_count` and `mapped_bytes` drop to just the live region.
7. **calloc accounting** — the recorded request is `count * size` (200 for
   `my_calloc(10,20)`), still zero-initialized.
8. **long mixed cycles** — 250 cycles of seven rotated mixed-size allocations:
   summed request equals summed sizes exactly, seven live blocks, and one
   final oversized request collapses the heap to a single region.

Regression: all 14 previous suites pass unchanged — recording the request
touches no observable allocation behaviour.

## Files

- `include/mymalloc/stats.h` — `my_heap_stats` + `my_heap_stats_collect`.
- `src/stats.cpp` — the validating walk and both metrics.
- `include/mymalloc/block.h`, `src/block.cpp` — `requested` field,
  `my_block_note_requested` / `my_block_requested`, the new validator rule.
- `src/allocator.cpp` — record the request on every allocation path.
- `include/mymalloc/regions.h`, `src/regions.cpp` — `my_region_at` accessor.
- `tests/test_fragmentation.cpp` — the eight scenarios above.
- `examples/fragmentation_demo.cpp` — live narration of both metrics.
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new
  sources wired in.

## Invariants

1. `live_bytes + free_bytes == mapped_bytes` and
   `block_count == live_blocks + free_blocks` for any consistent heap.
2. `requested_bytes <= payload_bytes <= live_bytes`; `internal_slack_bytes
   == payload_bytes - requested_bytes` (never underflows).
3. `largest_free_bytes <= free_bytes`; both permille ratios are in 0..1000.
4. An allocated block's recorded request fits its payload (or is the sentinel).
5. `my_heap_stats_collect` fills `*out` only on a fully consistent heap, and
   writes nothing on any refusal — its return value is a heap-health check.

## Known limitations

- **Not thread-safe** (consistent with every phase so far): a concurrent
  allocation during the walk could make a healthy heap look inconsistent.
- **Diagnostic cost.** Collection is O(blocks); it is meant for tests, demos
  and debugging, not per-allocation use.
- **External fragmentation is a lower bound**, not a prediction: it measures
  bytes stranded *now*. It does not model future request sizes, so it cannot
  distinguish "no hole is big enough for the next request" from "the next
  request happens to fit a hole". Real allocators pair it with allocation
  traces; that is beyond a single-heap snapshot.
- **Cross-region free space never coalesces** (regions are separate mappings),
  so two adjacent fully-free regions count as external fragmentation until
  scavenge-on-acquire collapses them — correct, and visible in scenario 6.
- Requests ≥ 2^31 bytes are approximated (see "Recording the request").

