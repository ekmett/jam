# jam

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A C++26 module for a double-mapped mark/compact heap. Typed references describe
managed edges; external roots register the objects that must survive collection.
Records have no collector header or runtime type tag.

`jam::oo` accepts `std::uint32_t` and `std::uint64_t` as object-offset types.
`heap<O>::ptr<T>` is exactly `jam::ptr<T, O>`. It holds one cell offset: four bytes
for `std::uint32_t`, eight for `std::uint64_t`. Visiting a field
carries its target type into the marking queue, which invokes `jam::tracer<T>`.
The default tracer calls the target's `trace` member when present. Types without
a hook are leaves: scalars and ordinary reference-free records need no boilerplate.

```cpp
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
import jam;

template<jam::oo O>
struct node {
  jam::ptr<node, O> next;
  std::uint64_t data;

  template<jam::visitor<O> V>
  constexpr auto trace(V & visit) const noexcept {
    return visit(next);
  }
};

using heap_type = jam::heap<>; // std::uint32_t offsets by default.
using node_t = node<heap_type::offset>;
static_assert(std::is_same_v<heap_type::ptr<node_t>, jam::ptr<node_t, std::uint32_t>>);
static_assert(sizeof(heap_type::ptr<node_t>) == 4);

int main() {
  heap_type heap{jam::options{
    .capacity_pages = 256,
    .reserve_pages = 64,
    .marking_workers = 4,
    .compaction_workers = 4
  }};

  static_cast<void>(heap.make<node_t>()); // Unreachable.
  auto const a = heap.make<node_t>(node_t{{}, 42});
  auto const b = heap.make<node_t>(node_t{a, 99});
  heap.store(a, node_t{b, 42});         // A cycle with inline data.
  jam::root<node_t> answer = heap.root(a); // External intrusive root registration.
  auto copy = answer;                    // An independent root hook.

  heap.collect();                        // a and b are now stale; use the roots.

  auto const first = heap.load(answer.get());
  auto const second = heap.load(first.next);
  assert(second.next == answer.get());
  assert(first.data == 42 && second.data == 99);
  assert(copy.get() == answer.get());
  assert(heap.used() == 5);               // One null cell plus two two-cell nodes.
}
```

`ptr<T, O>` can name an incomplete type, so recursive records need only their
member `trace` function. Typed heap operations check `jam::traceable<T, O>` once
`T` is complete: its tracer must be `noexcept` and support that offset type with
either alignment policy. `template<jam::visitor<O> V>` accepts visitors using `O`.
No superclass or separate tracer registration is needed for these records.

The intrusive object hook is `node::trace`. It visits only `next`. The inline
`std::uint64_t` data needs no visit and is never interpreted as a pointer.
For a type you cannot modify, specialize `jam::tracer<T>` with a static
`trace(Visitor &, T const &) noexcept` function. The default tracer forwards the
member hook's result; no particular return type is required. Collection currently
uses the hook's side effects.
Deriving that specialization from `jam::leaf<T>` explicitly suppresses traversal.
A record containing managed pointers must enumerate them: the default leaf convention
does not inspect arbitrary fields. Raw C++ pointers are data, not managed edges.

Allocation tracing claims the complete target and sets its live bits before
walking its contents. Cycles and sharing therefore invoke the hook once per
reachable record per collection. Even a leaf allocation needs those live bits.

The visitor walks parts of that already-live allocation. `visit(a, b, ...)`
visits each argument in order, by reference; `visit()` does nothing. It does not
claim embedded values again. `std::tuple` and `std::array` visit each element;
`std::variant` visits only its active alternative (none when valueless). These
adapters compose recursively and require every element or alternative to support
tracing with the visitor's offset type. Scalar visits do nothing.

Visiting a `ptr<T, O>` tags its location in the enclosing allocation's pointer
mask and queues its nonnull target for allocation tracing as `T`. Compaction uses
those marks to rewrite pointers; other live bytes remain data. Every edge to a record
must agree on its complete type. Base-subobject and interior references require
the raw API.

`make<T>(args...)` constructs and copies a value into the heap, returning an
unrooted `ptr<T>`. `load(ptr)` returns a value snapshot; `store(ptr, value)` replaces
the record without changing its type or extent. Types must be unqualified,
trivially copyable and standard-layout, with supported alignment. Construction
must be `noexcept`; jam does not run payload destructors. Tracing support does
not relax these storage requirements. In particular, standard-library tuples
need not be trivially copyable or standard-layout. `std::tie` provides a tuple
view of existing fields without storing a tuple in the heap when a tuple view
is useful; ordinary hooks can pass their fields directly to `visit`.

Tracing also uses a temporary value snapshot so typed field access does not
assume a persistent C++ object lifetime through VM alias changes or cell-wise
relocation. Visit ptr members **by reference from that supplied value**, including
members of nested subobjects. A copied ptr no longer identifies the field's
location. The value and visitor must not escape the callback. Leaf records need
no snapshot. Each traced non-leaf record is copied once for its hook.

Compact pointers are plain field values: copying one does not register a root.
`heap.root(ptr)` returns a `jam::root<T, O, may_dilate>` containing the intrusive
registration links and the type-specific tracer. For the default heap, this is
`jam::root<T>`. Its `get()` returns the current `heap::ptr<T>`.
Copying a root registers another hook; moving it transfers the hook without
allocating. The heap must outlive every attached root, including null roots.
Keep root objects outside the moving heap, since their registration links use
their addresses. The previous untyped root handle is named `heap::root_handle`.

