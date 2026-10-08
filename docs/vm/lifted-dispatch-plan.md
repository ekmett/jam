# Lifted collector entry plan

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

This is the bounded implementation plan for [lifted tracing](lifted-tracing.md),
not a supported API. Target Jam-patched **GraalVM HotSpot and SubstrateVM**.
A HotSpot package without Graal is outside this experiment. Keep the terminal
weak-key implementation and its qualification independent.

The first deliverable is a real collector call to compiled language-authored
methods. Stop at a failed feasibility gate and report the failing mechanism.
Do not replace methods with a table of recognized thunk shapes.

## Entry and lifetime

Use private native entries with this logical ABI; the backend must implement
the platform's native calling convention, not Java's compiled calling convention:

```cpp
uint32_t resolve(JamResolveContext * context, uint32_t receiver);
uint32_t project(JamResolveContext * context, uint32_t receiver, int32_t field);
```

Arguments and results use the adapter's compressed-reference encoding. Zero is
Java null. The receiver is nonnull and remains at its pre-compaction address
throughout the call. Context supplies decoding information, a frozen exact-class
entry table and access to approved collector operations. It contains no JavaThread
substitute. Do not enter Java, allocate managed objects or install code from a
collector callback.

Compile, validate and install both methods for an exact class before enabling
its entry. Publish the pair together; an incomplete or rejected registration
cannot become callable. For the prototype, pin the registered classes, relevant
metadata and installed code until VM shutdown. This deliberately retains class
loaders, but must not retain receiver instances. Dynamic redefinition of an
installed implementation is unsupported and must be rejected, not silently
leave stale code. Class unloading/redefinition support is a later lifetime task.

Start with one final carrier class and one collector caller. Keep registration
private to the experiment until installation and failure behavior are proven.
The public Java methods remain `Lifted.resolve()` and `Lifted.resolveField(int)`;
there is no consumer registration API to adopt yet.

## Verification boundary

Validate the complete reachable graph, including helpers and every possible
virtual implementation. Check both before and after lowering so intrinsics
cannot introduce an unchecked escape. Use an allowlist of operations; absence
of an obvious allocation in source is not evidence of safety.

Allow primitive arithmetic, guarded reference/primitive reads, branches and
statically bounded local computation. Reject allocation, monitors, throwing or
unwinding, class initialization, safepoints, deoptimization, reflection, JNI,
unchecked native calls and speculative class-hierarchy assumptions. Null and
bounds checks must be discharged or take an explicit non-throwing path.

Initially inline acyclic helper calls. Reject loops without a verified bound
and recursive calls. This is a limit of the first prototype, not a substitute
for general selector support. Lifted virtual calls must eventually dispatch
through the frozen collector-entry table, never through ordinary Java entries.
If required recursive projection methods cannot be lowered safely, report that
remaining gap before claiming the interface is complete.

SubstrateVM uses a closed AOT call graph and its normal uninterruptible checks
plus this stricter verifier. Disallow annotation escape hatches and unaudited
`NO_TRANSITION` calls; keep annotation checking enabled. Runtime-compiled method
variants are not collector entries.

## Storage and cycles

Begin with a specifically registered **nonfinal volatile `Lifted` field** in a
holder. The scanner rewrites its persistent slot before declaring/following it.
Other fields, arrays and roots keep ordinary Java behavior during this experiment.
In particular, do not rewrite arbitrary compiled roots from their source-level
type: reference maps do not describe all JIT type and identity assumptions.

Volatile storage is a conservative experimental restriction, not yet a proof
that rewriting is compatible with all compiler transformations. Check interpreted,
C1 and Graal execution before and after repeated collections, retaining an ordinary
reference to the original carrier while another path reads the rewritten field.
Do not broaden eligibility to plain/final fields, arrays or roots until their
compiler and representation contracts have been established.

Keep resolution-chain traversal and cycle detection in the collector. Detect
self and multi-object cycles without imposing a depth cutoff on valid chains;
retain a valid opaque reference on a cycle. During this first read-only-resolver
stage, constant-space cycle detection can re-probe stable state. Reconsider that
algorithm before permitting mutating resolvers: repeated probes are not generally
free of side effects.

