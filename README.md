# jam

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A C++26 module for a double-mapped mark/compact heap. Typed references describe
managed edges; external roots register the objects that must survive collection.
Records have no collector header or runtime type tag.

`jam::ptr<T>` holds a four-byte object offset. Dereferencing it uses the heap
bound by the current thread's `jam::heap_scope`. Visiting a field carries its
target type into traversal, which invokes `jam::tracer<T>`. The default
tracer supports manifests, ordinary member hooks and cooperative static hooks.
Types without a hook or manifest are leaves: scalars and ordinary reference-free
records need no boilerplate.

```cpp
#include <cassert>
#include <cstdint>
import jam.unqualified;

struct node {
  ptr<node> next;
  std::uint64_t data;

  static constexpr auto manifest = make_manifest<node>(&node::next);
};

int main() {
  heap heap{{.capacity = 8_MiB, .reserve = 1_MiB, .workers = 4}};
  heap_scope scope{heap};

  static_cast<void>(mk<node>());        // Unreachable.
  auto const a = mk<node>(nullptr, 42u);
  auto const b = mk<node>(a, 99u);
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

`import jam.unqualified;` brings `jam` and `jam::units` into scope, including
byte literals, `pages`, `bytes`, `space_cast` and `ceil`. Use `import jam;`
to keep names qualified.

`ptr<T>` can name an incomplete type. Typed heap operations check
`jam::traceable<T>` once `T` is complete. No superclass or separate tracer
registration is needed.

`make_manifest<T>(&T::left, &T::right, ...)` describes the members that may contain
managed pointers. Members can be pointers or embedded values, including nested
manifests, arrays, tuples and variants. Unlisted fields remain data. The
descriptor is built at compile time and can be declared inside the incomplete
class. Explicit tracing hooks and `tracer<T>` specializations take precedence.

Jam generates the allocation walk. It claims the complete `sizeof(T)` extent
with `alignof(T)`, batches pointer declarations into mask windows, and queues
outgoing targets. One nonnull same-type child from structural enumeration is kept
for the next loop iteration;
the other children are queued. A list therefore walks directly, and a binary
tree normally walks its right spine while donating the left branches. Cycles and
sharing stop at an earlier claim. The walk polls the existing stochastic
scheduler between records and does not grow the machine stack with graph depth.
Embedded objects share the enclosing allocation's claim and pointer mask.

Member offsets are computed from real subobject addresses and can fold to
constants in optimized code. The descriptor does not reinterpret member pointers
as integer offsets during constant evaluation. Dynamic hooks and active variant
alternatives are still evaluated at runtime.

The cooperative hook `static trace(visit, ptr<T>)` receives an unclaimed entry.
It claims the complete record before reading its fields, then walks between
records using raw pointers while marking is in progress. For manual control, the
list's manifest can be replaced with:

```cpp
static constexpr void trace(visitor auto & visit, ptr<node> at) noexcept {
  for (auto const * p = visit.claim_target(at); p; p = visit.claim(p->next))
    visit.poll();
}
```

The abbreviated parameter `jam::visitor auto & visit` constrains the hook to
jam's visitor. Inline `std::uint64_t` data needs no visit.

An ordinary member hook remains a field enumerator:

```cpp
constexpr auto trace(visitor auto & visit) const noexcept {
  return visit(next);
}
```

Jam claims the allocation before calling this form. Each visit declares a slot
and queues its target. For a type you cannot modify, specialize `jam::tracer<T>`
with `trace(Visitor &, T const &)` for ordinary enumeration or
`trace(Visitor &, ptr<T>)` for cooperative walking. If both forms exist, allocation
tracing chooses the cooperative form; embedded values use the ordinary form.
Return types are unrestricted; collection currently uses the hook's effects.
Deriving the specialization from `jam::leaf<T>` explicitly suppresses traversal.
A record containing managed pointers must enumerate them in a manifest or hook: the default leaf
convention does not inspect arbitrary fields. Raw C++ pointers are data, not
managed edges. Even a leaf allocation needs its complete extent marked live.

The visitor walks parts of that already-live allocation. `visit(a, b, ...)`
visits each argument in order, by reference; `visit()` does nothing. It does not
claim embedded values again. `std::tuple` and `std::array` visit each element;
`std::variant` visits only its active alternative (none when valueless). These
adapters compose recursively and require every element or alternative to support
tracing with the visitor. Scalar visits do nothing.

Visiting a `ptr<T>` tags its location in the enclosing allocation's pointer
mask and enqueues its nonnull target for tracing as `T`. Compaction uses
those marks to rewrite pointers; other live bytes remain data. Every edge to a record
must agree on its complete type. Base-subobject and interior references require
the raw API.

Cooperative walkers can separate pointer declarations from target claims:
`visit.pointer(p)` declares only the source slot, while `visit.claim_target(p)`
claims the complete target and returns its in-place `T const*` (null for a null
pointer or an earlier claim). `visit.claim(p)` does both, declaring the source
even when the target was already claimed. A successful target claim makes that
record current; failed claims leave the current record unchanged. Declare the
outgoing slots before descending into other records, and trace every target you
successfully claim yourself.

`visit.pointers(0b101001)` ORs a known pattern into the current record's pointer
metadata without following targets. Bit 0 denotes its first 32-bit slot; this
pattern declares slots 0, 3 and 5. An optional second argument gives the starting
slot for a chunk of up to 64 bits. Zero bits leave existing declarations alone.
These operations belong to the visitor, not to `ptr`.

`jam::mk<T>(args...)` forwards constructor arguments or aggregate fields into
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

Tracing reads each successfully claimed record in place. The claimant restores
its typed C++ lifetime after earlier VM remapping or cell-wise relocation;
marking finishes before any further relocation. Visit ptr members **by reference
from that supplied object**, including nested subobjects. A copied ptr no longer
identifies the field's location. Borrowed record addresses and the visitor must
not escape the callback. Tracing does not copy payloads onto the stack.

`jam::heap_scope scope{heap}` binds an existing heap until scope exit, then
restores the previous binding. Scopes can nest, switch heaps and re-enter them.
They neither own the heap nor synchronize access. The heap must outlive its
scopes and roots. Implicit operations require a scope on the calling thread;
collector workers establish their own binding before invoking tracer hooks.

Compact pointers are plain field values: copying one does not register a root.
Initializing a `jam::root` from a ptr deduces its target type and registers it
with the current heap.
`heap.root(ptr)` provides the same operation with an explicit heap. `auto p =
jam::mk<T>()` still deduces a ptr; use `jam::root` when retention is needed.
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
be safe for concurrent invocation and must not block waiting for other hooks.
Marking finishes before
compaction begins. `collect()` requires typed roots and edges; `collect(trace)`
also accepts untyped roots/edges, invoking its callback with a visitor and an
offset for those targets. That callback must claim each record and enumerate its
fields and remain `noexcept`. Both forms redeclare pointer fields each collection, so a conditional
tracer can stop treating a field as a managed edge.

Ordinary `visit(...)` calls enqueue edges without following them recursively.
Cooperative hooks can walk a spine and enqueue other branches. For a binary tree:

```cpp
struct tree {
  ptr<tree> left, right;
  std::uint64_t data;

