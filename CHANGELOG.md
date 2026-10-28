# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **Arena remainder corruption**: the 16-page growth path left the carved
  remainder's physical `prev` link NULL and charged the free remainder as live
  bytes, breaking `my_check_heap`/ownership and inflating statistics. The
  remainder now points back at the front block and is no longer counted live.
- **Thread safety beyond the four allocators**: stats collection, the heap
  checker, ownership queries, diagnostics, check toggling and the region
  registry could race allocation. They now share the allocator mutex through
  lock-assumed internals, and `my_owns` has a shared body so `my_canary_ok`
  does not self-deadlock.

### Added

- **`test_threads`**: concurrent malloc/free/realloc, introspection racing
  mutation, and strategy toggling.
- **TSan preset and CI job** (`MYMALLOC_SANITIZER=thread`).

### Changed

- The spinlock was replaced by a process-wide `std::mutex` with an RAII guard;
  `realloc`'s move path now runs entirely under one lock hold.
- Public headers document the new locking policy; `free_list.h`,
  `block.h` and `alignment.h` remain unsynchronized internals.
- **Installable CMake package**: `install` rules now ship the library, headers
  and generated `config.hpp`, plus an exported `mymalloc::mymalloc` target and
  `find_package(mymalloc CONFIG)` package files. The project version is 0.1.1,
  matching the changelog, and the target advertises C++20 and Threads.
- **Documentation consolidated**: removed the per-phase design notes and the
  completed TODO roadmap. `docs/architecture.md` (renamed from
  `mymalloc_documentation.md`) and `docs/api.md` are now the canonical docs,
  and the README links only to them and the changelog.
- **CI portability fixes**: moved the internal allocator mutex out of the
  `extern "C"` block to fix Clang's `-Wreturn-type-c-linkage`, normalized
  formatting with the CI-pinned clang-format, and fixed the clang-tidy findings
  (explicit multilevel pointer conversions, vector reserve, cast placement).

## [0.1.1] - 2026-10-15

### Fixed

- **MSVC `C2039 'size_t' is not a member of 'std'`**: Translation units used `std::size_t` but only included `<cstdint>` / `<stddef.h>`, which MSVC does not use to declare `std::size_t`. Added `#include <cstddef>` to every file that relies on it, and normalized unqualified `uintptr_t` to `std::uintptr_t`.
- **Windows multi-config generator ignored the build type**: The Visual Studio generator ignores `CMAKE_BUILD_TYPE`, so the `debug` and `release` presets both built the default configuration and `ctest` had no configuration to run. Added an explicit `"configuration"` (Debug/Release) to every build and test preset. `windows-msvc-release` now compiles and tests an optimized build.
- **Default alignment on MSVC**: MSVC's `alignof(max_align_t)` is 8 (not 16 as on GCC/Clang), which broke the documented 16-byte guarantee on 64-bit platforms. `my_default_alignment()` now floors the natural alignment at 16 when pointers are 64-bit; unchanged on GCC/Clang.
- **Undefined behavior / cppcheck finding on first table grow**: `grow_table` (`src/regions.cpp`) and `armed_grow` (`src/debug.cpp`) called `memcpy` with a null source and length 0, which UBSan's `nonnull-attribute` aborts and cppcheck flags. They now copy only when the source is non-null and there is at least one entry.

### Added

- **`clang-tidy` CI job**: The README documented clang-tidy but CI never ran it. A new job analyzes every TU in the compile database (library, tests and examples) with the repo's `.clang-tidy` via `scripts/run_clang_tidy.py`. The tree is clang-tidy clean; genuine findings are fixed in code and the few checks that are intentionally not applicable are excluded in `.clang-tidy` with a documented rationale.
- **CI hardening**: least-privilege `permissions: contents: read`, a `concurrency` group that cancels superseded runs, and `LastTest.log` uploaded as an artifact on test failure.
- **`LICENSE`** (MIT) and this changelog.

### Changed

- **Public header linkage**: `block.h` and `free_list.h` are now `extern "C"` (matching the other public headers); `strategy.h` is documented as C++-only because its `enum my_strategy : unsigned int` uses a fixed underlying type, which is not valid C. See `docs/api.md`.

## [0.1.0] - 2026-10-11

First tagged release: complete allocator (raw memory through benchmarking and documentation).
