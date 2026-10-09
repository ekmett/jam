# Building

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

Jam requires Clang 23+, CMake 4.4+ and Ninja on macOS, Linux or Windows 10 1803+.
CMake checks the C++26 features and platform page size. It uses installed
[native](https://github.com/ekmett/native) and [work](https://github.com/ekmett/work)
packages when available, otherwise fetching their current `main` branches.
Native supplies SIMD and target dispatch; an installed Native must include its
host modules and have exceptions enabled.
[Hint](https://github.com/ekmett/hint) supplies the textual `<hint.h>` attribute
catalog through `hint::hint`; Native supplies the dependency.
[work](https://github.com/ekmett/work) supplies the shared worker pool and typed
gigs through `work::work`.
To use an existing checkout, set `FETCHCONTENT_SOURCE_DIR_JAM_NATIVE=/path/to/native`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/jam
```

Tests default on when building Jam as the top-level project. Exceptions are
enabled; see [generation limits](generations.md) for failure behavior.

## Docker

The Linux x86-64 development image layers Jam and Work onto
`ghcr.io/ekmett/native:latest`, with Clang 23, CMake 4.4 and Ninja already present.
Jam and Work are installed in `/opt/jam`; Native and Hint are in `/opt/native`.
`CMAKE_PREFIX_PATH` includes both, so the CMake consumer below works directly.
The image contains the C++ library, not a JDK or GraalVM distribution.

```sh
docker run --rm -v "$PWD:/workspace" ghcr.io/ekmett/jam:latest \
  cmake -S . -B build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Build it locally with `docker build --pull -t jam .`. The separate Docker
workflow pulls Native's current base and rebuilds the library stage each run,
so Work follows `main` even when the toolchain layers are cached. It runs the
native tests, then compiles and runs an independent installed-package consumer.
Pull requests test the image; successful `main` builds publish it to
[GHCR](https://github.com/ekmett/jam/pkgs/container/jam). Use the workflow's manual
trigger to refresh dependencies between Jam changes. External toolchain and
action locks remain independent of this in-house dependency policy.

## Coverage and test analytics

Ordinary CI builds remain uninstrumented. A separate coverage workflow runs on
every `main` commit using the same reusable build workflow, adding Linux ARM64,
Windows ARM64 and macOS ARM64 to the normal Linux x86-64 and Windows x86-64
configurations. Windows x86-64 retains Debug and Release. Coverage also builds
the VM C ABI adapter and its native tests; it does not build JDKs or GraalVM.

For a local report, install grcov and LLVM's matching `llvm-profdata` and
`llvm-cov` tools alongside Clang, then use a separate build directory:

```sh
cmake -S . -B build-coverage -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DJAM_BUILD_TESTS=ON -DJAM_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel
ctest --test-dir build-coverage --output-on-failure --output-junit tests.xml
cmake --build build-coverage --target jam_coverage
```

The report target consumes the test profiles without rebuilding or rerunning
tests. Module providers and consumers share one instrumented dependency build.
Assembly/codegen consumers remain uninstrumented. Reports cover Jam's `src/`
and, when enabled with `JAM_BUILD_VM=ON`, `vm/src/`; dependency sources are
excluded. `build-coverage/coverage/report/coverage.info` preserves LLVM's
canonical LCOV branch counts. grcov generates browsable HTML from that file;
its derived branch metrics are not uploaded. On macOS, VM builds also need
`JAM_VM_RUNTIME` to point to the matching C++ runtime library directory.

CI retains reports and uploads coverage plus available CTest JUnit results to
Codecov using GitHub OIDC. Test results are uploaded after failures as well.
Codecov repository activation is required for the uploads to succeed.

## Nix

The separate Nix workflow checks Linux x86-64 and ARM64 packages, including an
installed-package consumer. Native supplies LLVM 23 and the pinned external
Nixpkgs toolchain; Native, Hint and Work follow `main` and are refreshed before
each CI check. Native exceptions remain enabled.

```sh
nix flake update native native/hint work
nix flake check --print-build-logs
nix develop
```

## Windows

Use `clang-cl`, matching `clang-scan-deps`, and LLD from LLVM 23, in a Visual
Studio developer shell with the MSVC C++ library and Windows SDK.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI exercises Debug and Release. [Mapping details](collector.md) cover Windows
page size, aliases and commit behavior.

## CMake consumers

```cmake
cmake_minimum_required(VERSION 4.4)
project(example LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(jam CONFIG REQUIRED)
add_executable(example example.cc)
target_link_libraries(example PRIVATE jam::jam)
```

Set `CMAKE_PREFIX_PATH` to the installation prefixes for Jam, native, Hint and
work.
Jam installs its module sources; CMake regenerates consumer BMIs after
installation or relocation. The compiler, standard library, exception mode and
extension mode must match the module build. `jam::jam` supplies C++26.

With Clang 23.1.2 and libc++ on macOS, include `<new>` before importing Jam.
Without it, the README example crashes the compiler while emitting libc++'s
allocation helper. The example includes it for that reason.

For cross compilation, set `JAM_PAGE_BYTES` to the target's page size. The heap
checks it at runtime. `JAM_CONTEXT_X28=ON` selects the optional AArch64
[register carrier](collector.md); TLS is the default.

## Module layout

`import jam` provides the scalar heap API. Add `import jam.simd` for pointer
vectors and gathers. The implementation uses independently
importable modules, so a consumer can name the layer it needs:

| Module | Contents | Jam dependencies |
| --- | --- | --- |
| `jam.units` | Space quantities, conversions and literals | None |
| `jam.mapping` | OS reservations and circular mappings | None |
| `jam.packed` | Rank metadata and compactor interface | None |
| `jam.heap` | Heap, pointers, roots, tracing and collection | Units, mapping, packed; external work |
| `jam.simd` | SIMD pointer storage and gathers | Jam |
| `jam` | Scalar API and current-heap convenience functions | Heap, units |
| `jam.unqualified` | Scalar API with names in scope | Jam |

Mapping and packing live in `jam::detail`; their module names
provide compilation boundaries, not a promise of stable public internals.
`jam.packed` imports only `native.isa` in its interface. Its SIMD kernels compile
separately against `native`, alongside code that uses the compactor interface.
`jam.simd` re-exports `jam` and `native.simd`; its x86 gathers privately import
`native.x86.memory`. The compactor imports `native.simd` and `native.features`,
plus `native.x86.bmi2`, `native.x86.memory` and `native.x86.vpopcntdq` on x86.
CPU inspection in application code requires `import native.features;`.
The scalar API does not re-export native. The current native CMake target still
builds all its providers; targeted imports shorten dependency chains without
pruning that target's source list.

Tracing declarations, heap lifecycle, collection, root bookkeeping and finalizers
compile in `jam.heap`; there are no module partitions. Scheduling lives in the
independent `work` module. Each marking pass creates a typed gig; its batch
handler binds the heap once, drains tracing jobs, and permits child submissions
after the external seed set closes. VM caller-only passes stay on their caller.
Pointer decoding, barriers, allocation and tracing templates stay available for
inlining. `jam.heap` provides explicit heap operations; `jam` adds `mk`,
`mk_weak`, bulk array assignment and the current-heap `collect` functions.

## Benchmarking

Enable `JAM_BUILD_BENCHMARKS=ON` to build `heap-bench`:

```sh
build/heap-bench --bytes 16MiB --workers 4 --reserve-list 32,64,128 --repeats 11
```

This times compaction after marking, excluding allocation, tracing and fixture
setup. It verifies relocated payloads and pointer fields after each trial. It
is not an end-to-end GC benchmark. Tests compare every compactor admitted on the
host with the baseline, including mixed-generation forwarding.

## Documentation

With Doxygen 1.18+, Graphviz, Pandoc 3.8+ and Python 3 installed:

```sh
cmake -S . -B build -DJAM_BUILD_DOCS=ON
cmake --build build --target jam-docs
```

Open `build/site/index.html`. It includes the README, these guides, API
contracts and source links. XML is in `build/docs/xml`. Warnings fail the build;
each run replaces generated output so deleted declarations leave no stale pages.

CI builds the library, runs tests and builds the docs. Successful `main` builds
publish the [reference](https://ekmett.github.io/jam/); pull requests validate it
without publishing.

[Topic guides](README.md)