  static constexpr void trace(visitor auto & visit, ptr<tree> at) noexcept {
    for (auto const * p = visit.claim_target(at); p; p = visit.claim(p->right)) {
      visit(p->left); // Declare and enqueue the left subtree.
      visit.poll();  // Offer queued branches to idle workers when due.
    }
  }
};
```

A tracer can instead keep a typed FIFO in front of its local walk. Here a
32-entry buffer prefetches object data on entry. `pushpop` only removes an entry when
full; otherwise it appends and returns null. The walker tries left first, then
right if nothing was displaced. Once left displaces work, right goes to the
worker queue. When neither displaces a node, the explicit drain handles the
partially filled buffer and the final tail. Early drains discover more children
and fill the buffer; later children refill any slots emptied by leaves or lost
claims. Draining is not a separate mode.

```cpp
#include <array>
#include <utility>

struct buffered_tree {
  ptr<buffered_tree> left, right;
  std::uint64_t data;

  static constexpr void trace(visitor auto & visit, ptr<buffered_tree> at) noexcept {
    std::array<ptr<buffered_tree>, 32> queue{};
    unsigned head = 0, size = 0;
    auto pushpop = [&](ptr<buffered_tree> value) noexcept -> ptr<buffered_tree> {
      if (!value) return {};
      value.prefetch();
      if (size < queue.size()) {
        queue[(head + size++) % queue.size()] = value;
        return {};
      }
      auto const result = std::exchange(queue[head], value);
      head = (head + 1) % queue.size();
      return result;
    };
    at.prefetch();
    for (;;) {
      if (!at) {
        if (!size) break;
        at = queue[head];
        head = (head + 1) % queue.size();
        --size;
      }
      auto const * p = visit.claim_target(std::exchange(at, {}));
      if (!p) continue;
      visit.pointer(p->left);
      at = pushpop(p->left);
      if (at) visit(p->right);
      else {
        visit.pointer(p->right);
        at = pushpop(p->right);
      }
      visit.poll();
    }
  }
};
```

`ptr<T>::prefetch()` is always inlined and hints the cache line containing the
object's first byte in the current heap. `prefetch_marks()` instead hints its
mark metadata. Both treat null as a no-op, even without a heap scope, and use
the pure current-heap accessor so the compiler can share its TLS lookup.
Prefetching neither claims
the target nor declares a pointer slot; it provides no synchronization. The
buffer remains local and typed, so only branches sent to `visit(...)` incur
queued callback dispatch. Buffer size is a tracer choice, not a heap setting.

There is no recursive call or queue entry for each right-spine link. `claim`
declares that link before attempting the next target, including links whose
target was already claimed. Stack use is independent of graph depth. Ordinary
embedded tuple, array and variant traversal still follows the nesting of values.
Tracing must not throw; an escaping user exception terminates collection.

Pending jobs own offsets and type-specific callbacks, not stack references.
Between queued jobs and at `visit.poll()`, the worker processes overdue donation
attempts and advances the previous deadline by exponential intervals (30
microseconds mean). Each successful attempt transfers the older half of the
queue to a randomly selected idle worker. Long cooperative hooks should poll
periodically so queued branches can be donated. A single chain offers no
independent branches to another worker. There are no scheduling exceptions,
stack captures or recursion-depth limits.

Only the owner accesses a local queue. A donor reserves an idle mailbox with CAS,
writes the batch, then publishes it with release ordering; the recipient acquires
it. Returning to idle releases the inbox for reuse. Active-worker accounting
includes reserved transfers, so marking cannot finish while a batch is in flight.
Local push/pop needs no synchronization; marking shared heap metadata still does.
This follows the sender-initiated algorithm in
[Acar, Charguéraud and Rainey (2013)](https://www.chargueraud.org/research/2013/ppopp/full.pdf)
and [PASL](https://github.com/deepsea-inria/pasl).

The heap uses 32-bit offsets scaled by eight, permitting a 32 GiB heap and two
managed fields in one cell. Each 256-byte rank block has 16 bytes of metadata;
alignment adds one byte per 512 bytes. Records may have 8-, 16-, 32- or 64-byte
alignment. Marking and compaction retain alignment groups as needed, and retained
neighboring cells count toward the used size.

Constructor options set initial capacity, reserve `N`, and one `workers` limit
shared by marking and compaction. Capacity and reserve are `jam::units::pages`.
The literals in `jam::literals` retain their units: `_B`, `_kB`, `_MB`, `_GB`,
`_KiB`, `_MiB` and `_GiB`. Defaults are 1 MiB capacity
and 256 KiB reserve. MiB units convert implicitly to pages; KiB units use
an explicit ceiling because a KiB is smaller than a page. Use
`units::ceil<units::pages>(size)` for arbitrary byte counts. The capacity must
hold at least twice the reserve.
Any whole-page capacity is supported. `configuration()` reports the initial
typed page counts; the low-level `capacity()` and `reserved()` accessors use cells.
`workers` must be positive and includes the calling thread. One persistent
pool serves both collection phases.

`jam::units::space<Rep, Ratio>` follows `std::chrono::duration`: an explicit
count constructor, implicit exact unit conversions, `.count()`, arithmetic in
a common unit and comparisons. `Ratio` measures a unit in bytes. Both integral
and floating representations are supported; there is no implicit conversion
to a plain integer. Integer arithmetic and conversions check overflow.

```cpp
using namespace jam;
using namespace jam::units;

