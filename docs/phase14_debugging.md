# Phase 14 — Debugging: Validation, Diagnostics, and Canaries

## Problem

Phases 5–13 built a working allocator and a way to *measure* it (Phase 13),
but a user debugging a program that uses it still had to fly blind on the
classic mistakes:

- Did this pointer really come from `my_malloc`? Freeing a wild pointer is
  undefined behaviour; the allocator could only refuse it silently (Phase 5).
- Why did my free get ignored — was it a double free, or a foreign pointer?
- Did that loop overrun its buffer into the block next door?
- Is the heap itself still structurally sound, or did some stray write corrupt
  the metadata?

None of these can be answered from outside. Phase 13 could measure a *healthy*
heap but had no way to *report* a broken one by name, and nothing watched the
individual operations. Phase 14 closes that gap with a **diagnostics surface**
that makes the allocator's refusals observable and adds **opt-in overflow
canaries** — without changing how memory is placed.

## Three independent surfaces

Everything lives behind `<mymalloc/debug.h>` and splits into three pieces
that are deliberately independent, so a feature can be used alone:

### 1. Ownership + consistency (always available, no setup)

- **`my_owns(ptr)`** answers *"is this a live allocation this allocator handed
  out?"* It locates the tracked region (Phase 12), validates the block header
  (Phase 4), confirms the pointer is exactly the payload start (not an interior
  offset), and checks the block is allocated. Returns 1 only when all hold.
  This is the ownership question behind every `my_free`.
- **`my_check_heap(detail, detail_size)`** walks every tracked region and the
  free list and verifies the structural invariants — blocks validate, tile
  their region exactly, never escape a region, and the physical free-block
  count agrees with the free list. Returns 1 on a consistent heap (writing a
  short summary into `detail`) or 0, naming the first problem found. It reuses
  the same block-level rules that Phase 13's collector enforces, so a heap it
  rejects is a heap `my_heap_stats_collect` would also reject.

### 2. A counters snapshot (peaks always maintained)

`my_diag` is a by-value struct of two flavours driven by their maintenance
cost (a few adds/compares per op, never a walk):

- **Always-on usage accounting** — `live_bytes`/`live_blocks`/`mapped_bytes`/
  `regions` and their **peaks**. The allocator feeds block- and region-size
  **deltas** through internal hooks after every successful operation
  (`my_dbg_note_alloc` / `_free` / `_resize` / `_region_add` / `_region_remove`),
  so the totals mirror the heap exactly. They cross-check 1:1 against Phase
  13's independent walk (the test suite proves it), which is why they are
  incremental and not walked here: the walk is the authority, the deltas are
  the cheap mirror, and a mismatch is itself a bug detector.
- **Event counters** — `invalid_frees`, `double_frees`, `corruptions`,
  `bad_canaries`, `foreign_pointers`. These advance **only while checks are
  enabled** (below); with checks off they are never touched, so event
  bookkeeping is absent from the default path.

`my_diagnostics(&out)` fills the snapshot cheaply (reads maintained state);
`my_diag_reset()` zeroes the events and re-baselines the peaks to "now" so a
peak is never below current usage.

### 3. Opt-in safety checks (OFF by default)

`my_enable_checks(1)` switches on the per-allocation **canaries** and makes
the event counters and the verbose hook live. With checks **off** — the
default — the allocator reserves nothing and stamps nothing, so release builds

## Design decisions

**Release builds are unchanged.** Turning checks off reserves `0` extra bytes
and stamps nothing; the layout, block sizes, and speed match Phases 5–13
exactly. Turning them on is a genuine debug-only trade (one alignment unit per
allocation, plus registry bookkeeping), which is why it is opt-in rather than
always-on like the usage accounting.

**Presence is tracked explicitly, not inferred from geometry.** A canary is
recorded in a debug-only **registry** of armed block pointers, not deduced
from the block's size. A page-rounded fresh region leaves large payload slack
whether or not checks are on, so geometry alone cannot distinguish an armed
block from a plain one — and would misread a checks-off block after a toggle.
The registry is a linear-search set backed by **raw memory** (`my_raw_acquire`,
never `my_malloc`, so allocator metadata never recurses into the allocator) and
is **cleared whenever checks are turned off**, so it only ever holds live armed
blocks and never outlives the arming that populated it.

**"No canary" is not a failure internally.** `my_dbg_canary_verify` treats an
unarmed block as *nothing to check* (returns 1), so a checked free of a block
allocated with checks off does not report corruption. The public `my_canary_ok`
layers the stricter *"0 when there is no canary at all"* rule on top via an
`is_armed` predicate — a query and a free report differently by design.

**Diagnostics never allocate on a failing path and never abort.** They report
and let the caller decide. A `detail` buffer is always bounded and
NUL-terminated.

**Not thread-safe**, consistent with every phase: concurrent mutation during a
walk or a counter bump could misreport.

## API (include/mymalloc/debug.h)

