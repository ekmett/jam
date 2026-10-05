# jam

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A C++26 module for a double-mapped mark/compact heap. Typed references describe
managed edges; external roots register the objects that must survive collection.
Records have no collector header or runtime type tag.

`jam::ptr<T>` holds a four-byte object offset. Dereferencing it uses the heap
bound by the current thread's `jam::heap_scope`. Visiting a field carries its
target type into the marking queue, which invokes `jam::tracer<T>`. The default
tracer calls the target's `trace` member when present. Types without a hook are
leaves: scalars and ordinary reference-free records need no boilerplate.

```cpp
#include <cassert>
#include <cstdint>
import jam.unqualified;

struct node {
  ptr<node> next;
  std::uint64_t data;

  constexpr auto trace(visitor auto & visit) const noexcept {
    return visit(next);
  }
};

int main() {
  heap heap{{.capacity = 1_MiB, .reserve = 256_KiB, .workers = 4}};
  heap_scope scope{heap};

  static_cast<void>(make_ptr<node>());        // Unreachable.
  auto const a = make_ptr<node>(nullptr, 42u);
  auto const b = make_ptr<node>(a, 99u);
  a->next = b;                           // A cycle with inline data.
  root answer = a;                       // Implicit root registration.
  auto copy = answer;                    // An independent root hook.

  collect();                             // a and b are now stale; use the roots.

  assert(answer->next->next == answer.get());
  assert(answer->data == 42 && answer->next->data == 99);
  assert(copy.get() == answer.get());
  assert(heap.used() == 5);              // One null cell plus two two-cell nodes.
}
```

`import jam.unqualified;` makes jam's public names and byte literals available
unqualified. Use `import jam;` to keep names in `jam`.

`ptr<T>` can name an incomplete type, so recursive records need only their
member `trace` function. Typed heap operations check `jam::traceable<T>` once
`T` is complete: its tracer must be `noexcept`. The abbreviated parameter
`jam::visitor auto & visit` constrains the hook to jam's visitor. No superclass or separate tracer registration is needed.

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
tracing with the visitor. Scalar visits do nothing.

Visiting a `ptr<T>` tags its location in the enclosing allocation's pointer
mask and queues its nonnull target for allocation tracing as `T`. Compaction uses
those marks to rewrite pointers; other live bytes remain data. Every edge to a record
must agree on its complete type. Base-subobject and interior references require
the raw API.

`jam::make_ptr<T>(args...)` forwards constructor arguments or aggregate fields into
`T{args...}`, then copies the value into the heap and returns an unrooted
`ptr<T>`. No preconstructed `T` is needed. Braced initialization rejects narrowing
conversions; use `42u` for the node's unsigned data field. `nullptr` implicitly
constructs a null ptr. `heap.load(ptr)` returns a value snapshot;
`heap.store(ptr, value)` replaces
the record without changing its type or extent. Types must be unqualified,
trivially copyable, trivially copy constructible and standard-layout, with
supported alignment. Construction
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

`jam::heap_scope scope{heap}` binds an existing heap until scope exit, then
restores the previous binding. Scopes can nest, switch heaps and re-enter them.
They neither own the heap nor synchronize access. The heap must outlive its
scopes and roots. Implicit operations require a scope on the calling thread;
collector workers establish their own binding before invoking tracer hooks.

Compact pointers are plain field values: copying one does not register a root.
Initializing a `jam::root` from a ptr deduces its target type and registers it
with the current heap.
`heap.root(ptr)` provides the same operation with an explicit heap. `auto p =
jam::make_ptr<T>()` still deduces a ptr; use `jam::root` when retention is needed.
A root implicitly converts to its current ptr; `get()` returns the same value.
Both types provide `*` and `->`.
Their stored offsets are mutable so forwarding can update const handles.

Copying a root registers another hook; moving it transfers the hook without
allocating. Roots retain their original heap for registration and destruction,
even after a scope switches heaps. Dereferencing a root still requires its heap
to be current. Keep roots outside the moving heap: their registration links use
their addresses. The untyped root handle is named `heap::root_handle`.

**Outside the managed heap, only registered roots retain their meaning across
`heap.collect()`.** Do not keep a `ptr` on the stack or in an external container
for use after collection. This includes implicit root-to-ptr conversions, copies obtained from `root.get()` and
pointers inside snapshots returned by `heap.load()`. Keeping the target alive
through a root does not update those copies. After collection, obtain a fresh
pointer from the root and reload any snapshots before following their fields.

