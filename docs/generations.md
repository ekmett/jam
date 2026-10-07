# Generations and sizing

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

The high bit of a pointer selects its generation: old is zero, young is one.
The other 31 bits count eight-byte cells, giving each generation up to 16 GiB.
Old has the lower virtual address range. `mk<T>` allocates in young.

## Collection

| Call | Work |
| --- | --- |
| `collect_minor()` | Trace and compact young; leave old objects unmoved and untraced. |
| `collect_minor(true)` | Trace young and append its survivors to old. |
| `collect_major()` | Trace both generations and compact all survivors into old. |
| `collect()` | Count down to a major; collect young on the intervening calls. |

The same operations are available on an explicit heap. Promotion and full GC
empty young and clear the remembered set. Collection is explicit; allocation
does not trigger it automatically. Old garbage is reclaimed by full GC.

`heap_options::minor_collections` defaults to `7`: seven minors, then a major.
The countdown counts calls to `collect()`, not allocations. `0` means always
major. An explicit `collect_major()` resets the countdown. Explicit
`collect_minor()` calls, including promotion, leave it unchanged. Each heap has
its own countdown.

```cpp
heap heap{{.minor_collections = 7}};
```

Minor GC starts from young roots and remembered old source slots. It follows
young targets and stops at old ones. Remembering the slot rather than its target
means overwriting an edge does not keep all its previous targets alive.

Old-to-young `weak_ptr<T>` slots are remembered for rewriting only. They do not
seed marking. Minor GC clears them if their young target dies, forwards them if
it survives, and drops cleared entries. Weak references to old targets wait for
major GC to decide liveness.

## Writes into old

Constructing a young `ptr<T>` in old storage registers that source slot with
its target tracer. Assignment checks the XOR of the old and new generation bits:
changing one young target to another needs no table update. Crossing into young
registers the slot; crossing out removes it. Both virtual aliases name the same
entry. Minor GC reads its current value. An unreachable old object can still
retain young targets until the next full GC.

Moving a pointer nulls its source and updates the affected registrations. Member
and ADL `swap` exchange targets directly; only different generation bits require
barrier updates. Self-move and self-swap preserve the value.

Ending a pointer's lifetime removes its entry, including stale entries left by
unsafe writes. A union alternative replaced by scalar data therefore cannot
leave a stale typed root.

Arrays use the scalar barriers. Pointer SIMD assignments and stores register
lanes in bulk; wide packs do so per register. `jam::assign` provides explicit
bulk assignment for arrays, SIMD and wide values.

For caller-managed bulk writes, `ptr::unsafe_assign` and `jam::unsafe_assign`
copy encoded offsets without a barrier. Before collecting, register every
old-to-young destination with `heap.remember(slot)`,
`heap.remember(span_of_slots)`, or a pointer vector's `.remember()`. The unsafe
operation alone is not enough. `remembered_size()` reports the retained slots.

## Options

```cpp
import jam.unqualified;

heap h{{.old = {.capacity = 8_MiB, .reserve = 1_MiB, .maximum = 1_GiB},
        .young = {.capacity = 8_MiB, .reserve = 1_MiB, .maximum = 64_MiB},
        .workers = 4}};
```

Each generation has its own initial `capacity`, compaction `reserve`, `maximum`
and `shrink_shift`. Maximum reserves virtual addresses; backing grows as needed.
The default is 1 MiB capacity, 256 KiB reserve and a 16 GiB maximum per generation.
Capacity must hold at least twice the reserve. Any whole-page count works.

Backing grows by doubling up to the maximum. Sparse collections can shrink once
toward half capacity, rounded to whole pages and bounded below by twice the
reserve. `shrink_shift = 2` shrinks below one-quarter full; `3` means one-eighth,
and `0` disables shrinking.

One persistent worker pool serves both generations and both collection phases.
`workers` includes the caller and must be positive. `configuration()` reports the
initial policy. `old()` and `young()` expose each arena's cell counts and views.
The legacy raw `heap.allocate()` and arena accessors refer to old.

A generation size above 16 GiB throws `std::length_error` before mapping. Resource
failure or exceeding the configured maximum during allocation/GC terminates;
a moving collection cannot unwind.

## Units

Sizes use `jam::units::pages`. MiB convert exactly to platform pages; smaller
units may need `ceil<pages>`. The literals retain their units, rather than quietly
turning a byte count into a `size_t`.

```cpp
using namespace jam::units;

bytes size = 1536_KiB;
auto truncated = space_cast<mebibytes>(size); // 1 MiB.
auto rounded = ceil<mebibytes>(size);        // 2 MiB.
auto reserve = ceil<pages>(256_KiB);
auto total = 1_MiB + 512_KiB;
auto fractional = space<double, mebi>{size}; // 1.5 MiB.
```

`space<Rep, Ratio>` follows `std::chrono::duration`: explicit count construction,
implicit exact unit conversions, common-unit arithmetic, comparisons and
`.count()`. `Ratio` measures bytes. Integral and floating representations work;
there is no implicit conversion to an integer. Integer overflow is checked.
`space_cast` truncates integral results; `floor`, `ceil` and `round` provide the
other choices, with ties to even for `round`.

Literals are `_B`, `_kB`, `_MB`, `_GB`, `_KiB`, `_MiB` and `_GiB`, available through
`jam::units`, alongside the unit types and conversions. `jam.unqualified` exposes
both the types and literals without qualification. Page size
is a build invariant; cross builds supply `JAM_PAGE_BYTES`.

[Mapping and compaction](collector.md) · [Topic guides](README.md)
