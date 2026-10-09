# mymalloc — User-Space Memory Allocator

An educational C++20 memory allocator, built phase by phase: raw OS memory,
alignment, block metadata, free lists, splitting/coalescing,
`malloc`/`free`/`calloc`/`realloc`, fragmentation analysis, corruption
detection, stress testing, and benchmarking.

See [docs/mymalloc_documentation.md](docs/mymalloc_documentation.md) for the
architecture and [docs/mymalloc_TODO.md](docs/mymalloc_TODO.md) for the full
phase roadmap.

## Project layout

```text
mymalloc/
├── CMakeLists.txt          # C++20 build (strict warnings, sanitizers, tests)
├── CMakePresets.json       # debug / release / asan / ubsan / asan-ubsan presets
├── cmake/                  # warnings + sanitizer CMake modules
├── include/mymalloc/       # public headers (mymalloc.h, generated config.hpp)
├── src/                    # library implementation
├── tests/                  # CTest suite (dependency-free harness)
├── examples/               # sample programs
├── scripts/                # developer tooling (check_format.py)
├── docs/                   # documentation and roadmap
└── .github/workflows/      # CI (build, test, sanitizers, format, cppcheck)
```

## Target API

```cpp
void* my_malloc(std::size_t size);
void  my_free(void* ptr);
void* my_calloc(std::size_t count, std::size_t size);
void* my_realloc(void* ptr, std::size_t new_size);
```

Declared in [`include/mymalloc/mymalloc.h`](include/mymalloc/mymalloc.h)
(C linkage, usable from C and C++). The implementation lands in Phase 5+;
Phase 0 only ships the project foundation and version helpers.

## Requirements

- CMake ≥ 3.20 (presets require ≥ 3.25)
- A C++20 compiler: GCC ≥ 11, Clang ≥ 14, MSVC ≥ 19.29 (VS 2019 16.10)

## Building

The easiest way is with the provided presets (run from the repository root):

```sh
cmake --preset debug          # configure (Debug)
cmake --build --preset debug  # build
ctest --preset debug          # run tests
```

Available configure presets:

| Preset        | Build type | Sanitizers                   |
|---------------|------------|------------------------------|
| `debug`       | Debug      | none                         |
| `release`     | Release    | none                         |
| `asan`        | Debug      | AddressSanitizer             |
| `ubsan`       | Debug      | UndefinedBehaviorSanitizer   |
| `asan-ubsan`  | Debug      | ASan + UBSan                 |

Build and test presets share the same names (`cmake --build --preset release`,
`ctest --preset asan-ubsan`, ...). Presets place their output under
`out/build/<preset>/` and leave the source tree clean.

### Manual build (no presets)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Useful cache options

| Option                        | Default | Effect                                          |
|-------------------------------|---------|-------------------------------------------------|
| `MYMALLOC_BUILD_TESTS`        | `ON`    | Build the test suite                            |
| `MYMALLOC_BUILD_EXAMPLES`     | `ON`    | Build the examples                              |
| `MYMALLOC_WARNINGS_AS_ERRORS` | `OFF`   | Turn strict warnings into errors (`-Werror` / `/WX`) |
| `MYMALLOC_SANITIZER`          | `off`   | `address`, `undefined`, or `address-undefined`  |

Example:
`cmake --preset debug -DMYMALLOC_SANITIZER=address-undefined -DMYMALLOC_WARNINGS_AS_ERRORS=ON`

> Sanitizers are best supported on Linux (GCC/Clang). MSYS2/MinGW does not
> ship the ASan/UBSan runtimes for its toolchains, so on Windows use WSL or
> rely on the CI sanitizer job (which runs ASan + UBSan on Ubuntu).

## Strict warnings

All targets are compiled with strict warnings: `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align ...`
(`-Werror` optional via `MYMALLOC_WARNINGS_AS_ERRORS=ON`; MSVC uses
`/W4 /permissive-`). CI builds with `MYMALLOC_WARNINGS_AS_ERRORS=ON`, so the
tree is warning-free.

## Testing

Tests are plain executables registered with CTest, using the tiny harness in
[`tests/test_framework.hpp`](tests/test_framework.hpp) (no external
dependencies). Current Phase 0 tests:

- `test_version` — generated version metadata matches the exported helpers
- `test_public_header` — public header exposes exactly the target API
  (compile-time signature checks, include-guard and `size_t` compatibility)

Run everything with `ctest --preset debug`, or a single test:

```sh
ctest --preset debug -R test_version
```

## Formatting and static analysis

```sh
python scripts/check_format.py          # verify formatting (clang-format)
python scripts/check_format.py --fix    # rewrite files in place
```

The style lives in [`.clang-format`](.clang-format) (LLVM base, 4-space
indent, 100 columns). CI enforces it with `clang-format==23.1.3`.

For static analysis:

- [`.clang-tidy`](.clang-tidy) — bugprone/analyzer/performance/portability
  checks, run locally as `clang-tidy` against
  `out/build/debug/compile_commands.json`
- CI also runs `cppcheck --enable=warning,performance,portability` with
  `--error-exitcode=1` on the generated `compile_commands.json`

## Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push to
`main` and every pull request:

1. **build-and-test** — GCC and Clang, Debug and Release, warnings-as-errors
2. **build-and-test-windows** — MSVC, Debug and Release
3. **sanitizers** — ASan + UBSan
4. **format** — clang-format check
5. **static-analysis** — cppcheck

## Roadmap

Progress is tracked in [docs/mymalloc_TODO.md](docs/mymalloc_TODO.md):

- [x] **Phase 0 — Foundation**: C++20 + CMake project, layout, strict warnings,
  Debug/Release, ASan/UBSan, clang-format/static analysis, GitHub CI,
  initial tests, build documentation
- [x] **Phase 1 — Memory Model**: address-space study and invariants in
  [docs/phase1_memory_model.md](docs/phase1_memory_model.md), layout experiment
  (`examples/memory_layout.cpp`, incl. `/proc/self/maps` on Linux), invariants
  test (`test_memory_layout`)
- [x] **Phase 2 — Raw Memory**: mmap/VirtualAlloc strategy chosen and documented
  in [docs/phase2_raw_memory.md](docs/phase2_raw_memory.md); raw region
  layer (`<mymalloc/raw_memory.h>`, page-rounded/aligned, zero-filled,
  overflow- and failure-safe), demo (`examples/raw_memory_demo.cpp`), tests
  (`test_raw_memory`)
- [x] **Phase 3 — Alignment**: requirements and overflow-safe rounding helpers
  in [docs/phase3_alignment.md](docs/phase3_alignment.md)
  (`<mymalloc/alignment.h>`), demo (`examples/alignment_demo.cpp`), tests
  (`test_alignment`, incl. boundary sweep + raw-region integration)
- [ ] Phase 4+ — metadata, allocator core,
  strategies, fragmentation, debugging, stress tests, benchmarks

## Engineering rules

1. Implement one phase at a time.
2. Every feature needs tests.
3. Preserve alignment and metadata invariants.
4. Check integer overflow.
5. Separate OS memory acquisition from allocation policy.
6. Use sanitizers and stress tests.
7. Document architectural changes.
8. Do not mark a phase complete until its tests pass.