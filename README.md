# jam

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

jam is a compacting generational garbage collector for C++26. Build lists, trees
or graphs, keep roots, and let the collector reclaim what you can no longer
reach. Sharing and cycles are fine. A pointer takes four bytes, and objects need
no GC header or intrusive base class.

Tell us where your pointers live and we generate the walk:

```cpp
#include <cassert>
#include <cstdint>
#include <new>
import jam.unqualified;

struct node {
  ptr<node> next;
  std::uint64_t data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};

int main() {
  heap heap{{.workers = 4}};
  heap_scope scope{heap};

  auto const a = mk<node>(nullptr, 42u);
  auto const b = mk<node>(a, 99u);
  a->next = b;
  root answer = a;

  collect();

  assert(answer->data == 42);
  assert(answer->next->next == answer.get());
}
```

Here `next` is a managed edge; `data` is just data. The two nodes form a cycle.
`answer` keeps it alive, and collection repairs the links as objects move.
Drop the root and the whole cycle becomes collectible.

Manifests compose through embedded records and arrays. You can store SIMD
pointer vectors too, or supply a trace hook when the layout depends on a tag.
The [tracing guide](docs/tracing.md) and [pointer vector examples](docs/simd.md)
pick up from there.

There are a few things you have to get right:

- List every managed edge in a manifest or trace hook. Unlisted fields are data;
  Jam cannot discover a pointer you haven't told it about.
- Keep roots outside the heap and pointers inside it. The local `a` and `b` above
  are stale after `collect()`; get fresh pointers from `answer`. Raw pointers and
  references into the heap can also expire when allocation grows it.
- Use the owning `heap_scope` when working with a pointer. Mixing heaps is
  undefined behavior. Collection can use several workers, but you must stop
  mutating the heap before collecting.
- Objects move as bytes. Jam does not call their move constructors or run their
  destructors when reclaiming them. Self-links through `ptr` work; raw
  self-pointers and resources that need cleanup don't. See the
  [storage contract](docs/lifetimes.md) before choosing what to put in the heap.

You choose when to call `collect()`. Most calls collect only young objects;
periodically one collects both generations. Writes from old objects to young
ones are tracked automatically. [Collection policy and sizing](docs/generations.md)
cover the controls when you need them.

To try it, you'll need Clang 23+, CMake 4.4+ and Ninja on macOS, Linux or Windows.
The [build guide](docs/building.md) has the commands. `import jam.unqualified;`
puts the names in scope as above; use `import jam;` for qualified names.

The [topic guides](docs/README.md) go deeper; the
[API reference](https://ekmett.github.io/jam/) documents the individual operations.

Dual [BSD-2-Clause / Apache-2.0](LICENSE.md).