```c
// Always available
int  my_check_heap(char* detail, size_t detail_size); // 1 consistent, 0 + reason
int  my_owns(const void* ptr);                        // 1 live allocation of mine

typedef struct my_diag {
    size_t peak_live_bytes, peak_live_blocks, peak_mapped_bytes, peak_regions;
    size_t live_bytes, live_blocks, mapped_bytes, regions;      // current mirror
    unsigned long invalid_frees, double_frees, corruptions,
                  bad_canaries, foreign_pointers;               // events
} my_diag;
int  my_diagnostics(my_diag* out);                   // 1 + filled, 0 when out NULL
void my_diag_reset(void);


## Tests (tests/test_debug.cpp, 7 scenarios)

Like Phase 13, every expected number is computed from the platform's real
constants so the suite holds on any LP64 target. Each test is its own process,
so the global accounting starts from a known baseline.

1. **default state** — checks off by default; `my_canary_ok` is 0 (no canary);
   ownership works with checks off; a consistent heap reports success with a
   summary naming the region count.
2. **accounting mirrors the walk** — six mixed allocations are cross-checked
   field-for-field against `my_heap_stats_collect` (live/regions/mapped);
   freeing keeps peaks immutable while dropping totals; a `my_realloc` keeps
   the mirror in step; a fully-drained heap reads 0 and still matches.
3. **invalid & double free** — a foreign `my_free` is one invalid free plus one
   hook event; a second `my_free` of a live pointer is a separate double-free
   that leaves the heap consistent; `my_free(nullptr)` is a no-op, not an event.
4. **canaries catch an overflow** — a small request verifies clean; a one-byte
   flip at `user + requested` fails the canary and bumps `bad_canaries`; a
   checked free reports the broken canary then frees normally; a foreign/NULL
   pointer has no canary to check.
5. **corruption detection** — hand-corrupting a block header's size makes
   `my_check_heap` fail, name a problem, fire the hook, and count a corruption;
   restoring the size makes it consistent again (proving the failure was real).
6. **bounded detail buffer** — a success summary truncates to fit but stays
   NUL-terminated; a zero-size buffer (or NULL) writes nothing and does not
   overrun.
7. **toggling checks** — a checks-off allocation has no canary; enable→disable
   clears the registry so an armed block verifies while enabled and its later
   free trips no stale canary.

Regression: all previous suites pass unchanged — with checks off, no allocation
behaviour, block size, or observable statistic differs from Phases 5–13.

## Files

- `include/mymalloc/debug.h` — the public diagnostics surface.
- `src/debug.cpp` — ownership + consistency, always-on accounting, canaries.
- `src/debug_internal.h` — the narrow internal hooks (accounting deltas +
  canary arm/disarm/verify/re-stamp), included only by library TUs.
- `src/allocator.cpp` — feed the accounting deltas, reserve/arm/re-stamp
  canaries, report invalid/double frees on the `my_free` path.
- `src/regions.cpp` — feed the mapped-bytes/region deltas on track/release.
- `tests/test_debug.cpp` — the seven scenarios above.
- `examples/debug_demo.cpp` — live narration of all three surfaces.
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new
  sources wired in.

## Invariants

1. `live_bytes`/`live_blocks`/`mapped_bytes`/`regions` always equal the
   independent Phase 13 walk (same block `size` totals and region extents).
2. Peaks are monotone and never below current usage; `my_diag_reset` re-baselines
   them to "now".
3. With checks off, `my_malloc`/`my_realloc` reserve `0` extra bytes — block
   layout and sizes are identical to Phases 5–13.
4. A canary occupies `user + requested` and stays inside the reserved payload
   (`requested + word <= align_up(requested) + reserve`); a request too large to
   arm is recorded, not forced.
5. The armed registry holds only currently-live armed blocks and is cleared on
   disable; it is backed by raw memory, never `my_malloc`.
6. Event counters and the hook advance only while checks are enabled;
   `my_check_heap`/`my_owns` query results are not events.
7. `my_diagnostics` fills `*out` only when `out` is non-NULL; `my_check_heap`
   always leaves `detail` NUL-terminated when `detail_size > 0`.

## Known limitations

- **Not thread-safe** (consistent with every phase): a concurrent allocation
  during `my_check_heap` can make a healthy heap look inconsistent.
- **Canaries are a trailing guard, not a leading one.** They catch an overflow
  that runs *past the end* of a request; a write *before* the block or into an
  already-free neighbour's header is a separate corruption caught by the header
  validator, not the canary.
- **Canary check is O(armed blocks).** The registry is a linear-search set,
  acceptable for a debug-only facility.
- **HUGE requests** (≥ 2^31 bytes, stored as the sentinel) have no reliable
  canary position and are left unarmed; such a request is a whole fresh region
  and canary-guarding it is not meaningful.
- **Event counters only count what checks are enabled for.** A double free with
  checks off is still *refused* safely (Phase 6) but is not *counted*; this is
  the deliberate cost/benefit split that keeps the default path pristine.

// Opt-in checks (off by default); returns the previous state
int  my_enable_checks(int enable);
int  my_checks_enabled(void);
int  my_canary_ok(void* ptr);                        // 1 intact canary, else 0

typedef void (*my_diag_hook)(const char* event, void* ptr);
void my_set_diag_hook(my_diag_hook hook);            // NULL clears
```

No existing public API changed. `my_malloc`/`my_free`/`my_calloc`/`my_realloc`
behave exactly as in Phases 5–13 when checks are off; the narrow coupling lives
in the internal `src/debug_internal.h` (included only by the library's own TUs).

keep the exact Phase 5–13 block sizes and behaviour byte-for-byte.

- **Canaries** — `my_malloc`/`my_realloc` reserve one extra trailing alignment
  unit (16 on x86-64; 0 when checks are off) and `my_canary_ok(ptr)` verifies a
  magic word stamped just past the request. The word sits **immediately after
  the requested bytes** (`user + requested`), so *any* overflow past the
  request — even a single byte — corrupts it; there is no padding blind spot.
- **Free-path detection** — `my_free` already refuses foreign and double frees
  silently (Phases 5–6). With checks on it now *reports* each as an event and,
  while the block is still intact, verifies its canary (a checked free of an
  overflowed block is caught here too).
- **Verbose hook** — `my_set_diag_hook(fn)` installs an optional sink called
  for every detected anomaly while checks are enabled: `"invalid free"`,
  `"double free"`, `"heap corruption"`, `"bad canary"`. Ordinary query results
  that simply return 0 are **not** events.
