# Lifted reference tracing

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

**Design accepted October 7, 2026; not implemented.** Jam owns the VM interface
and collector integration. [THC](https://github.com/ekmett/thc) owns its language
implementations. The interface below is the proposed contract, not an API in
released runtime packages.

## Purpose and ownership

A lifted reference can point to a computation or its result. Once a thunk has
an answer, we want incoming lifted references to skip the thunk. A selector can
also shed the rest of a constructor once its chosen field is available, without
evaluating that field. The language supplies these rules as methods; `jam::vm`
must make those methods safe to run during collection on HotSpot and
SubstrateVM.

Keep Jam physical forwarding as address lookup plus popcount. Do not add THC type knowledge to forwarding, marking, the frontier, or compaction. Do not substitute a terminal-thunk-only hook for this general protocol.

## Native model first

An ordinary `ptr<T>` names the object whose identity it preserves. A `lifted_ptr<T>` may name either a computation or its value. The incoming lifted pointer's tracer probes the target through a common vtable protocol, follows available replacements, rewrites its actual source slot, then requests ordinary tracing of the endpoint. The concrete target's tracing method supplies its allocation extent and outgoing fields. Use existing virtual `claim_and_trace`/`tracer<T>` machinery, not a second interpreted layout schema or redundant tag word.

Resolve an incoming lifted edge even when some other edge already marked its original target. Mark-once controls traversal of allocations; it does not control which incoming references get shortened. Never cast an unresolved computation to T.

Existing native `visitor.pointer`, `field`, and `target` separate slot declaration from following its contents. Hosted `visitor.fields` already receives indices identifying the actual mutable slots. The hosted shim can normalize before declaring/following those slots without changing Jam's collector algorithms. A native C++ implementation of `lifted_ptr` would additionally need a sanctioned collector-owned store capability: ordinary mutator assignment/barriers are not legal during collection.

## Java interface

Expose this beside `jam.vm.Weak`, in the bridge API supplied by Jam:

```java
package jam.vm;

public interface Lifted {
    Lifted resolve();
    Lifted resolveField(int field);
}
```

`resolve()` returns the next replacement, or null when no replacement is available. An ordinary terminal value and an unresolved thunk both return null. Null means stop, not WHNF, death, or a null guest value. A completed indirection returns its referent, which may itself be another thunk.

`resolveField(n)` returns the existing lifted reference at the language-defined projection n when available without evaluation; otherwise null. It must not box an unboxed field or force the selected field. THC defines the projection numbering and constructor-shape contract from its existing layouts. It forwards this request through already available indirections. Invalid compiler-generated projections remain compiler/runtime bugs; do not silently reinterpret an arbitrary field as lifted.

A selector is itself a lifted object. Its `resolve()` asks its target for `resolveField(n)`. On success it publishes the answer as an indirection and discards the old target/captures. On failure it keeps the unresolved selector. A projected answer may remain unevaluated.

Use plain `Lifted`. Generic `Lifted<T>` could add source-level constraints but erases to the same JVM type and does not help collector dispatch. In particular `Thunk<T>` erases to `Thunk`, so a field declared `Thunk<T>` cannot receive an unrelated constructor. A rewritable reference field uses the common `Lifted` type; concrete carrier references keep their ordinary identity and tracing policy.

The VM already enumerates physical reference slots and can inspect a referent's class/interface metadata. This identifies potential protocol implementations, but does not alone grant permission to replace every reference to that object. Eligible fields, array elements, and roots need a representation/type contract permitting every endpoint. Generated THC storage that currently uses `Object` needs an explicit semantic mapping or migration to `Lifted`; do not infer eligibility merely from `Object` or rewrite concrete `Thunk` slots. Preserve Java-owned handles/monitors and any references requiring wrapper identity.

## Resolution and tracing

For a nonnull eligible source slot, the semantic operation is:

```text
last = load(slot)
while (next = last.resolve()) is not null:
    last = next
store_gc(slot, last)
declare_real_slot_for_relocation(slot)
trace_normally(last)
```

This shows the successful acyclic path. The implementation must also terminate on self/cyclic indirections without evaluating them or manufacturing a value; there must be no arbitrary depth limit on valid chains. Preserve a valid opaque reference when a cycle prevents contraction. Never register a temporary local copy instead of the persistent source slot.

When `resolve()` returns null immediately, trace the original object as an ordinary Java object. After rewriting to another unresolved thunk, trace that thunk normally, including its live captures. Its outgoing lifted fields recursively use the same rule.

A selector's own ordinary object scan must also have a way to perform its contraction before following obsolete captures, when a direct reference retains the wrapper. Incoming-edge normalization alone must not leave the old selector environment strongly reachable indefinitely.

Collector stores must preserve relocation and generational bookkeeping, including old-to-young edges created by contraction, old source slots repaired after a minor collection, compressed references and applicable roots. Existing old-slot repair/card mechanisms remain authoritative. Normalize before enqueueing: a frontier job knows the target, not the original source slot.

## Laws and safety

- If resolution gives y, evaluating x and y has the same guest result/effects: resolution performs no guest evaluation and preserves sharing. Physical Java wrapper identity is deliberately preserved only by ordinary references.
- For stable published state, normalizing twice gives the same endpoint as normalizing once. A later thunk update can make further normalization possible in a later collection.
- `null` stops at the last nonnull object; it never overwrites a live slot with null.
- Selector contraction retains the chosen field and releases obsolete selectee captures. It never evaluates the field, loses its sharing, or treats an unboxed field as a reference.
- Unevaluated, owned/blackholed, suspended, failed and completed states retain their distinct meanings. Failure and ownership are not successful forwarding.
- Weak-key normalization never marks the key. The independently marked endpoint controls conditional reachability. Keep the weak least fixed point and freeze dead decisions before finalizer rescue; keep/rebind resolution policy across collections when endpoints can later change.
- Termination and relocation hold for long chains, cycles, young/old references, Java-held roots, and both VM providers. Source-slot exclusivity is not sufficient to synchronize concurrent updates to a shared selector: the owner must define publication and contraction rules before enabling parallel dispatch.

## Collector-safe method execution is required work

Ordinary `invokeinterface`/JavaCalls from a HotSpot collector callback is not a valid implementation. Jam must provide a verified/generated collector entry for language-defined resolution methods. For SubstrateVM, use an equivalent verified uninterruptible call graph. The entry must neither allocate, safepoint, block, initialize classes, throw, deoptimize, nor evaluate guest code. Compile/validate implementations and their reachable helpers before they can be invoked during collection. Include class-loader and code-lifetime ownership.

The intent is language-owned semantics, not a growing VM switch over THC thunk/constructor classes. If the required method dispatch cannot be supplied safely, report that design gap before replacing it with narrower descriptors. Existing completed-WHNF weak descriptors can ship independently but do not deliver this interface.

## THC integration and delivery

1. Jam owns the canonical design in its repository, the bridge API, safe entry compilation/validation, slot discovery/store semantics, and matching HotSpot/SubstrateVM implementations. Supply a versioned package/artifact containing the API and both providers, with focused evidence and build times. An interface JAR alone is not qualification.
2. THC implements the published interface on its lifted carriers and selectors, using existing `Thunk`, `DataLayout`/`DataValues`, capture and frame machinery. Inventory all lifted storage, arrays and roots; keep primitive/vector/aggregate storage and normal Java references unchanged. No per-object forwarding registry or replacement of normal CBD decoding.
3. Current THC state 2 means terminal WHNF; `Force` rejects a thunk result. General selector indirections to an unevaluated thunk require an explicit alias-state/forcing-contract change, preserving ownership, sharing, masking, suspension, tail handoff and both execution backends. Do not label that alias WHNF.
4. Qualify observable behavior with compact, intentional checks: ordinary value/unresolved thunk, a chain ending in an unresolved thunk, selector retention of only the chosen field, cycle termination, old-to-young relocation, and weak liveness through an independently live endpoint. Exercise both providers and relevant THC modes, with independent GHC results where applicable. Reuse existing producers; no giant matrix or additional acquisition harness.

Measure collector overhead and record build duration alongside qualification.
The terminal weak-key descriptor hook is separate work and does not qualify
this protocol.

## Next dispatch decision

The first implementation gate is a collector entry for the language-authored
methods on HotSpot. Identify how to compile and verify the restricted call graph,
enter it from a GC worker, and keep its code and class metadata alive without
requiring a Java thread transition. A normal compiled Java method is not such
an entry merely because its source contains no allocations. Its generated code
may still contain safepoint polls, resolution paths or deoptimization points.
SubstrateVM must enforce equivalent restrictions through its uninterruptible
call graph, including every possible virtual implementation.

No compilation route is selected yet. Establish feasibility before exposing the
interface as supported or asking consumers to migrate their storage. If this
cannot be provided on both VMs, report the limitation and revisit the design;
a finite set of thunk descriptors is not an implementation of these methods.

Once the API is stable, consumers pin a release's
[runtime manifest](distribution.md), the matching bridge JAR and provider
archives. Qualification must identify the source revision and exercise both
providers; a separately published interface JAR is insufficient. THC can then
implement the carriers and migrate eligible slots against that version.
