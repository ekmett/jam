# Heap architecture

Jam represents references as offsets into two generations. HotSpot represents
compressed oops as offsets from a fixed heap base. With a suitable choice of
address space, these can be the same bits.

That lets us keep jam's allocator, metadata, tracing frontier, rank forwarding,
SIMD compactor and work scheduler. The adapter supplies the representation of a
Java object and the rules for finding its references.

## Cells and objects

A jam cell is eight bytes. Mark metadata covers 32 cells at a time, and a second
mask describes the two possible four-byte pointer slots in each cell. Marking
claims an object's complete extent. Forwarding computes where its live cells
will land by counting live cells before them.

For Java, the VM obtains the extent from `oop::size()` and the pointer locations
from `oop_iterate`. Mark words, Klass pointers and primitive payload remain
opaque data. In particular, a compressed class pointer is not an oop merely
because it occupies four bytes.

There are two separate operations on a field:

* Declare its slot, so the compactor can update it.
* Follow its target, so the target stays alive.

A Java weak referent needs the first operation without automatically getting
the second. The [weak-pointer policy](weak-pointers.md) decides when following
such a target is appropriate.

The VM batches narrow slots in groups of 128 before calling the backend.
There is no foreign call for each pointer in a Java object. Wide fields, such
as references in stack chunks, are traced through jam but repaired by the VM
before copying. Their cells remain ordinary payload to the SIMD kernel.

## A stable view of a rotating heap

Each jam generation uses a double-mapped circular buffer. The second mapping
makes a logical arc contiguous even when it crosses the physical end of the
buffer. After compaction, the generation adopts a new origin in that ring.

Compiled Java code cannot follow a changing compressed-oop base. Instead, the
VM reserves a fixed canonical address range. The backend maps the current
logical arc of each ring into that range and republishes it after collection.
The ring's private aliases retain jam's overlap and copy-credit invariants.
The canonical alias gives Java a stable decoding rule.

Let `B` be the compressed-oop base and `i` a nonnull cell offset. With shift
three, HotSpot decodes it as:

```text
decode(i) = B + 8*i
```

Jam uses bit 31 to select young. Consequently:

```text
D = 8 * 2^31 = 16 GiB

decode(old(i))   = B     + 8*i
decode(young(i)) = B + D + 8*i
```

The gap is reserved virtual address space. It is not a 16 GiB allocation of
physical memory. Only the usable generation windows are published; their
private copy reserves and protected prefixes are not Java heap capacity.

| Quantity | Meaning |
| --- | --- |
| `B` | Fixed, nonzero compressed-oop base |
| `P` | Protected prefix before each usable generation |
| `O`, `Y` | Usable old and young capacity |
| `R` | Each generation's private copy reserve |
| `B + P` | First usable old address |
| `B + D + P` | First usable young address |
| `O + Y` | Reported Java heap capacity |

Each private ring needs room for its prefix, usable capacity and copy reserve
within the 16 GiB generation domain. The prefix is retained during compaction
but never scanned as Java objects. Promotion excludes the young prefix and
recreates it when the nursery resets. Cell zero represents null.

For a valid nonnull published address `p`, the encoding must satisfy:

```text
decode(encode(p)) = p
encode(decode(i)) = i
forward(0)       = 0
```

The first two laws hold only for usable published addresses and valid offsets;
the protected prefix and unmapped gap do not contain objects. A surviving
reference becomes `forward(i)` before mutators resume. Publishing the next
alias changes which backing pages those bits address, without a second payload
copy.

There is a useful consequence of choosing identical reference bits: jam's
existing SIMD forwarding code needs no conversion. There is also a trap.
A *slot index* counts four-byte positions, so doubling a young cell index
needs 33 bits. The C interface uses 64-bit slot indices even though the
references stored in those slots are still 32 bits.

## Minor collection

New objects and TLABs normally allocate in young. A retaining minor marks and
compacts young, leaving old addresses and old ring state unchanged:

```text
minor_forward(old(i)) = old(i)
```

Ordinary old roots are treated as live without traversing their objects.
Instead, HotSpot supplies old owners found through its card table. The adapter
scans each distinct dirty owner through jam's frontier and records the slots
that need repair.

Scanning the entire owner matters. A compiled instance-field barrier may dirty
the card containing the object's header rather than the card containing the
field. `SerialBlockOffsetTable` recovers the owner crossing a card boundary.
Arrays currently get the same whole-owner treatment.

A retaining minor keeps dirty cards. An old-to-young edge can survive several
collections without another mutator store; clearing its card after the first
scan would lose it at the second collection. The VM repairs remembered old
slots before jam moves young objects.

## Promotion

Every `JamPromoteEvery` minors, the collector attempts to promote the complete
live nursery into old. Promotion is checked after reference policy has closed,
when the final live size is known.

If the survivors do not fit, preparation fails before forwarding roots or weak
registrations. The same epoch then prepares a retaining minor. Changing that
failure into a major at this point would change the liveness question after
weak decisions had already been frozen.

A successful promotion resets young, rebuilds old object boundaries and clears
the old-to-young cards: there are no surviving young objects left to remember.
Selective promotion is not implemented.

## Major collection

A major marks and compacts both generations independently. It does not require
the combined live heap to fit in old space. Both forwarding tables remain
available while roots, fields and weak metadata are repaired, and both moves
finish before either generation's metadata is packed.

After publication, HotSpot rebuilds old object boundaries and the cards for
surviving old-to-young edges. For example, a 64 MiB heap with 32 MiB old can
retain over 40 MiB of live objects across a major, even though it cannot
promote them all into old.

## What belongs on each side

| Concern | jam | VM adapter |
| --- | --- | --- |
| Storage | Physical backing and private rings | Stable canonical reservation |
| Allocation | Cell allocation | Object initialization, TLABs and failure policy |
| Liveness | Full-extent claims and mark metadata | Object sizes, roots and reference policy |
| Tracing | Frontier and persistent worker pool | Object scanners and dirty-owner selection |
| Relocation | Rank forwarding, SIMD movement and mask packing | Root repair, wide fields and code relocations |
| Scheduling | Copy waves and bounded reserve | Safepoints, minor/major selection and promotion attempts |

The adapter borrows a `jam::heap::host` to control the collection phases.
Jam's heap, compactor and worker pool are used without a local patch. VM
scanning currently runs on the registered VM thread; native copy work uses
jam's worker pool. Parallel VM scanning needs a proper HotSpot
worker-registration contract first.

Each VM thread owns a descriptor for a `jam::heap_scope`. HotSpot enters that
scope on the executing OS thread and leaves it on detach. Native Image also
suspends the scope across native calls, allowing an OS thread to switch
isolates. Jam's own tracing workers retain their existing heap scopes.

The current encoding requires fixed capacities, ordinary object headers,
eight-byte alignment, normal pages and nonzero-base shift-three compressed
oops. Other layouts are separate implementation work, not alternate settings
of this adapter. See [HotSpot integration](hotspot-integration.md) for validation
and [Native Image](native-image.md) for the SubstrateVM layout and pinning rules.

## Source

The implementation builds on jam's pinned
[heap](https://github.com/ekmett/jam/blob/2e65a1bfc68cce66dc4a31befba95f8bf361a241/heap.ccm),
[compactor](https://github.com/ekmett/jam/blob/2e65a1bfc68cce66dc4a31befba95f8bf361a241/compact.ccm)
and [work scheduler](https://github.com/ekmett/jam/blob/2e65a1bfc68cce66dc4a31befba95f8bf361a241/work.ccm).
The [source manifest](../config/source-pins.json) records the exact inputs.
