# Optional sanitizer support: -DMYMALLOC_SANITIZER=address|undefined|address-undefined
include_guard(GLOBAL)

function(mymalloc_apply_sanitizers)
    set(mode "${MYMALLOC_SANITIZER}")

    if(mode STREQUAL "off")
        return()
    endif()

    if(mode STREQUAL "address")
        set(sanitizers address)
    elseif(mode STREQUAL "undefined")
        set(sanitizers undefined)
    elseif(mode STREQUAL "address-undefined")
        set(sanitizers address undefined)
    else()
        message(FATAL_ERROR
                "Unknown MYMALLOC_SANITIZER '${mode}'. "
                "Expected: off, address, undefined, address-undefined")
    endif()

    if(MSVC)
        if(NOT mode STREQUAL "address" AND NOT mode STREQUAL "address-undefined")
            message(FATAL_ERROR
                    "MSVC only supports MYMALLOC_SANITIZER=address or address-undefined")
        endif()
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        string(REPLACE ";" "," sanitizer_list "${sanitizers}")
        add_compile_options(-fsanitize=${sanitizer_list} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${sanitizer_list})
        if("undefined" IN_LIST sanitizers)
            # Any UBSan report fails the test run instead of being printed and ignored.
            add_compile_options(-fno-sanitize-recover=all)
        endif()
    endif()

    message(STATUS "mymalloc: sanitizer(s) enabled: ${mode}")
endfunction()