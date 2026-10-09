// mymalloc Phase 1 — address-layout experiment.
//
// Prints the location of one representative object per region of the process
// address space (.text, .rodata, .data, .bss, heap, stack), the observed
// stack-growth direction, and the heap/stack gap. On Linux it additionally
// dumps and annotates /proc/self/maps.
//
// See docs/phase1_memory_model.md for the full study this demonstrates.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

#ifdef __linux__
#include <fstream>
#include <string>
#endif

namespace {

// One representative symbol per segment of the binary.
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

void print_address(const char* region, const char* name, std::uintptr_t address) {
    std::printf("[%-6s] %-26s %p\n", region, name, reinterpret_cast<void*>(address));
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

// Records the address of a local in each of three nested call frames so the
// stack growth direction can be determined empirically. The functions must
// really be separate frames, hence MYMALLOC_NOINLINE.
#if defined(__GNUC__) || defined(__clang__)
#define MYMALLOC_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define MYMALLOC_NOINLINE __declspec(noinline)
#else
#define MYMALLOC_NOINLINE
#endif

struct StackProbes {
    std::uintptr_t outer;
    std::uintptr_t middle;
    std::uintptr_t inner;
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
    probes.outer = address_of(&outer_local[0]);
    probes.middle = middle_frame(probes.inner);
    return probes;
}

#ifdef __linux__
void dump_maps(std::uintptr_t data_address, std::uintptr_t stack_address) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        std::printf("(/proc/self/maps could not be opened)\n");
        return;
    }

    std::printf("--- /proc/self/maps (first lines) ---\n");
    std::string line;
    int printed = 0;
    while (printed < 10 && std::getline(maps, line)) {
        std::printf("  %s\n", line.c_str());
        ++printed;
    }

    std::printf("--- mappings containing our symbols ---\n");
    maps.clear();
    maps.seekg(0);
    while (std::getline(maps, line)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char permissions[8] = {};
        if (std::sscanf(line.c_str(), "%llx-%llx %7s", &start, &end, permissions) != 3) {
            continue;
        }
        const bool covers_data = data_address >= start && data_address < end;
        const bool covers_stack = stack_address >= start && stack_address < end;
        if (covers_data || covers_stack) {
            std::printf("  %s%s%s\n", line.c_str(), covers_data ? "   <- .data/.bss" : "",
                        covers_stack ? "   <- stack" : "");
        }
    }
}
#endif

} // namespace

int main() {
    void* heap_block = std::malloc(1024);
    void* small_heap_block = std::malloc(64);

    std::printf("=== mymalloc Phase 1: process memory layout experiment ===\n");
    std::printf("page size: %zu bytes\n\n", query_page_size());

    const std::uintptr_t text = code_address_of(&probe_function);
    const std::uintptr_t rodata = address_of(g_rodata_global);
    const std::uintptr_t data = address_of(&g_data_global);
    const std::uintptr_t bss = address_of(g_bss_global);
    const std::uintptr_t heap = address_of(heap_block);
    const std::uintptr_t heap_small = address_of(small_heap_block);

    std::printf("--- static storage segments ---\n");
    print_address("text", "probe_function()", text);
    print_address("rodata", "g_rodata_global", rodata);
    print_address("data", "g_data_global", data);
    print_address("bss", "g_bss_global[0]", bss);
    std::printf("        g_bss_global contents: [%lld, %lld, ..., %lld] (zero-init)\n",
                g_bss_global[0], g_bss_global[1], g_bss_global[15]);

    std::printf("\n--- heap (dynamic storage) ---\n");
    print_address("heap", "malloc(1024)", heap);
    print_address("heap", "malloc(64)", heap_small);
    std::printf("        malloc alignment ok: %s (required: %zu)\n",
                heap % alignof(std::max_align_t) == 0U ? "yes" : "NO", alignof(std::max_align_t));

    std::printf("\n--- stack frames ---\n");
    const StackProbes probes = probe_stack_frames();
    print_address("stack", "outer frame local", probes.outer);
    print_address("stack", "middle frame local", probes.middle);
    print_address("stack", "inner frame local", probes.inner);
    if (probes.inner < probes.middle && probes.middle < probes.outer) {
        std::printf("        stack grows: downward (towards lower addresses)\n");
    } else if (probes.inner > probes.middle && probes.middle > probes.outer) {
        std::printf("        stack grows: upward (towards higher addresses)\n");
    } else {
        std::printf("        stack direction: inconsistent (unexpected!)\n");
    }

    std::printf("\n--- gaps (bytes, signed; Windows places the heap elsewhere) ---\n");
    std::printf("        bss -> heap  : %lld\n", static_cast<long long>(heap - bss));
    std::printf("        heap -> stack: %lld\n", static_cast<long long>(probes.outer - heap));

    std::printf("\n--- observations ---\n");
    std::printf("        every object above has a distinct address: %s\n",
                (text != rodata && rodata != data && data != bss && bss != heap &&
                 heap != heap_small && text != heap && data != probes.outer)
                    ? "yes"
                    : "NO (unexpected!)");

#ifdef __linux__
    std::printf("\n");
    dump_maps(data, probes.outer);
#else
    std::printf("\n(/proc/self/maps is Linux-only; on Windows use System Informer/VMMap or "
                "VirtualQuery to inspect the same regions)\n");
#endif

    std::free(small_heap_block);
    std::free(heap_block);
    return 0;
}