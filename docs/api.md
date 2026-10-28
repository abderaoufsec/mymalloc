# mymalloc — API Reference

This is the consolidated reference for every **public** header. The library is
C-compatible: all functions use C linkage and plain C types (`size_t`,
`void*`, `NULL`), so it links from both C and C++. Nothing here allocates on a
failing path, and (except where noted) a rejected call changes no state.

**Thread-safety:** the allocator, statistics, debug, region-registry and
strategy entry points share one process-wide mutex. Each public call takes it
exactly once, so they may be called concurrently and may race allocation
safely. The lock is coarse-grained (calls serialize) and non-recursive:
diagnostic hooks must not call back into the allocator. The low-level
`block.h`, `alignment.h` and `free_list.h` helpers are unsynchronized internal
building blocks; use them only while holding the lock or single-threaded.

**Headers** (all under `include/mymalloc/`):

| Header         | Area    | Purpose                                        |
|----------------|---------|------------------------------------------------|
| `mymalloc.h`   | Core    | Core `malloc`/`free`/`calloc`/`realloc` + version |
| `raw_memory.h` | OS      | Raw OS page acquisition/release                |
| `alignment.h`  | Base    | Overflow-safe alignment helpers                |
| `block.h`      | Meta    | Block header, conversions, validation          |
| `free_list.h`  | Reuse   | Intrusive free list                            |
| `strategy.h`   | Policy  | First-fit / best-fit selector                  |
| `regions.h`    | Heap    | Managed region registry                        |
| `stats.h`      | Stats   | Heap statistics / fragmentation                |
| `debug.h`      | Debug   | Diagnostics, ownership, canaries               |

---

## `<mymalloc/mymalloc.h>` — core allocator

```c
void* my_malloc(std::size_t size);
void  my_free(void* ptr);
void* my_calloc(std::size_t count, std::size_t size);
void* my_realloc(void* ptr, std::size_t new_size);
const char*   mymalloc_version(void);        // "MAJOR.MINOR.PATCH"
unsigned int  mymalloc_version_number(void); // MAJOR*10000 + MINOR*100 + PATCH
```

- **`my_malloc(size)`** — returns a pointer to `size` usable bytes, aligned to
  `my_default_alignment()` (16 on x86-64), or `NULL` when `size == 0` or the
  request cannot be satisfied. Memory is **not** zeroed (use `my_calloc`).
- **`my_free(ptr)`** — returns a block to the allocator; coalesces with free
  physical neighbours. `my_free(NULL)` is a no-op. A pointer this allocator did
  not hand out, or an already-free pointer, is refused silently (loud reporting
  is opt-in via `<mymalloc/debug.h>`).
- **`my_calloc(count, size)`** — zero-initialized array of `count` elements;
  returns `NULL` when `count * size` overflows or either argument is 0.
- **`my_realloc(ptr, new_size)`** — resizes in place when it fits or can absorb
  a free upper neighbour, otherwise moves and copies the overlapping prefix.
  `my_realloc(NULL, n)` == `my_malloc(n)`; `my_realloc(p, 0)` frees and returns
  `NULL`. On allocation failure returns `NULL` and leaves the original block
  untouched.

---

## `<mymalloc/raw_memory.h>` — raw OS memory

```c
size_t my_raw_page_size(void);
void*  my_raw_acquire(std::size_t bytes);
void   my_raw_release(void* ptr, std::size_t bytes);
```

- **`my_raw_page_size()`** — the OS page size (queried once, cached).
- **`my_raw_acquire(bytes)`** — maps at least `bytes`, rounded up to whole
  pages; returns page-aligned, zero-filled, writable memory, or `NULL` when
  `bytes == 0`, rounding would overflow, or the OS refuses.
- **`my_raw_release(ptr, bytes)`** — unmaps a region previously acquired;
  `bytes` must match the original request (it is rounded identically). No-op
  when `ptr` is `NULL`.

---

## `<mymalloc/alignment.h>` — alignment helpers

```c
int    my_is_power_of_two(std::size_t value);
size_t my_default_alignment(void);
int    my_align_up_size(std::size_t value, std::size_t alignment, std::size_t* out);
int    my_align_up_address(std::uintptr_t address, std::size_t alignment, std::uintptr_t* out);
```

- **`my_is_power_of_two(value)`** — 1 when `value` is a non-zero power of two.
- **`my_default_alignment()`** — the guaranteed alignment (a power of two;
  `alignof(max_align_t)` = 16 on x86-64).
- **`my_align_up_size` / `my_align_up_address`** — round up to the next multiple
  of `alignment`. Return 1 and write through `out` on success; return 0 (leaving
  `*out` untouched) when `out` is `NULL`, `alignment` is not a power of two, or
  the rounding would overflow.

---

## `<mymalloc/block.h>` — block metadata

Every block begins with a header immediately followed by its payload:

```text
[ size | prev | next | free | requested ][ payload ... ]
<---------------- size ---------------->   (header + payload)
```

