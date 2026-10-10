# Minimal test registration helper: each test is a standalone executable
# registered with CTest (see tests/test_framework.hpp for the harness).
function(mymalloc_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mymalloc::mymalloc)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mymalloc_add_test(test_version)
mymalloc_add_test(test_public_header)
mymalloc_add_test(test_memory_layout)
mymalloc_add_test(test_raw_memory)
mymalloc_add_test(test_alignment)
mymalloc_add_test(test_block)
mymalloc_add_test(test_allocator)
mymalloc_add_test(test_free_list)