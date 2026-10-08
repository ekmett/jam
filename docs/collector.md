# Inside the collector

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

Collection stops mutation, marks reachable allocations, computes forwarding
addresses, then moves the bytes and rewrites pointers. Objects carry no GC
header. Type information arrives through roots and traced edges; pending jobs
hold offsets and tracing callbacks, not references into a worker's stack.

[Weak associations](finalizers.md) add an ordered scan after ordinary marking.
The scan retains conditional values or queues finalizers, draining marking after
each decision. Registry entries and pending/running callbacks are forwarded before
metadata reuse. Callbacks run after the collector releases its mutation guard.

## Marks and forwarding

Allocation and liveness use eight-byte cells. Each 256-byte rank block has
16 bytes of metadata: live cells, a forwarding base and pointer slots. Alignment
adds one byte per 512 bytes. Records may require 8-, 16-, 32- or 64-byte alignment.
Each block's alignment nibble stores `k = 0..3` in thermometer form: `000`, `001`,
`011`, `111`, meaning alignment of `8 << k` bytes. Atomic OR then computes
`max(k)`, so concurrent markers can combine alignment requirements without a
compare-and-swap loop. The fourth bit joins the block to its successor when an
allocation spans the boundary.

Dilation retains the necessary alignment groups, including interior padding,
but drops every cell after the block's last exact live cell. A record crossing
the block boundary marks its final cell, so trimming leaves joined boundaries
intact. The next independent group still aligns its own start. Those retained cells
count toward used space, but the live mask keeps the exact claims throughout
forwarding. The prefix pass writes each joined group's effective alignment into
the existing alignment nibbles. Each consumer derives the dilated mask in
registers; no second liveness bitmap is needed. Vector forwarding can omit the
suffix trim: it counts bits below a live target, whose rank is unchanged.

Forwarding checks the target's exact claim and clears unclaimed targets. For a
survivor it combines the block's destination base with the count of earlier
retained cells, including alignment padding. Strong and weak fields share the
pointer mask: marking follows only strong edges, so forwarding needs no weak tag.
The prefix pass respects alignment and records spanning blocks before
workers forward arbitrary pointers. Pointer masks distinguish offsets from data,
including the two possible 32-bit pointer fields in each cell. Cells retained
only for alignment are copied as data; stale pointer declarations in them are
discarded before dilation.

Typed collection rebuilds pointer declarations each time it traces an allocation,
so dynamic layouts can change which fields are pointers. Minor collection leaves
old allocations untraced and uses the remembered source slots instead.

Forwarding finishes for both generations and all roots before metadata is reused.
Pointer masks then pack in place, consuming each source descriptor before clearing
or writing its destination. Young masks append to old during promotion or major
collection, preserving any shared boundary block. Reclaimed metadata is cleared;
collection needs no fresh destination table unless the arena itself grows.

Each heap selects a compactor from CPU and OS capabilities: BMI2+AVX512 (with a
VPOPCNTDQ variant), BMI2+AVX2, NEON, or baseline. Workers forward pointer fields and
pack live cells using that implementation. Stores write only the live prefix,
so adjacent workers do not overstore. The x86 variants use BMI2 to pack pointer
masks; other variants use a table. Optional instructions stay inside the selected
implementation.

## Work donation

Each marker owns its local queue. Between jobs and at `visit.poll()`, it processes
overdue donation attempts, advancing the deadline by exponential intervals with
a 30-microsecond mean. A successful attempt donates the older half of the queue
to a randomly selected idle worker. A chain has no independent branch to donate;
a tree usually does.

Marking runs as a typed gig in [work](https://github.com/ekmett/work). Each batch
binds the heap once and drains a private stack. A donor reserves an idle lane
and transfers its batch under the gig mutex; the pool dispatches that lane to
an executor. Termination accounting includes queued and running lanes, so
children remain accepted after external submission closes. Local queue
operations need no synchronization; handoffs, completion and claims on shared
heap metadata still do.

This follows the sender-initiated work in
[Acar, Charguéraud and Rainey](https://www.chargueraud.org/research/2013/ppopp/full.pdf)
and [PASL](https://github.com/deepsea-inria/pasl). The current walker uses neither
stack capture nor scheduling exceptions.

## Mappings

Each generation has two adjacent coherent views of its circular backing. Payload
access uses the contiguous view; rotation and mapping setup handle wrapping.
Compaction steps backward into the reserve. A bounded window prevents workers
from getting far enough ahead to overwrite unread source pages. Completed pages
release credit to advance the window.

Growth and shrinking remap surviving pages rather than copying their contents.
The reserved maximum keeps old below young while the active rings change size.

Windows uses pagefile sections and `VirtualAlloc2`/`MapViewOfFile3` placeholders.
Placeholder replacement supports OS pages (4 KiB on x64); the 64 KiB allocation
granularity does not force 64 KiB heap pages. Dead pages receive advisory
`MEM_RESET`. A partly retained section keeps its commit charge until its final
view and handle are released, so shrinking need not immediately reduce commit.

## Heap context and synchronization

The default carrier is `thread_local`. On AArch64, `JAM_CONTEXT_X28=ON` reserves
`x28`; CMake checks support and propagates `-ffixed-x28`. All participating code
must reserve it. The register has no initial null guarantee: use implicit
operations inside a scope, and establish a scope at foreign callback entry from
an explicitly saved heap identity. Independent users cannot keep different
active contexts in that same register.

`heap::current()` is pure; scope changes are ordinary stores visible to the
compiler. Dereferencing a pointer loads the selected heap's current generation
view. Scopes provide no thread synchronization.

Compiler fences bracket collection; worker completion supplies the inter-thread
synchronization. These fences add no hardware fence instructions and do not make
concurrent mutation safe. Stop mutators and root-set changes before collecting.
Trace hooks must tolerate concurrent invocation and must not wait for other hooks.

The raw `clear_marks`, `claim`, `mark`, `pointer` and `compact` operations expose
the same machinery for custom schedules. Join markers before compaction.
Raw `heap::compact` requires no weak associations or pending/running finalizers;
use typed collection to process those registrations.
`collect(trace)`, `collect_major(trace)` and `collect_minor(trace, promote)`
accept a `noexcept` callback for untyped roots and edges. The callback must claim
complete records and enumerate their fields. Omitting the callback requires typed
roots and edges throughout. `collect(trace)` follows the same countdown as
`collect()`.

[Tracing hooks](tracing.md) · [Building and benchmarks](building.md) · [Topic guides](README.md)
