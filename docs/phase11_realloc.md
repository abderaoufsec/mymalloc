# mymalloc — Phase 11: `realloc`

## Goal

`realloc(ptr, new_size)` resizes an existing allocation, preserving the
contents of the overlapping prefix. It is the most decision-dense function in
the allocator, because it must combine everything built so far — splitting
(Phase 7), coalescing (Phase 8), placement (Phase 9) — under the C contract's
hard rule:

> On failure, `realloc` returns `NULL` and **the original block is left
> completely untouched** — still allocated, still the caller's.

## Semantics (the decision table)

| call                | behavior                                              |
|---------------------|-------------------------------------------------------|
| `realloc(NULL, n)`  | exactly `my_malloc(n)`                                |
| `realloc(p, 0)`     | `my_free(p)`, return `NULL` (documented decision)     |
| `new_size` overflows | `NULL`, original untouched                           |
| request **fits** the block | same pointer; header-level resize (shrink/same/grow-within-slack) |
| a **free upper neighbor** can cover the rest | same pointer; neighbor absorbed (merged) |
| neither            | move: `my_malloc` + copy + `my_free` of the old block |
| allocation failure | `NULL`, original untouched (C requirement)            |
| foreign / freed `ptr` | `NULL` (refused; validation is Phase 14)           |

`realloc(p, 0)` is implementation-defined in C; mymalloc frees and returns
`NULL`, consistent with `my_malloc(0)` failing loudly (Phase 5). Returning
`NULL` here is what portable callers already handle.

## Computing the new size (same math as `my_malloc`)

The request is turned into a total block size with the Phase 5 pipeline:
align the payload, add `my_block_user_offset()`, both overflow-checked.
An overflowing request bails out **before any metadata is touched**, which is
what keeps the failure contract cheap to honour.

## The fits path — shrink, same size, grow within slack

When `total <= block->size` the resize never moves a byte:

- **Remainder worth splitting** (`>= min_split_remainder()`, the Phase 7
  threshold: header-sized plus room for free-list links): `my_block_split`
  carves the slack off the (still allocated) front. The carve-out *inherits
  the allocated state* from the block it came from, so realloc flips it to
  free and publishes it via the **same coalesce-and-publish path as
  `my_free`** — its upper neighbor may already be free (a Phase 7 remainder),
  and two adjacent free blocks would break the Phase 8 invariant.
- **Sliver remainder**: kept as internal slack. A slightly oversized
  allocation beats a free block too small to re-list.

Shrinking therefore undoes Phase 7 splitting in the same style `my_free`
does: shrink cycles round-trip back to one whole free block.

## The absorb path — growing into the upper neighbor

Only the physically next block can extend an allocation without moving the
payload (absorbing the *lower* neighbor would relocate it, which is a move
anyway). The sequence is deliberately ordered so that failure mutates
nothing:

1. `upper` exists, is **free**, and `upper->size >= total - block->size`
   (the subtraction cannot underflow here — this branch only runs when
   `total > block->size`). An insufficient or absent neighbor skips straight
   to the move path with every header untouched.
2. `my_free_list_remove(upper)` then `my_block_merge(block, upper)` — the
   same remove-then-merge chain `my_free` uses, where a neighbor that
   un-lists cleanly also merges cleanly.
3. The merged extent is re-fitted exactly like the fits path: a worthwhile
   tail is split off and published (coalescing again), a sliver stays as
   slack.

The result keeps the original pointer and preserves the entire old payload.

## The move path — allocate, copy, free

When neither the block nor its neighborhood fits:

```
fresh = my_malloc(new_size);
if (fresh == NULL) return NULL;             // original untouched
memcpy(fresh, old, min(old_payload, new_size));
my_free(old);
return fresh;
```

- The copy length is the **overlap**: `min(old payload, new_size)`. The old
  payload is `my_block_payload_size(block)` — which can exceed what the
  caller originally asked for (page rounding, un-split fits), but those extra
  bytes were never the caller's to rely on, and `new_size` bounds the other
  side.
- `my_malloc` cannot touch the source: the old block is *allocated* (checked
  up front), so no fit scan can select it, and the absorb attempt failed
  without mutating anything.
- `my_free(old)` coalesces normally, so a moved-from block rejoins the list
  in its merged, reusable form.

## Rejected pointers

Validation mirrors `my_free`: a pointer failing `my_block_valid` returns
`NULL` (full foreign-pointer validation is Phase 14), and a block that is
already **free** — a dangling pointer — is refused too. Handing free memory
back as a live allocation would silently corrupt the free list the first time
the real owner freed it again; `NULL` at least surfaces the bug.

## API

`my_realloc` was already declared in `<mymalloc/mymalloc.h>` (Phase 0);
Phase 11 only adds the definition in `src/allocator.cpp`. One internal
refactor: `my_free`'s Phase 8 coalescing body moved into the shared
`coalesce_and_publish()` helper, also used by realloc's split paths.

## Test map (`tests/test_realloc.cpp`)

- **nullptr / zero**: `realloc(NULL, n)` behaves like malloc (aligned,
  valid, writable); `realloc(p, 0)` frees and lists the block exactly once;
  `realloc(NULL, 0)` is `NULL`.
- **grow within slack**: same pointer, extent untouched, nothing carved,
  pattern preserved; same-size is a no-op.
- **shrink in place** (handcrafted `A | free B`): same pointer, prefix kept,
  block shrunk to the aligned request, the carved slack coalesces with `B`
  into ONE listed survivor (the Phase 8 invariant holds); sliver remainders
  and exact fits keep the extent whole with zero list traffic.
- **absorb**: with a sufficient free neighbor — same pointer, ALL old data
  kept, tail split off and listed; with only a sliver tail — the whole
  absorbed extent stays and the list ends empty.
- **move**: lone block grown past its region — different pointer, overlap
  copied, old block listed; with an *insufficient* free neighbor — move, and
  the old block's `my_free` coalesces it with the neighbor into one survivor.
- **failure**: `SIZE_MAX`, near-`SIZE_MAX`, and unmappable sizes all return
  `NULL` with the original block valid, allocated, unlisted, and its data
  intact; a failing request never consumes a free neighbor (sufficiency is
  checked before mutation).
- **rejected pointers**: a misaligned foreign address and an already-freed
  pointer both return `NULL` without disturbing the free list.
- **round trip**: `32 → 100 → 500 → 2000 → 64 → 3000 → 48 → 5000` bytes with
  a patterned prefix that must survive every step (exercises fits, shrinks
  with coalescing, absorbs, and moves end to end).

## Limitations / deferred

- In-place growth only considers the **immediate** upper neighbor. Merging a
  run of several consecutive free blocks would need the Phase 8 invariant to
  be weakened or a longer scan; today at most one neighbor is ever free.
- Lower-neighbor absorption is never attempted (it would move the payload
  start); such cases always take the move path.
- No `reallocarray`-style checked multiply (`realloc(p, count, size)`) yet.
- Shrunk-away bytes beyond the new payload are not zeroed — `realloc` is not
  `calloc`.
- Thread safety: none (unchanged).
- Loud double-free / corruption diagnostics: Phase 14.