**Outside the managed heap, only registered roots retain their meaning across
`heap.collect()`.** Do not keep a `ptr` on the stack or in an external container
for use after collection. This includes copies obtained from `root.get()` and
pointers inside snapshots returned by `heap.load()`. Keeping the target alive
through a root does not update those copies. After collection, obtain a fresh
pointer from the root and reload any snapshots before following their fields.

A default ptr is null, represented by zero. Cell zero is reserved across
allocation and compaction, so no live object can have the null offset. `used()`
includes this cell: an empty heap uses one cell. Null survives forwarding.
Unrooted pointers and pointers in loaded snapshots expire at the next moving
collection; retain a root when a value must survive. Allocation preserves offsets,
but borrowed `data()` pointers and cell references expire when the heap grows.
Pointers from different heaps must not mix. The offset type rejects mixing
compressed and wide pointers in root registration or typed field visits at compile time.

Mutation and changes to the root set stop during collection. Tracer hooks must
be `noexcept` and safe for concurrent invocation. Marking finishes before
compaction begins. `collect()` requires typed roots and edges; `collect(trace)`
also accepts untyped roots/edges, invoking its callback with a visitor and an
offset for those targets. That callback must claim each record and enumerate its
fields. Both forms redeclare pointer fields each collection, so a conditional
tracer can stop treating a field as a managed edge.

`heap<O, may_dilate>` chooses the storage layout at compile time:

| Template parameters | Managed reference | Rank block | Persistent metadata |
|---|---|---|---|
| `heap<std::uint32_t, false>` | 32 bits, scaled by eight | 256 bytes | 16 bytes per block |
| `heap<std::uint32_t, true>` | 32 bits, scaled by eight | 256 bytes | 16 bytes per block + one byte per 512 bytes |
| `heap<std::uint64_t, false>` | 64 bits, scaled by eight | 512 bytes | 24 bytes per block |
| `heap<std::uint64_t, true>` | 64 bits, scaled by eight | 512 bytes | 24 bytes per block |

Compressed pointers permit a 32 GiB heap and two managed fields in one cell. With
`may_dilate=false`, records have eight-byte alignment. Enabling it permits
16-, 32- and 64-byte alignment; marking and compaction retain alignment groups
as needed, and retained neighboring cells count toward the used size.

Constructor options set initial capacity, reserve `N`, marking parallelism and
compaction parallelism. Capacity and `N` are measured in native OS pages, not
rank blocks; capacity must be a power of two holding at least twice the reserve.
Worker counts must be positive and include the calling thread. One persistent
pool serves both collection phases.
Compaction advances at most `N` source pages past its earliest unfinished page.
Ordinary collections step backward into the reserve through coherent virtual
aliases. Allocation grows the power-of-two backing when needed; sparse
collections can shrink it. `shrink_shift` selects the occupancy threshold:
the default `2` shrinks below 1/4 full, `3` below 1/8, and `0` disables automatic
shrinking. One collection can halve capacity once when its live cells and reserve fit.

The lower-level `clear_marks`, `claim`, `mark`, `pointer` and `compact` operations
remain available for clients that provide their own tracing schedule. These
operations have the same frozen-heap requirements as `collect`.

## Build and use

Jam requires macOS or Linux, CMake 3.30+, Ninja and a compiler with C++26 modules.
Configuration probes the language features used by the implementation. No
exceptions are enabled; resource failures terminate. Jam uses only native's
textual [attribute catalog](https://github.com/ekmett/native/blob/a1c56a3e249ff82e711c18d0f8cf308d9eb4b62c/src/native/attributes.h),
pinned at `a1c56a3e249ff82e711c18d0f8cf308d9eb4b62c`. It does not build native's
modules or inherit their numerical/compiler policies.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/jam
```

The first configure fetches the pinned native source. For an existing checkout
or an offline build, set `FETCHCONTENT_SOURCE_DIR_JAM_NATIVE=/path/to/native`.
Installed packages carry the attribute header and module sources; compatible
consumer BMIs are regenerated by CMake after installation or relocation.

```cmake
cmake_minimum_required(VERSION 3.30)
project(example LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(jam CONFIG REQUIRED)
add_executable(example example.cc)
target_link_libraries(example PRIVATE jam::jam)
```

The consumer's compiler, standard library, exception mode and language-extension
mode must match its module build. `jam::jam` supplies C++26 and no exceptions;
set `CMAKE_CXX_EXTENSIONS=OFF` for consumers as shown.

`JAM_USE_BMI2=ON` enables bit extraction on x86-64 and requires BMI2 support
throughout the resulting application. There is no runtime ISA dispatch. The
default uses a portable bit-packing table. Tests default on for a top-level build. Enable
`JAM_BUILD_BENCHMARKS=ON` for `heap-bench`.

```sh
build/heap-bench --mode compressed --bytes 16MiB --workers 4 \
  --reserve-list 32,64,128 --repeats 11
```

The benchmark times compaction after marking, excluding allocation, tracing and
fixture setup. Its reserve sweep checks the complete relocated payload and
managed fields after each trial.

## Documentation

With Doxygen 1.18+ and Graphviz installed, enable the API reference in an
existing build:

```sh
cmake -S . -B build -DJAM_BUILD_DOCS=ON
cmake --build build --target jam-docs
```

Open `build/docs/html/index.html`. The reference includes this guide, the public
heap, root and visitor contracts, and links to their source. XML is written to
`build/docs/xml`. Documentation warnings fail the build; each run replaces the
generated HTML and XML so removed declarations leave no stale pages.

Jam is extracted from the independently tested dip heap. Dip's query runtime
and its ownership model are separate from this module. See [LICENSE.md](LICENSE.md)
for the dual BSD-2-Clause/Apache-2.0 license.