A default ptr is null, represented by zero. Cell zero is reserved across
allocation and compaction, so no live object can have the null offset. `used()`
includes this cell: an empty heap uses one cell. Null survives forwarding.
Allocation preserves offsets, but native `T*` and `T&` borrows obtained through
`*`, `->` or `heap.address(ptr)` expire on growth or collection. Typed borrowing
requires exclusive access to the record and cannot overlap collection. Borrow
again through a current pointer after movement. `data()` pointers and cell
references have the same mapping lifetime.

`ptr<T>` provides equality and three-way comparison by offset within one heap.
Compaction preserves the relative order of surviving objects, including alignment
padding; null sorts first. Compare fresh pointers obtained from roots after each
collection. Offset values themselves can change, so offset hashes are not stable.

**Mixing pointers from different heaps is undefined behavior.** There are no
owner tags in a ptr. Use the owning heap's scope when following a pointer or
converting it to a root. Scopes do not make concurrent heap mutation safe.

Collection is a compiler-fenced boundary: mutator stores precede tracing, and
subsequent accesses observe forwarded roots, fields and the current mapping. The
pool synchronizes worker completion before collection returns. These compiler
fences add no hardware fence instructions and do not permit concurrent mutators.

Mutation and changes to the root set stop during collection. Tracer hooks must
be `noexcept` and safe for concurrent invocation. Marking finishes before
compaction begins. `collect()` requires typed roots and edges; `collect(trace)`
also accepts untyped roots/edges, invoking its callback with a visitor and an
offset for those targets. That callback must claim each record and enumerate its
fields. Both forms redeclare pointer fields each collection, so a conditional
tracer can stop treating a field as a managed edge.

The heap uses 32-bit offsets scaled by eight, permitting a 32 GiB heap and two
managed fields in one cell. Each 256-byte rank block has 16 bytes of metadata;
alignment adds one byte per 512 bytes. Records may have 8-, 16-, 32- or 64-byte
alignment. Marking and compaction retain alignment groups as needed, and retained
neighboring cells count toward the used size.

Constructor options set initial capacity, reserve `N`, and one `workers` limit
shared by marking and compaction. Capacity and reserve are byte counts;
`_KiB`, `_MiB` and `_GiB` in `jam::literals` multiply by powers of 1024 at compile
time and return `std::size_t`. Defaults are 1 MiB capacity
and 256 KiB reserve. Construction rounds each size up to a native OS page and
requires the rounded capacity to hold at least twice the rounded reserve.
Any whole-page capacity is supported. `configuration()` reports the rounded
initial sizes; the low-level `capacity()` and `reserved()` accessors use cells.
`workers` must be positive and includes the calling thread. One persistent
pool serves both collection phases.
Compaction advances at most the reserved number of source pages past its earliest
unfinished page.
Ordinary collections step backward into the reserve through coherent virtual
aliases. Allocation doubles the backing when needed, capped at 32 GiB; sparse
collections can shrink it. `shrink_shift` selects the occupancy threshold:
the default `2` shrinks below 1/4 full, `3` below 1/8, and `0` disables automatic
shrinking. One collection can shrink once toward half capacity, rounded down to whole pages
and bounded below by twice the reserve, when its live cells and reserve fit.
Payload accesses use the contiguous double mapping. Only the collection view
rotation and mapping setup/resizing wrap explicitly with modulo.

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

The default carrier is `thread_local`. On AArch64, `JAM_CONTEXT_X28=ON` uses a
reserved `x28` register instead. CMake probes support and propagates `-ffixed-x28`
to consumers; all participating compilation must reserve it. The carrier holds
a stable heap pointer, and dereference loads that heap's current mapping base.
`jam::current_heap()` is a pure accessor; scope entry and exit use ordinary C++
stores so the compiler can observe rebinding.

The register carrier has no initial null guarantee: use implicit operations only
inside a scope. Establish a scope at foreign callback entry using an explicitly
saved heap identity, since foreign code may temporarily use `x28`. Worker threads
need their own scopes. Other users of `x28` require distinct scoped regions or a
shared carrier; they cannot hold separate active contexts in
the same register.

`JAM_USE_BMI2=ON` enables bit extraction on x86-64 and requires BMI2 support
throughout the resulting application. There is no runtime ISA dispatch. The
default uses a portable bit-packing table. Tests default on for a top-level build. Enable
`JAM_BUILD_BENCHMARKS=ON` for `heap-bench`.

```sh
build/heap-bench --bytes 16MiB --workers 4 \
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
