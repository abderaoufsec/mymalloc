# Each phase ships one runnable demo. The helper keeps the target wiring in a
# single place; memory_layout is the one experiment that does not use the
# allocator and therefore links nothing.
function(mymalloc_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mymalloc::mymalloc)
endfunction()

# Phase 0: version smoke test.
mymalloc_add_example(hello_mymalloc)

# Phase 1: process address-space layout (see docs/architecture.md).
add_executable(memory_layout memory_layout.cpp)

# Phase 2: acquire/release raw page-granular regions
# (see docs/architecture.md).
mymalloc_add_example(raw_memory_demo)

# Phase 3: alignment requirements and overflow-safe rounding
# (see docs/architecture.md).
mymalloc_add_example(alignment_demo)

# Phase 4: block headers, conversions, validation
# (see docs/architecture.md).
mymalloc_add_example(block_demo)

# Phase 5: the first working my_malloc / my_free
# (see docs/architecture.md).
mymalloc_add_example(allocator_demo)

# Phase 6: free list structure, first-fit and block reuse
# (see docs/architecture.md).
mymalloc_add_example(free_list_demo)

# Phase 7: splitting oversized free blocks during reuse
# (see docs/architecture.md).
mymalloc_add_example(split_demo)

# Phase 8: coalescing adjacent free blocks on free
# (see docs/architecture.md).
mymalloc_add_example(coalescing_demo)

# Phase 9: first-fit vs best-fit placement strategies
# (see docs/architecture.md).
mymalloc_add_example(strategy_demo)

# Phase 10: calloc — zero-init, overflow checks, reuse still zeroed
# (see docs/architecture.md).
mymalloc_add_example(calloc_demo)

# Phase 11: realloc — in-place fits/absorb, moves, failure contract
# (see docs/architecture.md).
mymalloc_add_example(realloc_demo)

# Phase 12: region tracking, multi-region, guarded release,
# scavenge-on-acquire (see docs/architecture.md).
mymalloc_add_example(regions_demo)

# Phase 13: fragmentation statistics — requested/allocated/free bytes, block
# counts, internal vs external fragmentation
# (see docs/architecture.md).
mymalloc_add_example(fragmentation_demo)

# Phase 14: debugging — usage accounting, ownership/consistency checks, and
# opt-in canaries + invalid/double-free detection
# (see docs/architecture.md).
mymalloc_add_example(debug_demo)

# Phase 15: stress testing — a deterministic randomized alloc/calloc/realloc/
# free workload with data-integrity verification and a leak audit
# (see docs/architecture.md).
mymalloc_add_example(stress_demo)

# Phase 16: performance — hot-path ns/op benchmarks, a first-fit vs best-fit
# comparison on an identical workload, and the fragmentation each leaves behind
# (see docs/architecture.md).
mymalloc_add_example(benchmark)
