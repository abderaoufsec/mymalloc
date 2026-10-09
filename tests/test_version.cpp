// Phase 0 smoke test: the generated version metadata must match the project
// version, and the exported helpers must report it to user code.
#include <cstring>

#include <mymalloc/config.hpp>
#include <mymalloc/mymalloc.h>

#include "test_framework.hpp"

int main() {
    static_assert(MYMALLOC_VERSION_MAJOR == 0, "unexpected major version");
    static_assert(MYMALLOC_VERSION_MINOR == 1, "unexpected minor version");
    static_assert(MYMALLOC_VERSION_PATCH == 0, "unexpected patch version");

    MYMALLOC_CHECK(std::strcmp(mymalloc_version(), "0.1.0") == 0);
    MYMALLOC_CHECK(std::strcmp(mymalloc_version(), MYMALLOC_VERSION_STRING) == 0);
    MYMALLOC_CHECK(mymalloc_version_number() == 10000U * MYMALLOC_VERSION_MAJOR +
                                                    100U * MYMALLOC_VERSION_MINOR +
                                                    MYMALLOC_VERSION_PATCH);
    MYMALLOC_CHECK(mymalloc_version_number() == 100U);
    MYMALLOC_TEST_MAIN("version");
}