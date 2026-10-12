# Phase 15 — Stress Testing

## Problem

Phases 5–14 built the allocator (`malloc`/`free`/`calloc`/`realloc`), its
management (splitting, coalescing, strategies, region reclamation), and its
observability (statistics, diagnostics, canaries). Each phase tested its own
behaviour in isolation with hand-picked scenarios. What was missing is the
**integration guarantee**: under long, randomized, mixed load — the way real
programs actually use an allocator — does the heap stay *correct*, *accounted*,
and *leak-free* all at once?

A unit test that allocates three blocks proves the happy path. It does not
prove that ten thousand interleaved allocations, frees, and reallocs across
thousands of size classes never:

- hand two live allocations the *same* bytes (an overlap),
- corrupt a neighbour's payload (a stray write past a request),
- leave the incremental statistics drifting from the real heap,
- strand memory in regions that should have been reclaimed (a leak),
- trip a spurious canary or a phantom double free under its own bookkeeping.

Phase 15 closes that gap with a **deterministic stress suite**. It adds no new
allocator behaviour; it *hammers* everything built so far and leans on the
Phase 13 walk (`my_heap_stats_collect`) and the Phase 14 diagnostics
(`my_diagnostics` / `my_check_heap` / `my_owns`) as independent oracles.

## Design: deterministic randomized load

The whole suite is **reproducible by construction**, because a stress failure
you cannot reproduce is a stress failure you cannot fix:

- **Self-contained PRNG.** A fixed-seed **splitmix64** generator drives every
  choice (which slot, which action, which size). It is hand-rolled rather than
  `std::mt19937`/`std::uniform_int_distribution` so the sequence is identical
  across compilers, standard libraries, and platforms — the same seed yields
  the same workload everywhere, so a failure reproduces bit-for-bit.
- **Platform-derived expectations.** Nothing is hard-coded to x86-64. Expected
  live bytes come from the real `my_user_to_block(p)->size`; the leak audit's
  "oversized" request is `mapped_bytes + page_size` computed at runtime; region
  and block counts come from the Phase 13 walk. The suite holds on any LP64
  target.
- **No wall-clock or environment dependence.** No timeouts, no threads, no
  `/proc` — just arithmetic on a fixed seed.

### Data-integrity patterns

Every live allocation is stamped with a **per-byte pattern** derived from *both*
a unique per-allocation `tag` and the byte offset:

```
pattern_byte(tag, offset) = hash(tag, offset) & 0xFF
```

Because the value depends on the tag, two different live blocks have different
patterns even at the same offset — so if the allocator ever overlapped them,
the clobbered bytes would mismatch on the next verify. Because it depends on
the offset, a one-byte shift is caught too. Each slot is re-verified before it
is freed or realloc'd, and the whole pool is re-verified periodically, so a
single corrupted byte surfaces within a few hundred steps, not at teardown.

### Three-way accounting cross-check

The `audit()` helper proves the heap is correct *and* consistently measured at
every checkpoint by comparing three independent views:

1. the **Phase 13 walk** (`my_heap_stats_collect`) — the ground truth, derived
   by physically walking the regions;
2. the **Phase 14 incremental counters** (`my_diagnostics`) — the always-on
   mirror fed by allocator deltas;
3. the workload's **own tally** of live bytes/blocks it believes it holds.

All three must agree (`live_bytes`, `live_blocks`, `regions`, `mapped_bytes`),
and `my_check_heap` + `my_free_list_valid` must both pass. A drift between the
walk and the counters is itself a bug — exactly the desync Phase 13's

## Tests (tests/test_stress.cpp, 5 scenarios)

Each scenario drains to an empty heap and ends with `audit(..., 0, 0)`, so a
later scenario always starts from a known-zero live state (peaks persist, but
live totals reset).

1. **random alloc/free churn** — 4000 steps over 64 slots of mixed sizes
   (tiny → multi-page); the whole pool is verified and audited every 512 steps.
2. **random realloc churn** — 3000 steps that grow/shrink live slots in place
   and by move, verifying the preserved `min(old,new)` prefix every time and
   keeping the byte accounting exact through each resize.
3. **mixed long run** — 8000 steps mixing `my_malloc`, `my_calloc`, `my_realloc`
   and `my_free` over 96 slots, so calloc's zero-init and realloc's moves share
   one heap with plain malloc under sustained load.
4. **stress with canaries** — the same churn with Phase 14 checks ON; since the
   workload never intentionally overflows, every canary must stay intact and
   `invalid_frees`/`double_frees`/`bad_canaries`/`corruptions` must all remain
   **0** — a non-zero count would mean the allocator corrupted its own
   bookkeeping under load.
5. **leak audit** — 200 varied allocations are filled and freed; the heap must
   read **0 live blocks/bytes** in both the walk and the counters, and one
   request larger than the entire mapped footprint must scavenge every
   fully-free region down to a **single** region — proof nothing was stranded.

The suite runs ~30k assertions in ~150 ms on a debug build.

## Files

- `tests/test_stress.cpp` — the five scenarios, the splitmix64 PRNG, the
  pattern helpers, and the three-way `audit`.
- `examples/stress_demo.cpp` — a narrated, shorter run of the same idea,
  printing the live/mapped/region picture and the final drain-to-zero.
- `CMakeLists.txt` is unchanged (no new library source); `tests/CMakeLists.txt`
  registers `test_stress`, `examples/CMakeLists.txt` builds `stress_demo`.

## Invariants

1. The run is **deterministic**: a fixed seed produces an identical sequence on
   every platform, so any failure is exactly reproducible.
2. At every audit point the Phase 13 walk, the Phase 14 counters, and the
   workload's own tally agree on `live_bytes`, `live_blocks`, `regions`, and
   `mapped_bytes`.
3. No two live allocations ever share bytes: each carries a unique (tag, offset)
   pattern that is re-verified before it is freed or realloc'd.
4. Every scenario ends with the heap fully drained (0 live blocks/bytes).
5. With canaries enabled and no intentional overflow, no event counter advances.
6. Fully-freed regions are reclaimable: an oversized acquire scavenges them to
   a single region (no leak).

## Known limitations

- **Not a substitute for sanitizers.** This suite proves the *allocator's* logic
  stays self-consistent; it does not prove the *test's own* accesses are in
  bounds the way ASan/UBSan do. Run the suite (and the whole project) under the
  `asan`/`ubsan` presets in CI for that layer — see the README.
- **Deterministic, so not exhaustive.** A fixed seed explores one path through
  the state space. It is chosen to hit tiny/small/large sizes, in-place and
  move reallocs, and multi-region growth, but it is a sample, not a proof.
- **Not thread-safe** (consistent with every phase): the stress runs
  single-threaded, so it exercises correctness under concurrency zero.
- **Bounded memory.** Slot counts and the size distribution cap live memory in
  the low tens of MB; this is a correctness stress, not a capacity/throughput
  benchmark (that is Phase 16).
