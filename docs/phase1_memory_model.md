# Phase 1 — Process Memory Model

This document captures the memory-model study required by Phase 1 of
[the roadmap](mymalloc_TODO.md). Everything here is observable from ordinary
user-space code; the claims that can be checked programmatically are asserted
by `tests/test_memory_layout.cpp` and demonstrated by
`examples/memory_layout.cpp`.

## 1. Virtual address spaces

- Every running process owns its own **virtual address space**: the set of
  addresses its pointers may use. The same numeric pointer value refers to
  *different* physical memory in different processes.
- The OS + CPU (via the MMU and page tables) translate virtual addresses to
  physical RAM. A virtual address that has no mapping causes a hardware
  fault — typically `SIGSEGV` / an access violation.
- On 64-bit Linux/x86-64 the user-visible range is
  `0x0000000000000000`–`0x00007fffffffffff` (47 bits); the upper half belongs
  to the kernel. On 32-bit systems the whole 4 GB is split similarly.
- **ASLR** (address space layout randomization) randomizes the load address of
  the binary, libraries, stack, and mmap regions on every run — so absolute
  addresses differ between runs, but the *relative layout* described below
  stays the same.

## 2. Typical 64-bit Linux process layout

```text
high addresses
┌──────────────────────────────┐ 0x00007fffffffffff
│ kernel space (not accessible)│
├──────────────────────────────┤
│ stack            (grows ↓)   │  local variables, call frames
├──────────────────────────────┤
│ ...            (huge gap)    │
├──────────────────────────────┤
│ mmap regions                 │  shared libraries, file mappings,
│                               │  large malloc blocks, thread stacks
├──────────────────────────────┤
│ ...            (huge gap)    │
├──────────────────────────────┤
│ heap             (grows ↑)   │  malloc/new arena (brk area)
├──────────────────────────────┤
│ .bss                         │  zero-initialized globals
├──────────────────────────────┤
│ .data                        │  initialized globals
├──────────────────────────────┤
│ .rodata                      │  constants (string literals, const arrays)
├──────────────────────────────┤
│ .text                        │  executable code (functions)
└──────────────────────────────┘ 0x0000555555554000 (typical PIE base)
│ unmapped                     │
└──────────────────────────────┘ 0x0
```

## 3. Segments of the binary

| Segment | Contents | Example symbol in the experiment |
|---------|----------|----------------------------------|
| `.text` | compiled function code | `probe_function()` |
| `.rodata` | constants, string literals | `g_rodata_global` |
| `.data` | globals with a non-zero initializer | `g_data_global = 42` |
| `.bss` | globals with zero/omitted initializer | `g_bss_global[16]` |

- `.data` and `.bss` are **static storage duration**: they exist for the whole
  program run, at a fixed address.
- `.bss` does not occupy space in the executable file; the loader maps it as
  anonymous zero pages at startup — which is why `g_bss_global` reads as all
  zeros without any explicit initialization code.
- Objects with **automatic storage duration** (locals, function parameters,
  temporaries) live on the **stack**.

## 4. The stack

- Each thread has exactly one stack. Calls push a **frame** (return address,
  saved registers, locals); returns pop it.
- The stack grows **towards lower addresses** on x86/x86-64 (and most other
  common ISAs). The experiment verifies the direction empirically with three
  nested frames — deeper frames are at consistently lower (or, on exotic
  targets, consistently higher) addresses than shallower ones.
- Stacks are small (typically 1–8 MiB per thread) and managed by the OS
  (guard pages, automatic growth within limits). Allocators must **never**
  hand out stack memory.
- Because frames are pushed/popped, stack addresses are reused constantly —
  "the address was unique once" is not an invariant.

## 5. The heap

- The **heap** is the region malloc/new draw from. On Linux the main heap
  starts just above `.bss` and grows **upwards** via the `brk`/`sbrk` system
  calls; allocations above a threshold (~128 KiB in glibc) are instead served
  by `mmap`-ed regions.
- glibc keeps its own metadata for every block — that bookkeeping is exactly
  the kind of structure mymalloc will rebuild from scratch (Phases 4+).
- Heap addresses are far away from the current stack pointer (typically
  hundreds of GB of unmapped gap on 64-bit), so the two regions cannot
  silently collide; the experiment prints the observed gap.

## 6. Pages and page size

- Virtual memory is mapped in **page**-sized units. The hardware MMU translates
  one page (virtual) ↔ one page frame (physical) at a time.
- The **page size** is reported by `sysconf(_SC_PAGESIZE)` (POSIX) or
  `GetSystemInfo().dwPageSize` (Windows). x86/x86-64 default to **4096 bytes**;
  ARM64 commonly uses 16 KiB or 64 KiB. "Huge pages" (2 MiB / 1 GiB) exist as
  an optimization but the *minimum* granularity is the base page size.
