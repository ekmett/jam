# Pointer vectors

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

`import jam.simd;` enables pointer vectors and gathers, and also provides the
scalar Jam API and `native.simd`. Import `native.features` separately for CPU
inspection. `import jam;` alone does not include SIMD.

`native::simd<ptr<T>, N, A>` stores actual pointer subobjects. Put the vector in a
manifest and its lanes are traced and forwarded in place. `native::wide<V, K>`
visits each register. Numeric vectors remain ordinary data. Both can be complete
heap records or embedded members.

Pointer vectors offer lane access, typed `load`/`store`, raw encoded offsets via
`to_native()`, and lane-wise `==`/`!=` against vectors or null. Assignments and
stores obey the [old-to-young barrier](generations.md).

## Gather

`gather` is a free function found through ADL. Pass a mask when several reads
should share a null check; omit it to use `nodes != nullptr`.

```cpp
struct point {
  ptr<point> next;
  float x, y;
  static constexpr auto manifest = make_manifest<point>(&point::next);
};

using P = native::simd<ptr<point>, 4>;
P nodes{std::array<ptr<point>, 4>{a, b, nullptr, a}};
auto active = nodes != nullptr;
auto xs = gather(nodes, &point::x, active);
auto ys = gather(nodes, &point::y, active);
auto next = gather(nodes, &point::next);

native::wide<P, 4> packets{nodes};
auto packed_xs = gather(packets, &point::x);
```

Here `a` and `b` are live pointers in the current heap. Selected lanes must be
nonnull and valid. Inactive lanes are not accessed and return zero or null.
Gathering a pointer field says nothing about whether that child is nonnull.

The wide overload forms all register masks before gathering. An explicit
`native::wide<mask_type, K>` can be supplied as the third argument. Lane and
register order are preserved.

Single, nonvirtual inheritance is supported, including classes with virtual
functions. For lanes of `ptr<Derived>`, an inherited `&Base::field` is converted
to the corresponding `Derived` member pointer before its displacement is used.
This is a fixed offset, not a virtual call or a per-lane layout lookup. SIMD and
wide overloads accept the same inherited member syntax.

Fields can be 32/64-bit integers, `float`, `double` or `ptr<U>`, provided native
supports the result shape. Wider fields may require fewer lanes; `wide` lets you
retain a larger batch. The ISA tag selects the implementation at compile time.
Call it from a kernel compiled for that ISA and admitted on the current CPU.

AVX2 and AVX512 use masked gathers; baseline and NEON read active lanes
individually. On x86, the generation bit divides the mask between two bases.
The remaining 31-bit index is scaled by eight. Clang's flat member-pointer ABI
supplies the field displacement: Itanium on Unix, flat MS on Windows. Extended
MS representations are rejected. Finding the displacement neither fabricates an
object nor extracts the active mask.

External vectors and gathered pointers become stale across collection, just
like scalar `ptr`s. Root a heap-resident vector and obtain a fresh copy afterward.

[Topic guides](README.md)
