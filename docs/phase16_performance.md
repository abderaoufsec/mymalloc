# Phase 16 — Performance: Benchmarking and Profiling

## Problem

Phases 5–15 built a correct, observable, stress-tested allocator. What was
missing is the *performance* picture: which paths are fast, which are slow, and
— critically — **where the time actually goes**, so any future optimization is
driven by measurement rather than guesswork. The roadmap's rule is explicit:
*"optimize only after profiling."* Phase 16 is that profiling. It adds a
benchmark harness and a strategy comparison, documents the findings, and — the
honest conclusion — changes **no code**, because the profile shows the current
design is already the right trade-off for this model.

## Method

`examples/benchmark.cpp` times the hot paths with `std::chrono::steady_clock`
(monotonic — not wall-clock, so NTP steps cannot skew it) and reports ns/op and
Mops/s. The strategy comparison runs first-fit and best-fit over a **byte-for-
byte identical** deterministic workload (fixed-seed splitmix64), so the only
variable is *which* free block each placement picks. Every benchmark drains the
heap back to a single fresh region before the next, so measurements are
independent. Numbers are machine-dependent and are *reported*, never asserted —
`tests/test_benchmark.cpp` locks down the invariants the benchmark relies on
(determinism, clean drain, no-OS-acquisition on reuse) without pinning any
absolute time.

## Findings (representative Release numbers, x86-64, page = 4096)

| path                        | ns/op   | Mops/s |
|-----------------------------|---------|--------|
| alloc+free reuse (64B)      | ~90     | ~11    |
| realloc churn (32..95B)     | ~25     | ~40    |
| free distinct (64B)         | ~930    | ~1.1   |
| fresh acquire (64B)         | ~2350   | ~0.43  |

Strategy comparison on the identical mixed workload:

| strategy  | ns/op  | ext_frag | int_frag |
|-----------|--------|----------|----------|
| first-fit | ~195   | 964/1000 | 7/1000   |
| best-fit  | ~3400  | 952/1000 | 7/1000   |

(Exact figures vary by machine and load; the *ratios* are the stable signal.)

## What the profile says

1. **OS acquisition dominates the cold path.** A fresh distinct allocation
   (~2350 ns/op) is ~26× a reuse hit (~90 ns/op). The cost is the `mmap`/region
   bookkeeping per new region, not the allocator's own logic — the reuse path,
   which is pure free-list work, is the allocator running at its real speed.

2. **`realloc` is the cheapest per-op path** (~25 ns) because the churn stays
   under the split threshold and mostly fits/shrinks in place — no OS call, no
   move, just a header tweak.

3. **Best fit is ~17× slower than first fit here** (~3400 vs ~195 ns/op) yet
   leaves essentially the *same* fragmentation (952 vs 964 permille external).
   Best fit pays a full O(n) free-list scan per allocation for a fragmentation
   benefit that this region-per-block model does not realize — free lists are
   long (many separate mmaps) but the blocks are page-granular, so "smallest
   that fits" rarely helps. This **confirms the Phase 9 default** (first fit)
   on measured evidence, not just principle.

4. **The bottleneck is structural, not micro-optimizable.** The ~26× cold/hot
   gap points at one thing: this allocator maps a fresh region for memory it
   cannot reuse. The classic fix is a **size-class segregated cache** (slabs)
   that serves small requests from pre-dug arenas and amortizes the OS call —
   exactly what production allocators (tcmalloc, jemalloc) do. That is a
   Phase-17+-scale architectural change, deliberately out of scope here.

## Decision: no code changed

Applying the roadmap rule, the profile does **not** justify changing the
allocator in this phase:

- The hot paths are already fast (tens of ns); the slow path is slow because of
  a deliberate design choice (region-per-block, page-granular), not a bug.
- Best fit's cost is proven not worth its (nonexistent, here) fragmentation
  benefit, so the first-fit default stands.
- The one change that *would* move the needle — slab/size-class caching — is a
  large architectural addition with its own correctness surface, not a tuning
  tweak. It is recorded as future work, not attempted.

So Phase 16 delivers the **harness and the evidence**, and leaves the code
untouched. A future optimization phase starts from these numbers.

## Tests (tests/test_benchmark.cpp, 4 scenarios)

The suite asserts the benchmark's *correctness preconditions*, never a
wall-clock threshold (which would flake):

1. **deterministic workload** — the same seed drives an identical decision
   stream (compared via a folded checksum); a different seed diverges. This is
   what makes the first-fit/best-fit comparison apples-to-apples.
2. **paths drain clean** — the reuse, fresh-distinct, and realloc-churn shapes
   each leave the heap consistent and fully drained in both the Phase 13 walk
   and the Phase 14 counters (no leaks, no corruption).
3. **strategy workloads drain** — a shortened mixed workload runs under both
   strategies, each leaving a clean heap, and the selector round-trips.
4. **reuse avoids OS acquisition** — cycling malloc+free of one size 20k times
   keeps the region count at exactly 1 (proving zero `mmap`/`munmap` on the hot
   path), plus a deliberately loose smoke bound against a pathological hang.

## Files

- `examples/benchmark.cpp` — the benchmark harness (hot paths + strategy
  comparison).
- `tests/test_benchmark.cpp` — the four invariant scenarios above.
- `docs/phase16_performance.md` — this document.
- `examples/CMakeLists.txt` — builds `benchmark`; `tests/CMakeLists.txt`
  registers `test_benchmark`. No library source changed.

## Invariants

1. The benchmark never asserts an absolute time; the test asserts only
   structural/deterministic facts and a generous smoke bound.
2. Every benchmark path leaves the heap consistent and fully drained.
3. The strategy comparison feeds first-fit and best-fit an identical workload.
4. The reuse path performs zero OS acquisitions (region count stays flat).
5. The benchmark restores the first-fit default and a single-region heap on
   exit.

## Known limitations

- **Numbers are not portable or stable.** They are one machine's Release-build
  figures; treat the *ratios* (cold ≫ hot, best-fit ≫ first-fit) as the signal,
  not the absolute ns/op.
- **Not a rigorous benchmark harness.** It is a single-shot, in-process timing
  loop with warm-up limited to the first iteration; it does no statistical
  outlier rejection, CPU pinning, or repetition-min. For publishable numbers a
  framework like Google Benchmark would be used; the goal here is a clear,
  self-contained profile that justifies the design, not a leaderboard entry.
- **Single size class dominates each microbenchmark.** The reuse benchmark uses
  64B; a full sweep across size classes is future work.
- **The fragmentation snapshot is a point-in-time** lower bound (Phase 13's
  definition), so the strategy comparison reflects the live set at one instant.
