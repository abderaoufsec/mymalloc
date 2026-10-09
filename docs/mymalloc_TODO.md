# mymalloc — TODO Roadmap

## Phase 0 — Foundation
- [x] C++20 + CMake project
- [x] `include/`, `src/`, `tests/`, `docs/`, `examples/`
- [x] strict compiler warnings
- [x] Debug/Release builds
- [x] ASan/UBSan
- [x] clang-format/static analysis
- [x] GitHub CI
- [x] initial tests
- [x] README/build documentation

## Phase 1 — Memory Model
- [x] Study virtual address spaces
- [x] Study stack, heap, `.text`, `.data`, `.bss`
- [x] Study pages and page size
- [x] Study `/proc/self/maps`
- [x] Write address-layout experiments
- [x] Document process memory layout

## Phase 2 — Raw Memory
- [x] Study `mmap()` / `munmap()`
- [x] Study `brk()` / `sbrk()`
- [x] Choose initial OS memory strategy
- [x] Implement raw region acquisition
- [x] Implement region release
- [x] Handle OS allocation failure
- [x] Test repeated acquire/release

## Phase 3 — Alignment
- [x] Define alignment requirements
- [x] Implement safe alignment helpers
- [x] Handle alignment arithmetic overflow
- [x] Verify returned pointers
- [x] Test many sizes and boundaries

## Phase 4 — Block Metadata
- [x] Design block header
- [x] Define size/state semantics
- [x] Track neighboring blocks
- [x] Convert header ↔ user pointer safely
- [x] Define metadata invariants
- [x] Add metadata validation

## Phase 5 — First Allocator
- [ ] Implement `my_malloc`
- [ ] Implement `my_free`
- [ ] Acquire memory when needed
- [ ] Return aligned memory
- [ ] Handle `free(nullptr)`
- [ ] Test single/multiple allocations
- [ ] Test allocation failure

## Phase 6 — Free List
- [ ] Design free-list structure
- [ ] Insert/remove free blocks
- [ ] Search reusable blocks
- [ ] Implement first-fit
- [ ] Maintain list invariants
- [ ] Test block reuse
- [ ] Add consistency checks

## Phase 7 — Splitting
- [ ] Detect oversized free blocks
- [ ] Define minimum split size
- [ ] Split safely
- [ ] Return requested portion
- [ ] Insert remainder
- [ ] Test boundary cases

## Phase 8 — Coalescing
- [ ] Detect adjacent free blocks
- [ ] Merge previous/next blocks
- [ ] Update metadata and free list
- [ ] Handle both neighbors
- [ ] Test fragmentation reduction

## Phase 9 — Allocation Strategies
- [ ] Formalize first-fit
- [ ] Measure fragmentation/cost
- [ ] Implement best-fit optionally
- [ ] Compare strategies
- [ ] Select/document default

## Phase 10 — `calloc`
- [ ] Implement `my_calloc`
- [ ] Check multiplication overflow
- [ ] Zero memory
- [ ] Test zero initialization
- [ ] Test overflow cases

## Phase 11 — `realloc`
- [ ] Define semantics
- [ ] Handle `nullptr`
- [ ] Handle zero size
- [ ] Shrink in place when possible
- [ ] Expand in place when possible
- [ ] Move when necessary
- [ ] Preserve data
- [ ] Handle allocation failure
- [ ] Comprehensive tests

## Phase 12 — Heap Management
- [ ] Track managed regions
- [ ] Support multiple regions
- [ ] Safely release eligible regions
- [ ] Prevent invalid unmapping
- [ ] Test long allocation/free cycles
- [ ] Document ownership

## Phase 13 — Fragmentation
- [ ] Define internal/external fragmentation
- [ ] Add allocator statistics
- [ ] Track requested/allocated/free bytes
- [ ] Track block counts
- [ ] Measure fragmentation
- [ ] Add fragmentation workloads

## Phase 14 — Debugging
- [ ] Validate pointers
- [ ] Detect invalid free
- [ ] Detect double free
- [ ] Add optional canaries
- [ ] Detect metadata corruption
- [ ] Add heap consistency checker
- [ ] Add diagnostic output
- [ ] Add debug statistics

## Phase 15 — Stress Testing
- [ ] Random allocation/free
- [ ] Random realloc
- [ ] Mixed sizes
- [ ] Long-running workloads
- [ ] Verify stored data
- [ ] Run sanitizer builds
- [ ] Detect leaks/corruption

## Phase 16 — Performance
- [ ] Allocation benchmarks
- [ ] Free benchmarks
- [ ] Realloc benchmarks
- [ ] Fragmentation benchmarks
- [ ] Compare strategies
- [ ] Profile bottlenecks
- [ ] Optimize only after profiling

## Phase 17 — Final Quality
- [ ] Complete API docs
- [ ] Complete architecture docs
- [ ] Document invariants
- [ ] Document limitations
- [ ] Add examples
- [ ] Full test suite
- [ ] Sanitizers
- [ ] Static analysis
- [ ] Formatting checks
- [ ] Release build
- [ ] CI verification
- [ ] Final README
- [ ] Final review/tag

## AI Rules
For every phase:
1. Explain the concept first.
2. Identify exact files.
3. Make the smallest necessary change.
4. Explain important code and invariants.
5. Build immediately.
6. Run focused tests.
7. Run regression tests.
8. Diagnose failures before continuing.
9. Update documentation when architecture changes.
10. Commit only after verification.

## Definition of Done
A phase is complete only when implementation, tests, documentation, invariants, and failure handling are all verified.