```c
typedef struct my_block_header {
    size_t size;            // total bytes incl. header; multiple of alignment
    my_block_header* prev;  // physical neighbour at a lower address, or NULL
    my_block_header* next;  // physical neighbour at a higher address, or NULL
    int free;               // 1 = free, 0 = allocated
    unsigned int requested; // live request; 0 for free blocks
} my_block_header;

#define MY_BLOCK_REQUESTED_HUGE 0x80000000u   // "exact request unknown" sentinel

size_t          my_block_user_offset(void);
void*           my_block_to_user(my_block_header* block);
my_block_header* my_user_to_block(void* user);
size_t          my_block_payload_size(const my_block_header* block);
int             my_block_init(my_block_header* base, size_t size, int is_free,
                             my_block_header* prev, my_block_header* next);
void            my_block_note_requested(my_block_header* block, size_t bytes);
size_t          my_block_requested(const my_block_header* block);
int             my_block_split(my_block_header* block, size_t front_size);
int             my_block_merge(my_block_header* lower, my_block_header* upper);
my_block_header* my_block_prev(const my_block_header* block);
my_block_header* my_block_next(const my_block_header* block);
int             my_block_is_free(const my_block_header* block);
int             my_block_valid(const my_block_header* block);
```

- **`my_block_user_offset()`** — bytes from a block start to its user pointer
  (`align_up(sizeof(header), default_alignment)`; 32 on x86-64).
- **`my_block_to_user` / `my_user_to_block`** — exact inverses, NULL-safe.
- **`my_block_payload_size(block)`** — usable bytes at the user pointer
  (`size - user_offset`). Precondition: `block` is a trusted/readable header.
- **`my_block_init(...)`** — validated header initialization; returns 0 when the
  base is NULL/misaligned, `size` is below the offset or not an alignment
  multiple, or `is_free` ∉ {0,1}.
- **`my_block_note_requested` / `my_block_requested`** — record/read the live
  request for fragmentation statistics. Requests below `MY_BLOCK_REQUESTED_HUGE`
  (2³¹) are stored exactly; larger ones store the sentinel. `my_block_requested`
  is clamped so `payload - requested` never underflows.
- **`my_block_split(block, front_size)`** — carve a front of `front_size` and a
  remainder tail; repairs the physical chain. Returns 0 on any invalid argument
  or an unusable remainder.
- **`my_block_merge(lower, upper)`** — fuse two linked, address-ordered physical
  neighbours into `lower`; a pure physical operation (free-list membership is
  the caller's job). Returns 0 when they are not linked neighbours.
- **`my_block_prev` / `my_block_next` / `my_block_is_free`** — NULL-safe
  accessors. Precondition: trusted/readable header.
- **`my_block_valid(block)`** — verifies every metadata invariant; rejects
  misaligned/NULL pointers **without dereferencing** them. Also rejects an
  allocated block whose recorded request does not fit its payload.

---

## `<mymalloc/free_list.h>` — free list

An intrusive, address-ascending doubly linked list whose links live in the
payload of each free block (allocated blocks pay zero extra bytes).

```c
typedef struct my_free_links {
    struct my_free_links* prev; // previous entry in address order, or NULL
    struct my_free_links* next; // next entry in address order, or NULL
} my_free_links;

my_free_links* my_free_list_links(my_block_header* block);
int            my_free_list_insert(my_block_header* block);
int            my_free_list_remove(my_block_header* block);
my_block_header* my_free_list_first_fit(size_t min_block_size);
my_block_header* my_free_list_best_fit(size_t min_block_size);
my_block_header* my_free_list_head(void);
size_t          my_free_list_count(void);
int             my_free_list_valid(void);
void            my_free_list_clear(void);
```

- **`my_free_list_insert(block)`** — insert in ascending address order; returns
  0 when the block is NULL/invalid/not free/too small for the links or is
  **already listed** (double-insert guard).
- **`my_free_list_remove(block)`** — remove and clear its links; 0 when not a
  member. Does not change free state.
- **`my_free_list_first_fit(min)`** — lowest-address free block with total size
  ≥ `min`, else `NULL`. `min == 0` returns `NULL`.
- **`my_free_list_best_fit(min)`** — smallest such block, ties broken by lowest
  address (full O(n) scan). `min == 0` returns `NULL`.
- **`my_free_list_head()` / `my_free_list_count()`** — head (lowest address) and
  the exact member count.
- **`my_free_list_valid()`** — walks the list verifying every invariant
  (valid free blocks, strictly ascending order, reciprocal links, exact count);
  1 when consistent.
- **`my_free_list_clear()`** — drop the list without touching blocks (test
  cleanup); fully-free tracked regions are still reclaimable by the allocator.

---

## `<mymalloc/strategy.h>` — placement strategy

```c
enum my_strategy {
    MY_STRATEGY_FIRST_FIT = 0, // lowest-address block that fits (default)
    MY_STRATEGY_BEST_FIT  = 1  // smallest block that fits (least leftover)
};
void            my_set_strategy(enum my_strategy strategy);
enum my_strategy my_get_strategy(void);
```

- **`my_set_strategy`** — set the process-wide placement strategy. Unknown
  values are ignored (the current strategy is kept), so a bogus cast cannot
  silently disable reuse.
- **`my_get_strategy`** — the current strategy. The default is first fit; best
  fit's whole-list scan is not worth it for this region-per-block model (see
  `docs/architecture.md`).

