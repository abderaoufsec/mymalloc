// mymalloc — public API.
//
// This header is intentionally C compatible: the allocator API uses plain
// C types and C linkage so that it can be used from C and C++ alike.
//
// The allocation functions below are the target API from
// docs/mymalloc_documentation.md. They are only *declared* during the early
// project phases; the implementation lands in Phase 5 and later.
#ifndef MYMALLOC_H
#define MYMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Target allocation API (implemented in Phase 5+).
void* my_malloc(size_t size);
void my_free(void* ptr);
void* my_calloc(size_t count, size_t size);
void* my_realloc(void* ptr, size_t new_size);

// Version helpers (implemented in Phase 0).
// Returns the semantic version as "MAJOR.MINOR.PATCH".
const char* mymalloc_version(void);
// Returns MAJOR * 10000 + MINOR * 100 + PATCH.
unsigned int mymalloc_version_number(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_H