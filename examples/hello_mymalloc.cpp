// Phase 0 example: the allocator itself is not implemented yet, so this only
// shows how to link against the library and query its version.
#include <cstdio>

#include <mymalloc/mymalloc.h>

int main() {
    std::printf("mymalloc %s\n", mymalloc_version());
    std::printf("version number: %u\n", mymalloc_version_number());
    return 0;
}