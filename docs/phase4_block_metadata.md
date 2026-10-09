# Phase 4 — Block Metadata

This document defines mymalloc's block header, its invariants, the
conversion math between block starts and user pointers, and the validation
trust boundary. Building blocks: [phase2_raw_memory.md](phase2_raw_memory.md)
(regions) and [phase3_alignment.md](phase3_alignment.md) (rounding).

## 1. Header design

Every block — allocated or free — starts with a header immediately followed
by its payload:

```text
block start                     user pointer
v                               v
[ size | free | prev | next ][ payload ... ] ... [ next block's header ]
<--------------- size -------------------------->
```

```c
struct my_block_header {
    size_t size;            /* total bytes incl. header, alignment multiple */
    my_block_header* prev;  /* physically adjacent block below, or NULL     */
    my_block_header* next;  /* physically adjacent block above, or NULL     */
    int free;               /* exactly 0 = allocated, 1 = free              */
};
```

Semantics:

- **`size`** counts the *whole* block (header + payload). Because headers
  are placed at alignment multiples and sizes are alignment multiples, both
  block starts and payload starts stay naturally aligned (Phase 3).
- **`free`** is deliberately strict (0 or 1, validated) so tests and future
  debug canaries never depend on "nonzero means free" sloppiness.
- **`prev`/`next`** are *address-order, physically adjacent* neighbors —
  not free-list pointers. This is what "track neighboring blocks" means
  here: given any block, its neighbors are O(1) away, which makes Phase 8
  coalescing trivial and lets the validator prove physical adjacency.

### Tradeoff vs. boundary tags (documented per rule 7)

glibc-style allocators store only a size footer for free blocks ("boundary
tags") and keep free-list links inside free payloads — zero cost for
allocated blocks. mymalloc instead spends 2 pointers (16 bytes on x86-64)
in **every** header: total overhead 32 bytes/block (header is
`size_t + 2 pointers + int` = 28, rounded to the 16-byte default alignment).
The price is worth it for an educational allocator: neighbor tracking,
adjacency proofs, and corruption detection become straightforward. This can
be revisited in the performance phase (Phase 16).

## 2. Conversions (header ↔ user pointer)

- `my_block_user_offset()` = `align_up(sizeof(header), default_alignment)`
  = 32 on x86-64 (header 28 → 32; already an alignment multiple).
- `my_block_to_user(b)` = `(byte*)b + offset`. Because `b` is an alignment
  multiple (validated at init) and `offset` is too, **the user pointer is
  always aligned to `my_default_alignment()`** — the Phase 3 guarantee.
- `my_user_to_block(u)` = `(byte*)u − offset` — an exact inverse; the
  round trip `user_to_block(to_user(b)) == b` is machine-checked.
- `my_block_payload_size(b)` = `size − offset` — what `my_malloc` will
  eventually be able to promise the caller.

## 3. Metadata invariants

A block is *valid* iff all of the following hold (checked by
`my_block_valid`, in this order):

1. `block != NULL`.
2. The block address is a multiple of `my_default_alignment()`.
3. `size >= user_offset` (the header itself must fit) and
   `size % default_alignment == 0`.
4. `free ∈ {0, 1}`.
5. `address + size` does not wrap `uintptr_t` (corrupted-size guard).
6. `next`: its **address** equals `block + size` (physically adjacent —
   checked without dereferencing), and `next->prev == block` (reciprocity).
7. `prev`: its address is strictly below `block`, `prev->next == block`,
   and `prev + prev->size == block` (physically adjacent from the other
   side).

Order matters: every address-level fact is established *before* a link
pointer is dereferenced, so corruption that puts a wrong address into
`prev`/`next` is rejected by pure pointer arithmetic — the validator never
reads through a pointer that already failed an address check.

## 4. Trust boundary (what validation does *not* claim)

`my_block_valid` proves *internal consistency* of a header whose link
pointers it trusts to be readable memory. It cannot (yet) prove that a
`prev`/`next` value lies inside a managed region — an adversarial
corruption that happens to point at mapped memory with plausible contents
could pass. Closing that gap needs the region registry of Phase 12 and is
the subject of the full pointer validation in Phase 14. What Phase 4
already guarantees: NULL, misaligned, wrapped, undersized, mis-stated, and
wrong-address links are all detected *without crashing*.

## 5. API summary (`include/mymalloc/block.h`)

| Function | Purpose |
|----------|---------|
| `my_block_user_offset()` | bytes from block start to user pointer |
| `my_block_to_user` / `my_user_to_block` | safe conversions (NULL-safe, exact inverses) |
| `my_block_payload_size` | usable bytes at the user pointer |
| `my_block_init` | validated initialization (size/state/neighbors) |
| `my_block_prev` / `my_block_next` / `my_block_is_free` | NULL-safe accessors |
| `my_block_valid` | full invariant check, corruption-detecting |

## 6. Tests (`tests/test_block.cpp`)

| Check | Why it matters |
|-------|----------------|
| offset ≥ sizeof(header), alignment multiple, ≤ 64 | header stays cheap and aligned |
| NULL in → NULL out for conversions | free(nullptr)-adjacent safety |
| init failures: NULL/misaligned base, size < offset, size not aligned, `free` ∉ {0,1} | strict initialization (rule 3) |
| init + valid + aligned user pointer + round trip + payload size | conversion contract |
| 3-block chain: prev/next reciprocity, physical adjacency (`b_i + size == b_{i+1}`) | neighbor tracking works |
| full-payload write leaves all three headers valid, sizes unchanged | headers/payloads never overlap |
| detected corruptions: size +8, size < offset, free = 5, wrong next, prev above block, broken reciprocity, misaligned pointer, NULL | validation catches realistic damage without crashing |

## 7. Experiment (`examples/block_demo.cpp`)

Carves three 1 KiB blocks (allocated/free/allocated) into one raw region,
prints every header field and both conversions, proves payload writes leave
metadata intact, then corrupts `b2.size` to show detection:

```sh
cmake --build --preset debug
./out/build/debug/examples/block_demo
```

## 8. Limitations

- No free-list links yet — Phase 6 adds an intrusive list through free
  payloads; the `prev`/`next` here are purely address-order.
- `my_block_init` cannot know the region bounds (the raw layer does not
  expose them); size-vs-region checks belong to Phase 12's registry.
- Headers are unprotected (no canaries/XOR obfuscation) — optional canaries
  are a Phase 14 debugging feature.
- Single-threaded by design so far; no atomic state transitions yet.