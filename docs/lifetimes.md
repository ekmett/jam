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

Root deduction and ptr/root conversions are implicit. `heap.root(ptr)` registers
with an explicit heap. Copying a root registers another hook; moving it transfers
the hook without allocation. Roots remember their heap for registration and
destruction, even if the current scope changes. Dereferencing still requires the
owning heap to be current. Keep roots outside the moving heap: their links use
their addresses. The untyped handle is `heap::root_handle`.

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
storage do not get repaired. Resources that require destruction are unsuitable;
Jam has no finalizers. A manifest describes edges, not proof of relocatability.

Pointer copy/assignment barriers make `ptr<T>` nontrivial. Jam's storage contract
therefore goes beyond ISO C++ trivial-copy guarantees on its supported compiler
and VM platforms. The collector handles remembered-slot bookkeeping directly
when moving objects; it does not invoke mutator barriers.

Standard-library tuples and variants need not satisfy the storage requirements,
even when their elements can be traced. A `std::tie` view can enumerate existing
fields without storing a tuple in the heap.

[Generations and barriers](generations.md) · [Topic guides](README.md)
