# Topic guides

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

Start with the [example](../README.md). These pages explain the contracts and the
parts you might want to customize.

- [Tracing](tracing.md): manifests, dynamic layouts and cooperative walks.
- [Lifetimes and storage](lifetimes.md): roots, heap scopes and byte relocation.
- [Weak associations and finalizers](finalizers.md): conditional values and post-GC callbacks.
- [Generations and sizing](generations.md): minor collection, promotion, barriers and units.
- [Pointer vectors](simd.md): SIMD storage, masks and gathers.
- [Inside the collector](collector.md): marking, work donation, compaction and mappings.
- [Building](building.md): toolchains, CMake consumers, benchmarks and Doxygen.
