# mymalloc — Architecture and Design

## Project Name
**mymalloc — User-Space Memory Allocator**

## Goal
Build a serious C++20 educational memory allocator that progressively implements raw memory acquisition, alignment, metadata, free lists, splitting, coalescing, heap management, `malloc`/`free`/`calloc`/`realloc`, fragmentation analysis, corruption detection, debugging, stress testing, and benchmarking.

## Target API
```cpp
void* my_malloc(std::size_t size);
void  my_free(void* ptr);
void* my_calloc(std::size_t count, std::size_t size);
void* my_realloc(void* ptr, std::size_t new_size);
```

## Architecture
```text
Application
    ↓
mymalloc API
    ↓
Allocator
 ├── alignment
 ├── block metadata
 ├── free list
 ├── splitting/coalescing
 └── allocation strategy
    ↓
Heap Manager
    ↓
OS memory
 ├── mmap()
 └── munmap()
```

## Core Concepts
- virtual memory and process address space
- pages and memory mappings
- raw byte-level memory management
- alignment and pointer arithmetic
- allocator metadata and invariants
- free-list algorithms
- first-fit/best-fit strategies
- internal/external fragmentation
- block splitting and coalescing
- heap growth and release
- `realloc` semantics
- corruption and double-free detection
- testing, debugging, and benchmarking

## Engineering Rules
1. Implement one component at a time.
2. Every feature needs tests.
3. Preserve alignment and metadata invariants.
4. Check integer overflow.
5. Separate OS memory acquisition from allocation policy.
6. Use sanitizers and stress tests.
7. Document architectural changes.
8. Do not mark a component complete until its tests pass.

## Final Success Criteria
The developer must be able to explain both flows:

```text
allocate
  → align size
  → search free list
  → reuse or grow heap
  → split if necessary
  → return aligned pointer
```

```text
free
  → validate pointer
  → mark free
  → update free list
  → coalesce neighbors
  → optionally release memory
```

## Implementation map

The allocator is complete. The public API is documented in [`api.md`](api.md).

| Area | Key file(s) |
|------|-------------|
| Foundation (C++20 + CMake, CI, tests) | `CMakeLists.txt`, `src/version.cpp` |
| Process memory model | `examples/memory_layout.cpp` |
| Raw OS memory (`mmap`/`VirtualAlloc`) | `src/raw_memory.cpp` |
| Overflow-safe alignment | `src/alignment.cpp` |
| Block metadata + validator | `src/block.cpp` |
| Core `my_malloc`/`my_free` | `src/allocator.cpp` |
| Intrusive address-ordered free list | `src/free_list.cpp` |
| Splitting oversized free blocks | `src/allocator.cpp` |
| Coalescing adjacent free blocks | `src/block.cpp` |
| First-fit / best-fit strategies | `src/allocator.cpp` |
| `my_calloc` (overflow-checked, zeroed) | `src/allocator.cpp` |
| `my_realloc` (in-place / move) | `src/allocator.cpp` |
| Region registry + scavenge-on-acquire | `src/regions.cpp` |
| Fragmentation statistics | `src/stats.cpp` |
| Diagnostics, ownership, canaries | `src/debug.cpp` |
| Deterministic stress suite | `tests/test_stress.cpp` |
| Benchmarks + profiling | `examples/benchmark.cpp` |

## Consolidated invariants

These hold across the whole heap at all times and are machine-checked by the
test suite:

1. **Alignment** — every user pointer is aligned to `my_default_alignment()`;
   every block `size` is a multiple of it.
2. **Tiling** — blocks tile each tracked region exactly; a region's blocks start
   at its base and end exactly at its end.
3. **No overlap** — a live block's payload is disjoint from every other block's
   payload and header (proved under randomized load by `test_stress`).
4. **Free-list integrity** — listed blocks are free, in strictly ascending
   address order, with reciprocal links, and the count equals the physical
   free-block count.
5. **Accounting** — the statistics walk, the incremental diagnostic counters,
   and the allocator's own view agree on live bytes/blocks, mapped bytes, and
   region count at every checkpoint.
6. **Overflow safety** — every size computation (align, `+header`, `count*size`,
   canary reserve) is checked and fails cleanly rather than wrapping.
7. **Ownership** — `my_free`/`my_realloc` only act on pointers this allocator
   handed out and that are still allocated; anything else is refused without
   corrupting state.

## Consolidated limitations

1. **Coarse-grained locking** — one process-wide mutex serializes every public
   entry point, so concurrent calls are safe but do not scale; there are no
   per-thread caches or lock-free paths.
2. **Single-threaded performance model** — no per-thread caches; the cold path
   costs an `mmap` per fresh region (~26× a reuse hit in the benchmark). A
   slab/size-class cache is the recorded future optimization.
3. **Requests ≥ 2³¹ bytes** are approximated (the block header's `requested`
   field stores a sentinel); internal fragmentation for such a request reads 0.
4. **Canaries are trailing-only** — they catch overflows past a request, not
   writes before a block; header corruption is caught by the validator instead.
5. **Diagnostics are O(blocks)** (`my_check_heap`, `my_heap_stats_collect`) —
   meant for tests/debugging, not per-allocation use.
6. **Best fit is opt-in and not beneficial here** — its O(n) scan costs ~17×
   first fit for no fragmentation gain in this region-per-block model
   (benchmark); first fit is the documented default.
