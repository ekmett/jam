# Lifetimes and storage

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A `ptr<T>` is an offset, not ownership. A `root<T>` is an external handle that
collection updates. Keep pointers in managed objects and roots outside them.
Moving a pointer leaves the source null. Member and ADL `swap` exchange targets;
both operations maintain the write barriers for heap-resident slots.

## Roots and scopes

```cpp
heap h;
heap_scope scope{h};
root answer = mk<node>(nullptr, 42u);
auto copy = answer;
collect();
assert(copy.get() == answer.get());
```

Root deduction and ptr/root conversions are implicit. Both `root<T>` and
`weak_root<T>` are four-byte indices into the current heap's registration table.
They store neither a heap pointer nor a registration address. Copies share a
registration; moving transfers the index and empties the source. The last release
recycles the slot. Growing or moving the table does not change existing indices.
The untyped strong handle is `heap::root_handle`.

`heap.root(ptr)` and `heap.weak_root(ptr)` register with that heap, which must be
current. Every operation on a registered handle, including copying, locking,
resetting and destruction, requires the owning `heap_scope`. A handle may sit
unused while another heap is bound, then be used after re-entering its heap.
Using it under the wrong heap is undefined behavior; there is no stored owner ID.
Keep these handles outside the managed heap. Use `ptr` and `weak_ptr` for fields.

Creating, copying and releasing roots modifies registration state during ordinary
execution. GC updates the registrations' target offsets. Both kinds of access
must be serialized: stopping mutators during GC alone does not make concurrent
root creation or destruction safe. Registration reference counts are non-atomic.

A `heap_scope` binds an existing heap to the current thread and restores the
previous binding on exit. Scopes can nest, switch heaps and re-enter them. They
do not own the heap or synchronize access. The heap must outlive its scopes and
roots. Collector workers bind it before invoking hooks.

Mixing pointers from different heaps is undefined behavior. A pointer has no
owner ID. Use the owning scope when dereferencing or converting a ptr to a root.

## What survives collection

Only registered roots retain their meaning outside the heap across collection.
A root keeps its target alive, but does not update other copies of its pointer.
Reacquire pointers and reload snapshots from roots after collecting. This applies
to stack variables, external containers, SIMD values, `root.get()`, implicit
root-to-ptr conversions and pointers in `heap.load()` results.

Allocation preserves encoded offsets. Raw `T*` and `T&` borrows from `*`, `->` or
`heap.address(ptr)` expire on growth or collection, as do `data()` borrows.
Borrow again after movement. Mutation and borrowing require exclusive access to
the affected record and cannot overlap collection.

Null is zero; allocation reserves cell zero. Pointer equality and ordering compare
offsets within one heap. Compaction preserves surviving order, including when
young follows old into the old arena. Offset hashes are not stable across GC.

## Weak fields

Use `weak_ptr<T>` for an edge that should not keep its target alive. It takes
the same four bytes as `ptr<T>` and belongs in the manifest too:

```cpp
struct node {
  ptr<node> next;
  weak_ptr<node> parent;
  static constexpr auto manifest = make_manifest<node>(&node::next, &node::parent);
};

// Inside a heap scope, with answer a root<node>:
if (auto parent = answer->parent.lock()) {
  // parent is a strong root, valid across collection.
}
```

Construct or assign a weak field from a pointer or root. Tracing declares its
slot but does not follow it. Collection forwards the field if the target survives
and clears it otherwise. `expired()` tests for null; it does not run GC. A minor
collection cannot decide whether an old target is dead, so that waits for a major.

There is no control block. An external weak copy is just an offset and expires
at collection, exactly like an external `ptr`. Read the field again through its
owner's root, or call `lock()` before collecting. `lock()` requires the owning
heap current and returns an empty root for a null field. These operations do not
synchronize with concurrent collection.

Weak fields compose through manifests, arrays, tuples and variants. Copy, move,
ADL `swap`, `unsafe_assign` and array `assign` maintain the same slot discipline
as strong pointers; remembered weak slots never become marking roots.

## Weak roots

Use `weak_root<T>` to observe an object from outside the heap without keeping it
alive. Unlike a stack copy of `weak_ptr<T>`, its slot is updated across collection:

```cpp
heap h;
heap_scope scope{h};
root answer = mk<unsigned>(42u);
weak_root watch = answer; // Also accepts ptr<T>; CTAD supplies T.
collect_major();
assert(*watch.lock() == 42);

auto locked = watch.lock();
answer = {};
collect_major();
assert(*locked == 42); // A successful lock is a strong root.
locked = {};
collect_major();
assert(watch.expired() && !watch.lock());
```

`lock()` resolves through `heap::current()` and returns a strong `root<T>`, or an
empty root if the target has gone. `expired()` checks the slot; neither operation
collects or synchronizes with GC. A minor leaves old targets alone. Dead young
targets clear during a minor; old targets wait for a major. Clearing follows exact
liveness, not alignment padding retained by compaction.

Copies share a weak registration, without making it strong. Move leaves the
source empty. `reset()` releases the handle, and member/ADL `swap` exchanges two
handles from the same heap. Even an expired registered handle must be reset or
destroyed under its heap scope before the heap dies. Default and moved-from
handles are detached and need no heap.

A plain weak root has no finalizer. If another mechanism retains its target—for
example, a queued finalizer retaining its key—it remains lockable. The
[generalized `weak<V>` association](finalizers.md) has different semantics: it
expires when its registration retires, even if its key is resurrected.

Hosted runtimes own their weak-reference policy. Java uses `WeakReference<T>`;
JNI uses weak global references and acquires a strong local/global before use.
Those use the [hosted collection phases](hosting.md), not C++ weak roots.

## Byte relocation

`mk<T>(args...)` constructs `T{args...}` before allocation can invalidate borrowed
arguments, then transports the bytes into young. Aggregate field arguments work,
and braced initialization rejects narrowing. `heap.load(ptr)` copies a snapshot;
`heap.store(ptr, value)` assigns without changing the allocation's type or extent.

Managed records must be unqualified and byte-relocatable, with
alignment no greater than 64 bytes. Construction must be `noexcept`. Byte
relocation means copying the representation, forwarding managed pointers and
discarding the old storage preserves the object's meaning. Compaction knows
cells and masks, not C++ types. It invokes no move constructors or destructors
and adds no per-object dispatch pointer. Existing vptrs travel unchanged with
the representation. [Polymorphic allocation hooks](tracing.md#polymorphic-allocations)
allow single, nonvirtual inheritance with a base at the allocation start.

Self-links and cycles through `ptr<T>` work. Raw pointers into an object's own
storage do not get repaired. Required C++ destruction is unsupported.
[Managed finalizer actions](finalizers.md) can perform explicit cleanup after GC;
they do not change the byte-relocation contract. A manifest describes edges,
not proof of relocatability.

Pointer copy/assignment barriers make `ptr<T>` nontrivial. Jam's storage contract
therefore goes beyond ISO C++ trivial-copy guarantees on its supported compiler
and VM platforms. The collector handles remembered-slot bookkeeping directly
when moving objects; it does not invoke mutator barriers.

Standard-library tuples and variants need not satisfy the storage requirements,
even when their elements can be traced. A `std::tie` view can enumerate existing
fields without storing a tuple in the heap.

[Generations and barriers](generations.md) · [Topic guides](README.md)