Only the collector performs incoming-slot stores initially. A later selector
stage lowers permitted writes to the receiver's registered mutable fields into
audited collector stores, preserving reference encoding and remembered edges.
Run that stage serially until shared-selector publication has a proven parallel
contract. Ordinary mutator write barriers are not the collector-store API.

## Implementation gates

### 1. Call one method from HotSpot's actual scanner

- [ ] Add the experimental interface in
  `vm/src/bridge/java/jam/vm/Lifted.java` and one fixture in
  `vm/t/bridge/LiftedBridgeSmoke.java`. A final carrier returns its stored
  reference only when a volatile state is complete; its projection method
  returns null. The fixture starts with no collector implementation and must
  demonstrate that the holder has not contracted.
- [ ] Add `JamLiftedCompiler.java` under
  `vm/src/graal/compiler/src/jdk.graal.compiler/src/jdk/graal/compiler/hotspot/jam/`.
  Parse the actual resolver method, validate its graph and compile a separate
  native entry using Graal's runtime-stub machinery. Keep changes to existing
  upstream compiler files in `vm/patches/graal-jam.patch`.
- [ ] Add private installation and scanner plumbing in
  `vm/patches/hotspot-jam.patch`. Decode and encode through the adapter's current
  reference layout. Audit generated code for native register preservation,
  reference decoding, stack use and absence of Java-thread accesses/runtime
  escapes; do not treat stub installation alone as success.
- [ ] Run the fixture with Jam from the patched GraalVM: unresolved carrier is
  retained; completed carrier's holder points to its answer; after collection
  the answer is intact and an otherwise unreachable carrier can die. Enable
  heap verification. Save generated assembly and build/run commands.

This proves only the entry mechanism. It is not delivery of lifted tracing.

### 2. Prove dispatch and rejection

- [ ] Extend that fixture with a constructor whose language-authored
  `resolveField(0)` returns an existing lifted child and a selector whose
  `resolve()` calls it. Compile the call through the collector table; no
  class-name switch or hand-translated method body. The selected child remains
  unevaluated, and a chain may end at another unresolved thunk.
- [ ] Add rejection cases to `vm/t/java/LiftedValidationSmoke.java`: allocation,
  a monitor, explicit/cached throw, unchecked foreign call, deferred class
  initialization, recursive helper and an unsafe overriding implementation.
  Each must fail installation before a GC callback can invoke it.
- [ ] Test null stop, a long valid chain, a self-cycle and a two-object cycle.
  Verify ordinary carrier references retain their identity and that rejected
  registrations leave no callable partial entry.

### 3. Run the same contract on SubstrateVM

- [ ] Add `JamLiftedFeature.java` and `JamLiftedSupport.java` alongside `JamGC.java`
  in the existing `com.oracle.svm.core.jam` source overlay. Reuse Graal graph
  validation where its graph stage is shared; retain provider-specific lowering
  checks. Connect the verified AOT entries to the existing scanner/isolate entry.
- [ ] Build the same positive fixture with `native-image --gc=jam`. Compile each
  negative fixture separately and require image-build rejection. Record the
  difference from HotSpot's registration-time rejection. Annotation presence
  alone never satisfies a negative test.

### 4. Add mutation and generational correctness

- [ ] Extend the two scanner adapters and fixtures for receiver-local selector
  memoization/capture clearing through approved collector stores. A direct
  ordinary reference retaining the selector must not retain discarded captures.
- [ ] Exercise young/old sources and endpoints, retaining minors, promotion and
  majors; verify source-slot repair and remembered edges after contraction.
  Check shared selectors serially before adding concurrent dispatch.
- [ ] Integrate generalized weak-key resolution separately from strong tracing:
  probes do not mark their keys, independently live endpoints activate values,
  and unresolved endpoints keep their future resolution policy.

### 5. Stabilize and deliver

- [ ] Resolve recursive projection support and remaining slot/JIT eligibility
  gaps before calling the general protocol complete. Publish supported method
  operations, installation failures, storage rules and lifetime costs; only then
  ask THC to implement carriers and migrate eligible storage.
- [ ] Run THC's real forcing/selector checks against a matched artifact. Measure
  a no-lifted workload and a contraction workload. Reuse existing VM checks and
  release tooling; publish a pinned manifest with matching bridge and provider
  archives, source revision and evidence. Record build time as well as runtime
  overhead. A successful interface-JAR build is not qualification.
