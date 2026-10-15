# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-10-15

### Fixed

- **MSVC `C2039 'size_t' is not a member of 'std'`**: Translation units used `std::size_t` but only included `<cstdint>` / `<stddef.h>`, which MSVC does not use to declare `std::size_t`. Added `#include <cstddef>` to every file that relies on it, and normalized unqualified `uintptr_t` to `std::uintptr_t`.
- **Windows multi-config generator ignored the build type**: The Visual Studio generator ignores `CMAKE_BUILD_TYPE`, so the `debug` and `release` presets both built the default configuration and `ctest` had no configuration to run. Added an explicit `"configuration"` (Debug/Release) to every build and test preset. `windows-msvc-release` now compiles and tests an optimized build.
- **Default alignment on MSVC**: MSVC's `alignof(max_align_t)` is 8 (not 16 as on GCC/Clang), which broke the documented 16-byte guarantee on 64-bit platforms. `my_default_alignment()` now floors the natural alignment at 16 when pointers are 64-bit; unchanged on GCC/Clang.
- **Undefined behavior / cppcheck finding on first table grow**: `grow_table` (`src/regions.cpp`) and `armed_grow` (`src/debug.cpp`) called `memcpy` with a null source and length 0, which UBSan's `nonnull-attribute` aborts and cppcheck flags. They now copy only when the source is non-null and there is at least one entry.

### Added

- **`clang-tidy` CI job**: The README documented clang-tidy but CI never ran it. A new job analyzes every TU in the compile database (library, tests and examples) with the repo's `.clang-tidy` via `scripts/run_clang_tidy.py`. The tree is clang-tidy clean; findings are fixed in code, never suppressed.
- **CI hardening**: least-privilege `permissions: contents: read`, a `concurrency` group that cancels superseded runs, and `LastTest.log` uploaded as an artifact on test failure.
- **`LICENSE`** (MIT) and this changelog.

### Changed

- **Public header linkage**: `block.h` and `free_list.h` are now `extern "C"` (matching the other public headers); `strategy.h` is documented as C++-only because its `enum my_strategy : unsigned int` uses a fixed underlying type, which is not valid C. See `docs/api.md`.

## [0.1.0] - 2026-10-11

First tagged release: Phases 0-17 complete (raw memory through benchmarking and final documentation).