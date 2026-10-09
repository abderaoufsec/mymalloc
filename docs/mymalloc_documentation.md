# mymalloc — Memory Allocator

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
1. Implement one phase at a time.
2. Every feature needs tests.
3. Preserve alignment and metadata invariants.
4. Check integer overflow.
5. Separate OS memory acquisition from allocation policy.
6. Use sanitizers and stress tests.
7. Document architectural changes.
8. Do not mark a phase complete until its tests pass.

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
