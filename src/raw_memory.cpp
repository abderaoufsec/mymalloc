// Raw OS memory acquisition for mymalloc (Phase 2).
//
// POSIX uses mmap()/munmap() with anonymous private mappings; Windows uses
// VirtualAlloc()/VirtualFree(). Both work in whole pages and both return
// zero-initialized memory. See docs/phase2_raw_memory.md.
#include <mymalloc/raw_memory.h>

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace {

// Rounds `bytes` up to a whole number of pages.
// Returns 0 for requests that must fail: zero-sized requests and requests
// whose rounding-up would overflow size_t.
std::size_t round_up_to_pages(std::size_t bytes) {
    const std::size_t page = my_raw_page_size();
    if (bytes == 0 || bytes > SIZE_MAX - (page - 1U)) {
        return 0;
    }
    return ((bytes + page - 1U) / page) * page;
}

} // namespace

size_t my_raw_page_size(void) {
    // Magic static: initialized exactly once, thread-safe since C++11.
#ifdef _WIN32
    static const std::size_t page_size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
#else
    static const std::size_t page_size = [] {
        const long size = sysconf(_SC_PAGESIZE);
        return size > 0 ? static_cast<std::size_t>(size) : 4096U;
    }();
#endif
    return page_size;
}

void* my_raw_acquire(size_t bytes) {
    const std::size_t rounded = round_up_to_pages(bytes);
    if (rounded == 0) {
        return NULL;
    }

#ifdef _WIN32
    // MEM_RESERVE sets up the address space; MEM_COMMIT backs it with pages
    // (zero-filled by the OS).
    void* region = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return region; // NULL on failure
#else
    // Anonymous private mapping: the OS hands out zero-filled pages lazily.
    void* region =
        mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        return NULL;
    }
    return region;
#endif
}

void my_raw_release(void* ptr, size_t bytes) {
    if (ptr == NULL) {
        return;
    }

    const std::size_t rounded = round_up_to_pages(bytes);
    if (rounded == 0) {
        return; // inconsistent call: cannot determine the real region size
    }

#ifdef _WIN32
    // MEM_RELEASE must free the whole reservation, hence size == 0.
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, rounded);
#endif
}