bytes size = 1536_KiB;
auto truncated = space_cast<mebibytes>(size); // 1 MiB: truncate toward zero.
auto reserved = ceil<mebibytes>(size);        // 2 MiB: enough room.
auto total = 1_MiB + 512_KiB;                 // 1536 KiB.
auto fractional = space<double, mebi>{size};  // 1.5 MiB.
```

`floor` rounds downward; `round` chooses the nearest count, with ties to even.
`pages` has a compile-time ratio determined at configuration. Cross builds set
`JAM_PAGE_BYTES` explicitly; the heap checks that the running platform matches.

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

Jam requires macOS, Linux or Windows 10 version 1803+, CMake 4.4+, Ninja
and Clang 23+ with C++26 modules.
Configuration probes the language features used by the implementation.
Exceptions are enabled. Heap construction throws `std::length_error` above
32 GiB; resource failures and failures during collection still terminate. Jam links
[native](https://github.com/ekmett/native), pinned at
`4f5f6533417b2951b063581a3252c63de0024102`, for CPU/OS capability detection,
source-targeted SIMD and attributes.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/jam
```

On Windows, use LLVM 23.1.1 `clang-cl`, its matching `clang-scan-deps` and
LLD, and a Visual Studio developer shell with the MSVC C++ library and Windows
SDK. CI uses CMake 4.4.3 and Ninja 1.13.2 and tests both Debug and Release. Run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows uses pagefile sections and `VirtualAlloc2`/`MapViewOfFile3` placeholders.
Aliases, rotations and resizing retain the original backing without copying
payloads. Placeholder replacement supports the OS page size (4 KiB on x64),
including non-power-of-two capacities; 64 KiB allocation granularity does not
constrain heap capacities. Dead pages receive advisory `MEM_RESET`; a partially
retained section keeps its commit charge until its final view and handle are
released. Logical shrinking therefore need not immediately reduce system commit.

