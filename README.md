# mymalloc — User-Space Memory Allocator

An educational C++20 memory allocator: raw OS memory, alignment, block
metadata, free lists, splitting/coalescing, `malloc`/`free`/`calloc`/`realloc`,
fragmentation analysis, corruption detection, stress testing, and benchmarking.

See [docs/architecture.md](docs/architecture.md) for the design and
[docs/api.md](docs/api.md) for the public API reference.

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
├── scripts/                # developer tooling (format, clang-tidy driver)
├── docs/                   # architecture and API reference
└── .github/workflows/      # CI (build, test, sanitizers, format, static analysis)
```

## Target API

```cpp
void* my_malloc(std::size_t size);
void  my_free(void* ptr);
void* my_calloc(std::size_t count, std::size_t size);
void* my_realloc(void* ptr, std::size_t new_size);
```

Declared in [`include/mymalloc/mymalloc.h`](include/mymalloc/mymalloc.h)
(C linkage, usable from C and C++). All four are implemented.

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
| `tsan`        | Debug      | ThreadSanitizer              |

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
| `MYMALLOC_SANITIZER`          | `off`   | `address`, `undefined`, `address-undefined`, or `thread` |

Example:
`cmake --preset debug -DMYMALLOC_SANITIZER=address-undefined -DMYMALLOC_WARNINGS_AS_ERRORS=ON`

> Sanitizers are best supported on Linux (GCC/Clang). MSYS2/MinGW does not
> ship the ASan/UBSan/TSan runtimes for its toolchains, so on Windows use WSL
> or rely on the CI sanitizer jobs (ASan + UBSan and TSan on Ubuntu).

### Installing and consuming the package

```sh
cmake --install out/build/release --prefix /path/to/prefix
```

This installs the static library, every public header (including the generated
`config.hpp`), and a CMake package. Consumers use the exported target:

```cmake
find_package(mymalloc 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE mymalloc::mymalloc)
```

`mymalloc::mymalloc` carries the C++20 and `Threads::Threads` requirements, so
consumers do not repeat them.

## Strict warnings

All targets are compiled with strict warnings: `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align ...`
(`-Werror` optional via `MYMALLOC_WARNINGS_AS_ERRORS=ON`; MSVC uses
`/W4 /permissive-`). CI builds with `MYMALLOC_WARNINGS_AS_ERRORS=ON`, so the
tree is warning-free.

## Testing

Tests are plain executables registered with CTest, using the tiny harness in
[`tests/test_framework.hpp`](tests/test_framework.hpp) (no external
dependencies). Current tests:

- `test_version` — generated version metadata matches the exported helpers
- `test_public_header` — public header exposes exactly the target API
  (compile-time signature checks, include-guard and `size_t` compatibility)
- `test_memory_layout` — process address-space invariants
- `test_raw_memory` — page-rounded acquire/release, failures, repeated
  cycles
- `test_alignment` — rounding boundaries, overflow, raw-region integration

- `test_block` — header invariants, conversions, corruption detection

- `test_allocator` — `my_malloc`/`my_free` contract: alignment, isolation,
  failures
- `test_free_list` — free-list invariants, first-fit, reuse
- `test_split` — splitting, remainder insertion, reuse boundary cases
- `test_coalescing` — `my_block_merge`, neighbor fusion, split/free round-trip,
  fragmentation reduction
- `test_strategy` — `my_free_list_best_fit`, strategy selector, end-to-end
  first-fit vs best-fit placement
- `test_calloc` — zero-init, multiplication-overflow rejection, reused-block
  zeroing
- `test_realloc` — in-place resize (fits/absorb), moves, failure leaves the
  original intact, data preservation
- `test_regions` — region tracking, multi-region scavenge-on-acquire, guarded
  release (invalid-unmap refusal), long-cycle registry bounds
- `test_fragmentation` — heap statistics, internal/external fragmentation,
  derive-from-ground-truth walk, collect contract
- `test_debug` — pointer validation, heap-consistency checker, usage/diagnostic
  counters, opt-in canaries, invalid/double-free detection
- `test_stress` — deterministic randomized alloc/free/realloc over mixed sizes,
  data-integrity verification, three-way accounting cross-check, leak audit

- `test_threads` — concurrent malloc/free/realloc, introspection racing
  mutation, strategy toggling
- `test_benchmark` — benchmark invariants: deterministic workload, clean drains,
  reuse performs no OS acquisition

Run everything with `ctest --preset debug`, or a single test:

```sh
ctest --preset debug -R test_version
```

## Examples

The repository ships runnable demos under `examples/` (build with
`MYMALLOC_BUILD_EXAMPLES=ON`, the default). Run any from its build directory,
e.g. `out/build/release/examples/fragmentation_demo`:

- `hello_mymalloc` — version smoke test
- `memory_layout` — process address-space layout
- `raw_memory_demo` — page-granular acquire/release
- `alignment_demo` — overflow-safe rounding
- `block_demo` — headers, conversions, corruption detection
- `allocator_demo` — first `my_malloc`/`my_free`
- `free_list_demo` — free-list reuse
- `split_demo` — splitting
- `coalescing_demo` — neighbour fusion
- `strategy_demo` — first-fit vs best-fit
- `calloc_demo` — zero-init + overflow checks
- `realloc_demo` — in-place / move resizing
- `regions_demo` — region registry + scavenge
- `fragmentation_demo` — live fragmentation metrics
- `debug_demo` — diagnostics, ownership, canaries
- `stress_demo` — narrated randomized workload
- `benchmark` — hot-path ns/op + strategy comparison

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
4. **thread-sanitizer** — TSan
5. **format** — clang-format check
6. **static-analysis** — cppcheck
7. **clang-tidy** — bugprone/analyzer/performance/portability checks

## Documentation

- [docs/architecture.md](docs/architecture.md) — design, invariants, and
  limitations
- [docs/api.md](docs/api.md) — public API reference
- [CHANGELOG.md](CHANGELOG.md) — release history

## Engineering rules

1. Implement one component at a time.
2. Every feature needs tests.
3. Preserve alignment and metadata invariants.
4. Check integer overflow.
5. Separate OS memory acquisition from allocation policy.
6. Use sanitizers and stress tests.
7. Document architectural changes.
8. Do not mark a component complete until its tests pass.