---

## `<mymalloc/regions.h>` — region registry

```c
typedef struct my_region { void* base; size_t bytes; } my_region;

int    my_region_track(void* base, size_t bytes);
int    my_region_find(const void* addr, my_region* out);
size_t my_region_count(void);
int    my_region_at(size_t index, my_region* out);
int    my_region_release(void* base, size_t bytes);
size_t my_regions_release_free(void);
```

- **`my_region_track(base, bytes)`** — start tracking `[base, base+bytes)`;
  returns 0 when `base` is NULL, `bytes` is 0 or not a whole number of pages,
  the region is already tracked, or the internal table cannot grow.
- **`my_region_find(addr, out)`** — copy the tracked region containing `addr`
  into `*out`; 0 when none contains it (or `out` is NULL).
- **`my_region_count()`** — the exact number of tracked regions.
- **`my_region_at(index, out)`** — copy the entry at `index`; 0 when out of
  range. Order is unspecified (swap-remove on release) — do not assume address
  order when iterating.
- **`my_region_release(base, bytes)`** — unmap the region starting at `base`,
  provided `bytes` matches the tracked extent **and** the region is fully free;
  a refused call unmaps nothing and changes nothing.
- **`my_regions_release_free()`** — release every fully-free region; returns the
  number released. Used by `my_malloc` scavenge-on-acquire.

---

## `<mymalloc/stats.h>` — heap statistics

```c
typedef struct my_heap_stats {
    size_t region_count, mapped_bytes;             // tracked regions
    size_t block_count, live_blocks, live_bytes;   // allocated blocks
    size_t free_blocks, free_bytes, largest_free_bytes;
    size_t requested_bytes, payload_bytes;
    size_t internal_slack_bytes, overhead_bytes;
    unsigned int external_frag_permille;           // 0..1000
    unsigned int internal_frag_permille;           // 0..1000
} my_heap_stats;

int my_heap_stats_collect(my_heap_stats* out);
```

**`my_heap_stats_collect(out)`** derives every field by walking the tracked
regions and their physical block chains — a derive-from-ground-truth design
that cannot drift. Returns 1 and fills `*out` on a consistent heap; returns **0
and writes nothing** when `out` is NULL or any inconsistency is found (invalid
block, chain escaping/undershooting its region, free-count disagreement), so
the return value doubles as a heap-health check. O(blocks) — a diagnostic, not a
hot path. See `docs/architecture.md` for the metric definitions.

---

## `<mymalloc/debug.h>` — diagnostics

```c
int  my_check_heap(char* detail, size_t detail_size);
int  my_owns(const void* ptr);

typedef struct my_diag {
    size_t peak_live_bytes, peak_live_blocks, peak_mapped_bytes, peak_regions;
    size_t live_bytes, live_blocks, mapped_bytes, regions;   // current mirror
    unsigned long invalid_frees, double_frees, corruptions,
                  bad_canaries, foreign_pointers;            // events
} my_diag;
int  my_diagnostics(my_diag* out);
void my_diag_reset(void);

int  my_enable_checks(int enable);   // returns the previous state
int  my_checks_enabled(void);
int  my_canary_ok(void* ptr);

typedef void (*my_diag_hook)(const char* event, void* ptr);
void my_set_diag_hook(my_diag_hook hook);
```

- **`my_check_heap(detail, detail_size)`** — walk every region and the free
  list verifying the structural invariants; 1 when consistent (writing a short
  summary into `detail`, bounded and always NUL-terminated), 0 otherwise (naming
  the first problem). `detail` may be NULL to check without reporting.
- **`my_owns(ptr)`** — 1 when `ptr` is a live allocation this allocator handed
  out (inside a tracked region, valid header, exactly a payload start,
  allocated); 0 for NULL/foreign/interior/freed/corrupt.
- **`my_diagnostics(out)`** — fill a snapshot cheaply (reads maintained state);
  0 when `out` is NULL. Peak/current usage fields are always maintained; the
  event counters advance only while checks are enabled.
- **`my_diag_reset()`** — zero the event counters and re-baseline the peaks to
  "now".
- **`my_enable_checks(enable)`** — turn opt-in canaries + event counters on/off
  (off by default). With checks off, `my_malloc` reserves nothing and stamps
  nothing, so release builds keep the default block layout and speed.
  Returns the previous state.
- **`my_canary_ok(ptr)`** — 1 when `ptr` is an owned, allocated block whose
  trailing canary is intact; 0 for NULL/foreign/freed, a broken canary (heap
  overflow), or a block with no canary (checks off / allocated unchecked).
- **`my_set_diag_hook(hook)`** — install an optional sink called for every
  detected anomaly while checks are enabled (`"invalid free"`, `"double free"`,
  `"heap corruption"`, `"bad canary"`); `NULL` clears it.

Diagnostics never allocate on a failing path and never abort — they report and
let the caller decide. See `docs/architecture.md`.
