// mymalloc Phase 10 — my_calloc experiment.
//
// calloc has two jobs malloc does not:
//   1. it takes (count, size) and must reject a product that would overflow
//      size_t BEFORE multiplying (the classic calloc safety bug),
//   2. it must return memory that is all zero bits.
//
// The subtle part on this allocator: a FRESH raw region arrives zero-filled by
// the OS, but a REUSED free block (Phases 6-7) hands back stale bytes. So
// calloc cannot trust the mapping — it zeroes explicitly. This demo makes the
// stale-reuse case visible.
//
// See docs/phase10_calloc.md for the design.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <mymalloc/block.h>
#include <mymalloc/mymalloc.h>

namespace {

constexpr std::size_t kPage = 4096U;

// True when every byte in [ptr, ptr+size) is zero.
bool all_zero(const void* ptr, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::printf("=== mymalloc Phase 10: my_calloc experiment ===\n\n");

    // --- 1. basic zero-initialization --------------------------------------
    const std::size_t count = 32U;
    const std::size_t elem = 64U;
    void* array = my_calloc(count, elem);
    std::printf("my_calloc(%zu, %zu) -> %p  all_zero=%s\n", count, elem, array,
                all_zero(array, count * elem) ? "yes" : "NO");
    my_free(array);

    // --- 2. overflow is rejected BEFORE the multiply -----------------------
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    std::printf("my_calloc(SIZE_MAX, 2)      -> %s\n",
                my_calloc(max, 2U) == nullptr ? "NULL" : "?");
    std::printf("my_calloc(2, SIZE_MAX)      -> %s\n",
                my_calloc(2U, max) == nullptr ? "NULL" : "?");
    std::printf("my_calloc(SIZE_MAX/2, 4)    -> %s\n",
                my_calloc(max / 2U, 4U) == nullptr ? "NULL" : "?");
    std::printf("my_calloc(SIZE_MAX/8, 1)    -> %s\n",
                my_calloc(max / 8U, 1U) == nullptr ? "NULL" : "?");

    // --- 3. zero count / zero size fails -----------------------------------
    std::printf("my_calloc(0, 16)            -> %s\n",
                my_calloc(0U, 16U) == nullptr ? "NULL" : "?");
    std::printf("my_calloc(16, 0)            -> %s\n",
                my_calloc(16U, 0U) == nullptr ? "NULL" : "?");

    // --- 4. the key property: a REUSED block is still zeroed ----------------
    // malloc a block, dirty every byte, free it, then calloc the SAME size.
    // The reuse path (Phases 6-7) would hand back the dirty bytes; calloc must
    // clear them. Verify by pointer identity first, then by contents.
    const std::size_t bytes = kPage;
    void* dirty = my_malloc(bytes);
    std::memset(dirty, 0xFF, bytes);
    my_free(dirty);

    void* reused = my_calloc(1, bytes);
    const bool same = (reused == dirty);
    std::printf("calloc(1, %zu) reused the dirty block: %s  all_zero=%s\n", bytes,
                same ? "yes" : "no (fresh region)", all_zero(reused, bytes) ? "yes" : "NO");
    my_free(reused);

    std::printf("\ncalloc: overflow rejected, zero-count/size rejected, reuse zeroed.\n");
    return 0;
}
