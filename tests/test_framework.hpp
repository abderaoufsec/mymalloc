// Minimal test harness for the mymalloc test suite.
//
// Kept deliberately dependency-free: every test is a standalone executable
// that returns EXIT_SUCCESS/EXIT_FAILURE so CTest can run it directly.
#ifndef MYMALLOC_TESTS_TEST_FRAMEWORK_HPP
#define MYMALLOC_TESTS_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>

namespace mymalloc::test {

inline int g_checks = 0;
inline int g_failures = 0;

inline void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expr, file, line);
    }
}

inline int summarize(const char* suite) {
    std::printf("[%s] %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace mymalloc::test

#define MYMALLOC_CHECK(expr)      ::mymalloc::test::check((expr), #expr, __FILE__, __LINE__)
#define MYMALLOC_TEST_MAIN(suite) return ::mymalloc::test::summarize(suite)

#endif // MYMALLOC_TESTS_TEST_FRAMEWORK_HPP