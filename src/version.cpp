// Version helpers for the mymalloc library.
//
// The definitions below match the `extern "C"` declarations in
// <mymalloc/mymalloc.h>, so they are exported with C linkage.
#include <mymalloc/config.hpp>
#include <mymalloc/mymalloc.h>

const char* mymalloc_version(void) {
    return MYMALLOC_VERSION_STRING;
}

unsigned int mymalloc_version_number(void) {
    return 10000U * MYMALLOC_VERSION_MAJOR + 100U * MYMALLOC_VERSION_MINOR + MYMALLOC_VERSION_PATCH;
}