The first configure fetches the pinned native source. For an existing checkout
or an offline build, set `FETCHCONTENT_SOURCE_DIR_JAM_NATIVE=/path/to/native`.
Installed packages include native and the module sources; compatible
consumer BMIs are regenerated by CMake after installation or relocation.

```cmake
cmake_minimum_required(VERSION 4.4)
project(example LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(jam CONFIG REQUIRED)
add_executable(example example.cc)
target_link_libraries(example PRIVATE jam::jam)
```

The consumer's compiler, standard library, exception mode and language-extension
mode must match its module build. `jam::jam` supplies C++26;
set `CMAKE_CXX_EXTENSIONS=OFF` for consumers as shown.

The default carrier is `thread_local`. On AArch64, `JAM_CONTEXT_X28=ON` uses a
reserved `x28` register instead. CMake probes support and propagates `-ffixed-x28`
to consumers; all participating compilation must reserve it. The carrier holds
a stable heap pointer, and dereference loads that heap's current mapping base.
`jam::heap::current()` is a pure accessor; scope entry and exit use ordinary C++
stores so the compiler can observe rebinding.

The register carrier has no initial null guarantee: use implicit operations only
inside a scope. Establish a scope at foreign callback entry using an explicitly
saved heap identity, since foreign code may temporarily use `x28`. Worker threads
need their own scopes. Other users of `x28` require distinct scoped regions or a
shared carrier; they cannot hold separate active contexts in
the same register.

Each heap selects its compactor at construction from the capabilities admitted
by the CPU and OS: BMI2+AVX512, BMI2+AVX2, NEON, or a portable baseline.
Workers forward pointer fields and pack live cells with that implementation.
SIMD stores write exactly the live prefix; adjacent workers never overstore.
The x86 variants use BMI2 for final pointer-mask packing; the other variants
use a portable table. Optional instructions stay in the selected compactor,
without raising the instruction requirements of the rest of the application.
Tests compare every implementation admitted on the host against the baseline.
Tests default on for a top-level build. Enable
`JAM_BUILD_BENCHMARKS=ON` for `heap-bench`.

```sh
build/heap-bench --bytes 16MiB --workers 4 \
  --reserve-list 32,64,128 --repeats 11
```

The benchmark times compaction after marking, excluding allocation, tracing and
fixture setup. Its reserve sweep checks the complete relocated payload and
managed fields after each trial.

## Documentation

The [API reference](https://ekmett.github.io/jam/) is published from `main` after
CI builds the library, passes the five tests, and builds Doxygen with warnings
treated as errors. Pull requests run the same checks without publishing.

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
