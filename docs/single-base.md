# Single-base addressing

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

Accepted as the plan of record on 2026-10-07. The prototype has been measured;
production integration is still to do.

Give each heap a canonical 32 GiB address window. Old starts at its base, young
starts 16 GiB above it. Our existing pointer encoding then decodes directly:

```cpp
base + std::size_t(bits) * 8
```

The high bit already supplies the displacement to young. We keep four-byte
pointers, the reserved null cell in each generation, and the existing per-generation
capacity limits. Reserving the window does not commit 32 GiB of backing.

Keep the private double mappings for compaction. Publish their logical contents
into the canonical window, then republish after rotation or resizing before
mutators resume. Ordinary dereferences use the canonical view. Explicit arena
views can still expose the private mappings. All aliases of an old pointer slot
must name the same remembered-set entry.

The heap owns this mapping. Operations continue to find it through
`heap::current()`; pointers and roots do not acquire a cached heap or base address.
Borrows still expire on growth or collection.

## Why take the trade?

The shorter decoder removes a dependency from pointer chasing. The selected
generation's base no longer has to wait for the pointer's tag. Publication costs
something, including page-table work paid on the next access, so timing only the
mapping call misses part of the bill.

The prototype was compared with `1696e6b` using the existing chain, tree and raw
compaction benchmarks. These are million-node results with one worker, medians
of 27 samples. GC plus the first walk includes publication and subsequent access
costs; warm walks show what repeated traversal can recover.

| Machine | Workload | Warm walk | GC + first walk |
| --- | --- | --- | --- |
| Intel i9-12900K / Linux | Chain, major | 57% faster | 4.5% faster |
| Intel i9-12900K / Linux | Tree, major | 12.5% faster | 0.9% slower |
| Intel i9-12900K / Linux | Tree, minor | 11.5% faster | 1.2% slower |
| Apple M3 / macOS | Chain, major | 13% faster | 5.6% slower |
| Apple M3 / macOS | Tree, major | 5.2% faster | 2.0% slower |
| Apple M3 / macOS | Tree, minor | 1.2% slower | 4.1% slower |

This is a trade for cheaper traversal, not a claim that every collection gets
faster. These graphs are entirely live and settle into one generation. Growth
and mixed-generation correctness were tested, but their performance needs a
separate measurement. Both variants passed the 12-test suite on both machines.

## Integration

- Own and release the canonical reservation with the heap. Reuse publication
  machinery on Linux and macOS; complete Windows alias publication and teardown.
- Republish after rotation, growth and shrink. Revoke discarded alias tails,
  preserve null cells, and cover promotion and host-driven collection epochs.
- Route scalar decoding through the canonical base. Keep the accessor inline
  and pure. Recognize canonical and private addresses in raw claims and barriers.
- Lift the same layout into SIMD member gathers: use the young base as the
  midpoint and signed indices formed by flipping the generation bit, with scale
  eight. Preserve null masks and bulk remembered-set contracts.
- Retain tests for all aliases, growth, shrink, polymorphic claims, weak
  references, finalizers and external hosts. Recheck generated code and repeat
  end-to-end benchmarks on each supported platform before integration is complete.

The decision is to make this the default addressing scheme. The SIMD gather
change and Windows integration are planned work, not results of the measured
prototype. Unifying metadata remains a separate experiment: this decision does
not change the forwarding tables or claim a saving in their gathers.
