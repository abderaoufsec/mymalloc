# Strict compiler warnings applied to every target in the project.
include_guard(GLOBAL)

function(mymalloc_apply_warnings)
    if(MSVC)
        add_compile_options(/W4 /permissive-)
        if(MYMALLOC_WARNINGS_AS_ERRORS)
            add_compile_options(/WX)
        endif()
    else()
        add_compile_options(
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference)
        if(MYMALLOC_WARNINGS_AS_ERRORS)
            add_compile_options(-Werror)
        endif()
    endif()
endfunction()