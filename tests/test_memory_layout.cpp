// Phase 1 test: assert the process memory-layout invariants documented in
// docs/phase1_memory_model.md. These are the properties later allocator
// phases (raw regions, alignment, validation) may rely on.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "test_framework.hpp"

namespace {

// One representative symbol per region of the binary.
int g_data_global = 42;                           // .data
long long g_bss_global[16] = {};                  // .bss (zero initialized)
const char g_rodata_global[] = "mymalloc-rodata"; // .rodata

int probe_function(int value) {
    return value + 1;
}

std::uintptr_t address_of(const volatile void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

std::uintptr_t code_address_of(int (*function)(int)) {
    return reinterpret_cast<std::uintptr_t>(function);
}

int* static_data_pointer() {
    return &g_data_global;
}

std::size_t query_page_size() {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 0U;
#endif
}

// Records the address of a local in each of three nested call frames so stack
// growth direction and frame distinctness can be checked. The functions must
// really be separate frames, hence MYMALLOC_NOINLINE.
#if defined(__GNUC__) || defined(__clang__)
#define MYMALLOC_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define MYMALLOC_NOINLINE __declspec(noinline)
#else
#define MYMALLOC_NOINLINE
#endif

struct StackProbes {
    std::array<std::uintptr_t, 3> frames{};
};

MYMALLOC_NOINLINE std::uintptr_t inner_frame() {
    volatile char local[256] = {};
    local[0] = 3;
    return address_of(&local[0]);
}

MYMALLOC_NOINLINE std::uintptr_t middle_frame(std::uintptr_t& deeper) {
    volatile char local[256] = {};
    local[0] = 2;
    const std::uintptr_t self = address_of(&local[0]);
    deeper = inner_frame();
    return self;
}

StackProbes probe_stack_frames() {
    StackProbes probes{};
    volatile char outer_local[256] = {};
    outer_local[0] = 1;
    probes.frames[0] = address_of(&outer_local[0]);
    probes.frames[1] = middle_frame(probes.frames[2]);
    return probes;
}

#ifdef __linux__
// Returns true if some mapping in /proc/self/maps covers the address.
bool maps_contain(std::uintptr_t target) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx", &start, &end) == 2 && target >= start &&
            target < end) {
            return true;
        }
    }
    return false;
}

int count_maps_lines() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    int count = 0;
    while (std::getline(maps, line)) {
        ++count;
    }
    return count;
}
#endif

} // namespace

int main() {
    // --- page size: power of two, sane bounds -----------------------------
    const std::size_t page_size = query_page_size();
    MYMALLOC_CHECK(page_size >= 256U);
    MYMALLOC_CHECK(page_size <= 16U * 1024U * 1024U);
    MYMALLOC_CHECK((page_size & (page_size - 1U)) == 0U);

    // --- one distinct address per region ----------------------------------
    const std::uintptr_t text = code_address_of(&probe_function);
    const std::uintptr_t rodata = address_of(g_rodata_global);
    const std::uintptr_t data = address_of(&g_data_global);
    const std::uintptr_t bss = address_of(g_bss_global);
    const StackProbes probes = probe_stack_frames();
    const std::uintptr_t stack = probes.frames[0];

    void* heap_block = std::malloc(1024);
    MYMALLOC_CHECK(heap_block != nullptr);
    const std::uintptr_t heap = address_of(heap_block);

    MYMALLOC_CHECK(text != data);
    MYMALLOC_CHECK(rodata != data);
    MYMALLOC_CHECK(data != bss);
    MYMALLOC_CHECK(bss != heap);
    MYMALLOC_CHECK(heap != stack);
    MYMALLOC_CHECK(text != stack);

    // --- static storage keeps its address across calls ---------------------
    MYMALLOC_CHECK(static_data_pointer() == static_data_pointer());
    MYMALLOC_CHECK(address_of(static_data_pointer()) == data);

    // --- .bss is zero initialized ------------------------------------------
    for (long long element : g_bss_global) {
        MYMALLOC_CHECK(element == 0);
    }

    // --- stack frames: distinct and consistently ordered -------------------
    MYMALLOC_CHECK(probes.frames[0] != probes.frames[1]);
    MYMALLOC_CHECK(probes.frames[1] != probes.frames[2]);
    const bool grows_down =
        probes.frames[2] < probes.frames[1] && probes.frames[1] < probes.frames[0];
    const bool grows_up =
        probes.frames[2] > probes.frames[1] && probes.frames[1] > probes.frames[0];
    MYMALLOC_CHECK(grows_down || grows_up);

    // --- heap: malloc alignment and block distinctness ---------------------
    MYMALLOC_CHECK(heap % alignof(std::max_align_t) == 0U);
    void* second_block = std::malloc(64);
    MYMALLOC_CHECK(second_block != nullptr);
    MYMALLOC_CHECK(address_of(second_block) != heap);

    // --- globals are naturally aligned -------------------------------------
    MYMALLOC_CHECK(data % alignof(int) == 0U);
    MYMALLOC_CHECK(bss % alignof(long long) == 0U);

#ifdef __linux__
    // --- /proc/self/maps covers every region we probed ---------------------
    MYMALLOC_CHECK(count_maps_lines() >= 5);
    MYMALLOC_CHECK(maps_contain(text));
    MYMALLOC_CHECK(maps_contain(data));
    MYMALLOC_CHECK(maps_contain(bss));
    MYMALLOC_CHECK(maps_contain(heap));
    MYMALLOC_CHECK(maps_contain(stack));
#endif

    std::free(second_block);
    std::free(heap_block);
    MYMALLOC_TEST_MAIN("memory_layout");
}