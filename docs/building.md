# Building

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

Jam requires Clang 23+, CMake 4.4+ and Ninja on macOS, Linux or Windows 10 1803+.
CMake checks the C++26 features and platform page size. It fetches a pinned
[native](https://github.com/ekmett/native) for SIMD, target dispatch and attributes.
To use an existing checkout, set `FETCHCONTENT_SOURCE_DIR_JAM_NATIVE=/path/to/native`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/jam
```

Tests default on when building Jam as the top-level project. Exceptions are
enabled; see [generation limits](generations.md) for failure behavior.

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

Set `CMAKE_PREFIX_PATH` to the installation prefix. The package includes native
and module sources; CMake regenerates consumer BMIs after installation or
relocation. The compiler, standard library, exception mode and extension mode
must match the module build. `jam::jam` supplies C++26.

With Clang 23.1.2 and libc++ on macOS, include `<new>` before importing Jam.
Without it, the README example crashes the compiler while emitting libc++'s
allocation helper. The example includes it for that reason.

For cross compilation, set `JAM_PAGE_BYTES` to the target's page size. The heap
checks it at runtime. `JAM_CONTEXT_X28=ON` selects the optional AArch64
[register carrier](collector.md); TLS is the default.

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

With Doxygen 1.18+ and Graphviz installed:

```sh
cmake -S . -B build -DJAM_BUILD_DOCS=ON
cmake --build build --target jam-docs
```

Open `build/docs/html/index.html`. It includes the README, these guides, API
contracts and source links. XML is in `build/docs/xml`. Warnings fail the build;
each run replaces generated output so deleted declarations leave no stale pages.

CI builds the library, runs tests and builds the docs. Successful `main` builds
publish the [reference](https://ekmett.github.io/jam/); pull requests validate it
without publishing.

[Topic guides](README.md)