- Implications for an allocator:
  - whole-region acquisition/release (`mmap`/`munmap` in Phase 2) must be
    **page-granular**;
  - returned pointers must additionally satisfy alignment requirements
    (Phase 3), which are independent of — but often a multiple of — page size;
  - touching a previously untouched page triggers a real physical
    allocation ("page fault") — the OS lazily backs mappings.

## 7. `/proc/self/maps` (Linux)

`/proc/self/maps` is a text file the kernel generates per process, listing
every mapping in the address space. Each line is:

```text
address           perms offset  dev   inode pathname
7f8a2c000000-7f8a2c021000 rw-p 00000000 00:00 0
55ed9a4b4000-55ed9a4b5000 r--p 00000000 fd:00 12345  /usr/bin/app
55ed9a4b5000-55ed9a4c9000 r-xp 00001000 fd:00 12345  /usr/bin/app   ← .text
55ed9a4c9000-55ed9a4d1000 r--p 00001500 fd:00 12345  /usr/bin/app   ← .rodata/.data
55ed9a4d1000-55ed9a4d3000 rw-p 00002000 fd:00 12345  /usr/bin/app   ← .data/.bss
55ed9a4d3000-55ed9a4d5000 rw-p 00000000 00:00 0      [heap]
7ffd6a2b1000-7ffd6a2d2000 rw-p 00000000 00:00 0      [stack]
```

- Fields: `start-end` hex addresses, permissions (`rwx`, `p` private / `s`
  shared), file offset, device, inode, and the mapped file or pseudo-name
  (`[heap]`, `[stack]`, `[vdso]`, ...).
- Every address the program can legally dereference lies inside some line's
  range — the experiment and the test both verify that symbols from each
  region (code, data, bss, heap, stack) are covered.
- The `[heap]` marker names the `brk` area only; `mmap`-backed allocations
  appear as anonymous `rw-p` lines above it.

### Windows equivalents

Windows has no `/proc`; the same information is available via
`VirtualQuery`/`VirtualQueryEx`, the System Informer/VMMap tools, or
`GetSystemInfo` for the page size. `tests/test_memory_layout.cpp` uses
`GetSystemInfo` for the page-size check and keeps the layout probes
portable, so the invariants hold on the Windows CI job too.

## 8. Experiments and tests in this repository

| Artifact | Purpose |
|----------|---------|
| `examples/memory_layout.cpp` | Prints addresses of one symbol per region, the observed stack-growth direction, heap/stack gaps, and (on Linux) dumps and annotates `/proc/self/maps`. |
| `tests/test_memory_layout.cpp` | Asserts the machine-checkable invariants below; runs on Linux (GCC/Clang) and Windows (MSVC) in CI. |

Run the experiment:

```sh
cmake --build --preset debug
./out/build/debug/examples/memory_layout      # Windows: out\build\debug\examples\memory_layout.exe
```

## 9. Invariants captured by the tests

1. The system page size is a power of two between 256 B and 16 MiB.
2. One representative object per region (`.text`, `.rodata`, `.data`, `.bss`,
   heap, stack) has a **distinct** address.
3. Static-storage objects keep the **same address across calls**.
4. `.bss` objects read as zero at program start.
5. Stack frames grow in one consistent direction (deeper frames strictly
   ordered against shallower ones).
6. `std::malloc` results are aligned to `alignof(std::max_align_t)` and
   distinct from each other and from all static/stack objects.
7. Addresses of globals are naturally aligned (`alignof(T)`).
8. On Linux: `/proc/self/maps` is readable, and the stack, heap, and global
   addresses all fall inside some listed mapping.

## 10. What mymalloc takes from this phase

- Phase 2 will reserve/release raw memory; the OS primitives we study there
  (`mmap`/`munmap`, or `VirtualAlloc` on Windows) work in **page-sized
  units**, as established in §6.
- Allocator metadata (Phase 4) must live *inside* the regions we allocate —
  never in `.data`/globals beyond a single fixed root — so that the allocator
  remains a pure function of the address space it manages.
- The stack/heap separation justifies why `my_malloc` can only ever return
  heap (region) memory, and why validation of foreign pointers (Phase 14)
  can rely on mapping lookups.

## Limitations

- Absolute addresses vary per run (ASLR) and per platform; the tests assert
  relationships, never specific values.
- The classic layout is x86/x86-64-centric; other ISAs may grow the stack the
  other way (the tests accept either direction, only requiring consistency).
- Windows segment names (`.text`/`.data`/`.bss` still exist in PE files) map
  onto `VirtualQuery` regions rather than `/proc` lines; the experiment marks
  this clearly when run on Windows.