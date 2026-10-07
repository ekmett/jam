# Tracing

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A trace has two jobs: keep the complete allocation alive and identify the fields
that need forwarding. Embedded values contribute pointer fields to their
containing allocation; they are not separate allocations.

## Manifests

List the members that may contain managed pointers. Leave ordinary data out.

```cpp
struct tree {
  ptr<tree> left, right;
  std::uint64_t data;
  static constexpr auto manifest = make_manifest<tree>(&tree::left, &tree::right);
};
```

Jam claims `sizeof(tree)` with its alignment and declares both pointer slots,
even when null. It queues the left branch and walks the right in a loop. If only
one child is nonnull, it walks that child. More generally, the last nonnull
same-type child in structural order becomes the next loop iteration. Earlier
children are queued. Claiming each allocation once handles sharing and cycles.

Manifests compose through embedded manifests, arrays, tuples, variants and
pointer vectors. The descriptor is `consteval` and can be declared inside the
incomplete class. Member offsets come from actual subobject addresses and can
fold to constants; a variant's active alternative is selected at runtime.

The walk polls the scheduler between records. Graph depth does not grow the
machine stack; embedded traversal still follows the nesting of values.

## Hooks

For a dynamic layout, enumerate the active fields yourself:

```cpp
constexpr auto trace(visitor auto & visit) const noexcept {
  return visit(next);
}
```

Jam claims the allocation before calling a member hook. `visit(a, b, ...)` walks
its arguments in order, by reference. Arrays and tuples visit each element;
variants visit their active alternative, or nothing when valueless. Scalars do
nothing. Every element or alternative must support tracing. These adapters do
not imply that every standard-library container satisfies the
[heap storage contract](lifetimes.md).

A pointer visit declares its original slot and queues its nonnull target as `T`.
Pass fields from the supplied object, not copies: the address tells us which bit
to set. Tracing finishes before anything moves.

A `weak_ptr<T>` visit only declares the slot. It neither queues the target nor
continues the manifest's same-type walk. Include weak fields alongside strong
ones; the forwarder clears unclaimed targets after marking has finished.

For a type you cannot change, specialize `jam::tracer<T>`. Its ordinary hook is
`trace(Visitor &, T const &)`. Hooks take precedence over manifests. A type with
neither is treated as a leaf; Jam cannot discover unlisted pointers. Raw C++
pointers are data, not managed edges.

`jam::leaf<T>` is an advanced customization helper, not a base class for heap
objects. `template<> struct jam::tracer<T> : jam::leaf<T> {};` explicitly makes
value tracing do nothing, for example when a foreign type has an unrelated
`trace` member. Scalars and records without hooks or manifests already get that
behavior; they need no annotation. Suppressing traversal of actual managed
fields would let their targets die. An allocation-level `claim_and_trace` hook
still takes precedence.

`ptr<T>` may name an incomplete type. Typed heap operations check
`jam::traceable<T>` once it is complete. All edges to an allocation must agree on
its complete type unless an allocation-level hook supplies the dynamic type.
Arbitrary interior pointers are not supported by typed tracing.

## Polymorphic allocations

For a single, nonvirtual inheritance chain, put an allocation hook in the base:

```cpp
struct object {
  virtual void claim_and_trace(jam::heap::visitor &) const noexcept = 0;
};

struct node final : object {
  jam::ptr<object> next;
  std::uint64_t data;

  node(jam::ptr<object> p, std::uint64_t n) noexcept : next(p), data(n) {}

  void claim_and_trace(jam::heap::visitor & visit) const noexcept override {
    if (!visit.claim_target(this)) return;
    visit(next);
  }
};

jam::root<object> answer = jam::mk<node>(nullptr, 42u);
```

Jam dispatches `claim_and_trace` **before** claiming storage. The derived
method passes its own `this` to `claim_target`, supplying the complete size and
alignment. A successful claim establishes the record for pointer tagging;
`visit(next)` declares that slot and queues its target. A failed claim returns
immediately, handling shared targets and cycles. The hook must perform all
three jobs: claim the allocation, declare its pointer fields and trace their
targets. A virtual function needs the concrete visitor signature; it cannot be
a function template.

Every concrete dynamic type must supply an override that claims its complete
type. Make leaves `final` to avoid accidentally inheriting a smaller type's
claim. Do not call a base override that claims only the base subobject. Factor
shared field enumeration into an ordinary helper instead.

`ptr<Derived>` converts to `ptr<Base>` when the base provides this hook and the
inheritance is public and nonvirtual. The base must share the complete
allocation's address; a debug assertion checks the conversion. Jam supports
single, nonvirtual inheritance here, not multiple inheritance or adjusted base
pointers. `root<Base>` can register a derived pointer directly. The same hook
is used for roots, queued edges and remembered old-to-young slots.

The allocation hook takes precedence over cooperative `trace` and manifests.
It is never called for embedded values: those share the outer allocation and
need an ordinary value trace or manifest if they contain pointers. The hook
may also be nonvirtual. The byte-relocation and no-destructor contracts still
apply; a virtual destructor does not make Jam invoke it during collection.

## Cooperative walks

A static hook can take over allocation traversal. For a list:

```cpp
static constexpr void trace(visitor auto & visit, ptr<node> const & at) noexcept {
  for (auto const * p = visit.claim_target(at); p; p = visit.claim(p->next))
    visit.poll();
}
```

This entry is unclaimed. The visitor gives you three operations:

| Operation | Effect |
| --- | --- |
| `visit.pointer(p)` | Declare the source slot without following it. |
| `visit.claim_target(p)` | Claim the target and return its in-place `T const*`, or null if null/already claimed. |
| `visit.claim(p)` | Declare the slot, then claim the target. |

A successful claim makes that allocation current; a failed claim leaves the
current allocation unchanged. Declare outgoing slots before descending, and
trace every target you successfully claim. `visit(...)` remains available to
queue branches. Poll periodically so other workers can receive queued work.

`visit.pointers(0b101001)` declares slots 0, 3 and 5 in the current allocation,
counting 32-bit slots from its start. A second argument supplies a starting slot
for a chunk of up to 64 bits. This only ORs declarations into the pointer mask;
it does not follow targets. Zero bits leave existing declarations alone.

A tracer specialization may also provide `trace(Visitor &, ptr<T>)`. Allocation
tracing prefers this cooperative form; embedded values use the ordinary form.
Hook return types are unrestricted, though collection currently uses only their
effects. Hooks must not throw or retain borrowed addresses after tracing.

`ptr::prefetch()` hints the target's first cache line; `prefetch_marks()` hints
its mark metadata. Both are inlined and do nothing for null. They neither claim
an object nor declare a pointer, and provide no synchronization.

[Collector scheduling](collector.md) · [Topic guides](README.md)
