// mymalloc — raw OS memory layer (Phase 2).
//
// This is the only place where mymalloc talks to the OS for memory:
//   POSIX   : mmap() / munmap()
//   Windows : VirtualAlloc() / VirtualFree()
//
// All sizes are rounded up to whole pages (see docs/phase2_raw_memory.md for
// the strategy decision). Returned blocks are page-aligned, zero-initialized
// and fully writable. Allocation policy (free lists, splitting, ...) must be
// built *on top of* this layer, never inside it.
#ifndef MYMALLOC_RAW_MEMORY_H
#define MYMALLOC_RAW_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns the system page size in bytes (queried once, then cached).
size_t my_raw_page_size(void);

// Acquires at least `bytes` of fresh, zero-initialized, writable memory,
// rounded up to a whole number of pages. Returns NULL when:
// - `bytes` is 0,
// - rounding `bytes` up to a page would overflow size_t,
// - the OS refuses the request.
void* my_raw_acquire(size_t bytes);

// Releases a block previously returned by my_raw_acquire. `bytes` must be
// the same value that was passed to my_raw_acquire (it is rounded up
// identically, so both calls agree on the real region size).
// No-op when ptr is NULL.
void my_raw_release(void* ptr, size_t bytes);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MYMALLOC_RAW_MEMORY_H