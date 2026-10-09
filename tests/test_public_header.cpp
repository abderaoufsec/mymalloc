// Phase 0 smoke test: <mymalloc/mymalloc.h> must expose exactly the target
// API from docs/mymalloc_documentation.md, must be idempotent (include guard)
// and C-linkage compatible.
//
// The allocator functions are only *declared* at this stage (implemented in
// Phase 5), so only unevaluated expressions (decltype/static_assert) are used
// here — taking their address or calling them would fail at link time.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include <mymalloc/mymalloc.h>
#include <mymalloc/mymalloc.h> // second include must be a no-op

static_assert(std::is_same_v<decltype(&my_malloc), void* (*)(std::size_t)>,
              "my_malloc must be void*(size_t)");
static_assert(std::is_same_v<decltype(&my_free), void (*)(void*)>, "my_free must be void(void*)");
static_assert(std::is_same_v<decltype(&my_calloc), void* (*)(std::size_t, std::size_t)>,
              "my_calloc must be void*(size_t, size_t)");
static_assert(std::is_same_v<decltype(&my_realloc), void* (*)(void*, std::size_t)>,
              "my_realloc must be void*(void*, size_t)");

// The C API uses the C library size_t; the header must not introduce a
// divergent type.
static_assert(std::is_same_v<std::size_t, size_t>, "size_t must come from the C library");

int main() {
    // Every check above is evaluated at compile time; reaching main means the
    // public header is well-formed.
    std::puts("[public_header] API signatures verified at compile time");
    return EXIT_SUCCESS;
}