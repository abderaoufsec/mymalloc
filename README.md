# mymalloc

A compact, educational C++20 user-space memory allocator. It builds
`my_malloc`, `my_free`, `my_calloc`, and `my_realloc` on raw OS pages, with
alignment, block metadata, free lists, splitting/coalescing, region tracking,
heap statistics, corruption checks, stress tests, and benchmarks.

[Architecture](docs/architecture.md) | [API](docs/api.md) | [Changelog](CHANGELOG.md)

## Requirements

- CMake 3.20+ (3.25+ for presets)
- C++20: GCC 11+, Clang 14+, or MSVC 19.29+

## Quick start

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan`, `ubsan`, `asan-ubsan`, `tsan`.
Build and test presets share names; output goes to `out/build/<preset>/`.

Manual build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## API

```cpp
void* my_malloc(std::size_t size);
void  my_free(void* ptr);
void* my_calloc(std::size_t count, std::size_t size);
void* my_realloc(void* ptr, std::size_t new_size);
```

C linkage, declared in [`include/mymalloc/mymalloc.h`](include/mymalloc/mymalloc.h).
See the [API reference](docs/api.md) for contracts and diagnostics.

## Configuration

| Option | Default | Purpose |
|--------|---------|---------|
| `MYMALLOC_BUILD_TESTS` | `ON` | Build the CTest suite |
| `MYMALLOC_BUILD_EXAMPLES` | `ON` | Build the examples |
| `MYMALLOC_WARNINGS_AS_ERRORS` | `OFF` | Treat warnings as errors |
| `MYMALLOC_SANITIZER` | `off` | `address`, `undefined`, `address-undefined`, or `thread` |

```sh
cmake --preset debug -DMYMALLOC_SANITIZER=address-undefined -DMYMALLOC_WARNINGS_AS_ERRORS=ON
```

Sanitizers are best supported on Linux; on Windows use WSL or the CI jobs.

## Install

```sh
cmake --install out/build/release --prefix /path/to/prefix
```

```cmake
find_package(mymalloc 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE mymalloc::mymalloc)
```

The exported target provides C++20 and `Threads::Threads`.

## Testing and examples

The suite contains 19 dependency-free CTest executables. Run all tests or one
suite:

```sh
ctest --preset debug
ctest --preset debug -R test_realloc
```

Examples are built by default and live in `out/build/<preset>/examples/`.
See [`tests/`](tests/) and [`examples/`](examples/) for the full inventory.

## Tooling and CI

```sh
python scripts/check_format.py          # check formatting
python scripts/check_format.py --fix    # apply formatting
```

Static analysis uses `.clang-tidy` via `scripts/run_clang_tidy.py` and
`cppcheck` in CI. CI covers GCC, Clang, and MSVC debug/release builds, ASan +
UBSan, TSan, formatting, clang-tidy, and cppcheck.

## Contributing

Keep the tree warning-free, add tests for behavior changes, check integer
overflow, and preserve the alignment and metadata invariants documented in
[`docs/architecture.md`](docs/architecture.md).

## License

MIT. See [`LICENSE`](LICENSE).
