# Hosting a runtime

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A runtime may already know how to find its roots, scan its objects and handle
weak references. `heap::host` lets it use Jam's marker and compactor without
adopting C++ `ptr<T>` fields or Jam's finalizer policy.

The representation stays the same: a 32-bit offset counts eight-byte cells,
with bit 31 selecting young space. Zero is null. Narrow heap fields can therefore
be forwarded directly. A host with wide references can enqueue their targets
with `visitor::target(offset)` and repair the wide fields itself.

## Setting up

Construct a `heap` with fixed capacities (`capacity == maximum`) and
`shrink_shift = 0` for both generations. Then borrow it:

```cpp
heap::host host{storage, guard_cells};
using generation = heap::host::generation;
auto chunk = host.allocate(generation::young, words);
```

`guard_cells` is a positive whole-page size measured in eight-byte cells. The
constructor reserves it in each fresh generation. It survives collection and
never promotes. The host leaves the corresponding canonical addresses
inaccessible. Allocation can hand out a whole thread-local buffer; the scanner's
claims, rather than allocation calls, determine individual object boundaries.

There must be exactly one host capability for the heap, and it must die first.
It cannot be copied or moved. Do not mix hosted collection with ordinary Jam
strong or weak roots, weak registrations, remembered slots or collection calls. The host owns
mutator synchronization, allocation serialization, barriers and GC scheduling.

## A collection

1. Stop mutators and call `host.begin(minor)`.
2. Call `host.trace(roots, scanner, workers)` as often as the reference policy
   needs. Each call drains before returning. The scanner receives a
   `heap::visitor &` and an offset; it claims the complete object before walking
   its fields. With `workers = 1`, callbacks run on the calling thread.
3. Query `host.marked(offset)` between drains. Resolve weak references and
   finalizer reachability here; Jam does not run host callbacks automatically.
4. Call `host.prepare(promote)`. A major compacts the generations independently.
   A minor either retains young survivors or promotes all of them into old.
   If promotion cannot fit, this returns false without moving objects or exposing
   forwarding. Marks remain available; retry with `prepare(false)`. The first
   prepare ends tracing, including when promotion fails.
5. Repair external roots and, for a minor, old fields using
   `host.forward(offset)`. Null and dead collected targets become null. Source
   objects are still at their original addresses.
6. Call `host.finish()`. Both moves consume the original forwarding tables before
   either table is packed. Republish any canonical aliases, then resume mutators.

During a minor, old objects count as live and ordinary old roots are not scanned.
For dirty old owners, use `trace(owners, scanner, 1, true)`. The host deduplicates
these jobs and scans their fields without trying to claim the old owner.
`visitor::field(cell, half)` declares a narrow source and queues its target;
`visitor::target(offset)` queues only a target. Both skip old targets during a
minor. `storage.pointer(cell, half)` declares a field without following it, useful
for weak references. The host must repair old fields before `finish()` because
old space does not move during a minor.

Every callback must be `noexcept`. Borrowed object addresses expire when the
movement phase starts. Finish an epoch before destroying its capability.

## Stable addresses

Jam rotates its private ring views during compaction. A runtime whose decoder
needs a fixed base can reserve its own address range and call:

```cpp
host.publish(generation::young, target, skip_cells, count_cells);
```

`target` corresponds to `skip_cells`, not cell zero. Skip, count and target must
be page aligned. The range must fit the fixed generation and must not overlap
Jam's private views. Publication aliases the backing pages; it does not copy
objects. Republish after movement while mutators are stopped. For the simple
`base + offset * 8` decoder, place the young window 16 GiB above the old window.

On Windows, the initial target must lie in a single placeholder reserved with
`VirtualAlloc2` and `MEM_RESERVE_PLACEHOLDER`. Jam splits that placeholder at the
window and backing-view boundaries, then maps its existing sections with
`MapViewOfFile3`. Page-aligned offsets suffice; placeholder replacement does not
require allocation-granularity alignment. A window may be republished only at
the same address and size, from the same generation. Published windows must be
disjoint. Do not alter their mappings or protection outside this API.

Remove an alias while mutators are stopped and no collection is active:

```cpp
host.unpublish(target, count_cells);
```

The address and count identify one complete published window. Unpublication
restores inaccessible reserved memory without releasing the address range. On
Windows it restores one placeholder covering exactly that window, which can be
published again or coalesced with adjacent placeholders by the host. Guards and
unrelated mappings outside the window are untouched. Changing a Windows
window's size or generation requires unpublishing it first.

The host owns and releases its reservation. On Windows, unpublish every window
before destroying the capability or heap; the reservation must remain valid
through those calls and may be released after heap destruction. On macOS and
Linux, aliases may instead remain until the reservation is released, but must
never be accessed after the heap dies. `storage.old()` and `storage.young()`
expose used cells, capacity and ring origins; `storage.compactor_name()` reports
the selected kernel.

## Pins

`host.pin_object(at, words)` registers a complete object and returns a stable
registration. `pin->address()` is a side alias of its backing pages; it remains
valid until the matching `host.unpin(pin)`. Repeated pins of one object share a
registration and must each be released. The managed offset can change:
`pin->position()` follows it. Include `host.visit_pins(callback)` in the strong
root walk before resolving weak references.

Several objects can share a pinned physical page. They retain their relative
positions while the collector remaps that page into its new canonical location.
Native code may concurrently write primitive payload through the side alias;
it must not write managed references without the runtime's barriers. Collection
repairs the traced reference slots but does not copy pinned payload.

Tracing first determines which objects are really live. Only after it drains
does pin placement fill occupancy bits for retained gaps. Forwarding ranks use
that expanded occupancy; weak-target survival uses the original claims. Bytes
retained around a pin are not resurrected objects, and their untraced fields
are never treated as pointers.

The extra claims array costs 32 bits per 256-byte mini-page during pinned
collection. Together with the 64-bit pointer mask, 32-bit occupancy mask,
32-bit forwarding base and alignment nibble, this is 20.5 bytes per mini-page.
Without moving pins, claims and occupancy share the existing mask and no extra
array is allocated. A minor with only old pins keeps the ordinary SIMD path.
After `finish()`, `host.gaps()` identifies destination padding that an object-stream
runtime must format as fillers before walking the heap.
Republish canonical windows after movement, including old space after pinned
promotion. The native side aliases stay fixed.

Collections that move pins currently stage movable survivors and splice backing
runs serially. A run can require several mapping calls, especially when earlier
collections have fragmented the backing map.

A gap can be as small as one eight-byte cell. The Native Image integration uses
its ordinary filler objects and therefore requires an eight-byte minimum instance
size, as in its default compressed-header layout. Image building rejects extra
object-header bytes that would make those fillers too large.
