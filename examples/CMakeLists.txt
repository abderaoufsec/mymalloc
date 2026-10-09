add_executable(hello_mymalloc hello_mymalloc.cpp)
target_link_libraries(hello_mymalloc PRIVATE mymalloc::mymalloc)

# Phase 1 experiment: prints the process address-space layout
# (see docs/phase1_memory_model.md).
add_executable(memory_layout memory_layout.cpp)

# Phase 2 experiment: acquire/release raw page-granular regions
# (see docs/phase2_raw_memory.md).
add_executable(raw_memory_demo raw_memory_demo.cpp)
target_link_libraries(raw_memory_demo PRIVATE mymalloc::mymalloc)

# Phase 3 experiment: alignment requirements and overflow-safe rounding
# (see docs/phase3_alignment.md).
add_executable(alignment_demo alignment_demo.cpp)
target_link_libraries(alignment_demo PRIVATE mymalloc::mymalloc)

# Phase 4 experiment: block headers, conversions, validation
# (see docs/phase4_block_metadata.md).
add_executable(block_demo block_demo.cpp)
target_link_libraries(block_demo PRIVATE mymalloc::mymalloc)

# Phase 5 experiment: the first working my_malloc / my_free
# (see docs/phase5_allocator.md).
add_executable(allocator_demo allocator_demo.cpp)
target_link_libraries(allocator_demo PRIVATE mymalloc::mymalloc)