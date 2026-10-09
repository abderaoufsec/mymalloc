add_executable(hello_mymalloc hello_mymalloc.cpp)
target_link_libraries(hello_mymalloc PRIVATE mymalloc::mymalloc)

# Phase 1 experiment: prints the process address-space layout
# (see docs/phase1_memory_model.md).
add_executable(memory_layout memory_layout.cpp)