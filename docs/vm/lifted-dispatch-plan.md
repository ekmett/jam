# Lifted weak handoff plan

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

This replaces the previous collector-entry implementation plan. Implement the
[language-owned weak handoff](lifted-tracing.md) using ordinary `jam.vm.Weak`
registrations. No core Jam change, resolver registration, common Lifted interface,
Graal collector-entry compiler or SVM scanner extension is part of this work.

1. **Define the language lifecycle.** THC specifies the stable handle, bootstrap
   and ordinary registration states, dereference during a queued/running handoff,
   explicit finalization and failure handling. Keep key and value distinct.
   Done when each transition identifies who roots the thunk, value and callback,
   and who can claim the real finalizer. Handles retain tokens/control state,
   not strong paths to their conditional objects. Real finalization is at most
   once; pending bootstrap retirement alone does not expire the logical handle.
2. **Implement the language wrapper.** Resolve an already available answer before
   registration. Otherwise capture the thunk, backing value and real finalizer
   in the bootstrap callback. On execution, resolve without forcing, then install
   and publish the replacement registration or invoke the real finalizer. Release
   obsolete captures after a successful handoff. Use the language's actual thunk
   states; do not label an unresolved alias a terminal value. Handle chains and
   cycles without endless re-registration. Done when this works using the existing
   guest API, including when the callback allocates or triggers another collection.
3. **Check the lifetime laws.** Cover an ordinary unlifted key; an already resolved
   thunk; an unresolved thunk that dies; a thunk evaluated before its bootstrap
   runs with an independently live answer; an otherwise dead answer requiring a
   later collection; distinct backing values and value-to-key backedges; dropped
   handles; dereference and explicit-finalize/pump races; nested GC; replacement
   failure; and chains/cycles. Check that keys/backing resources become reclaimable
   after completion. Assert eventual behavior with a bounded test driver, not a
   universal exact collection count. Compare relevant Haskell outcomes without
   claiming identical GC scheduling.
4. **Qualify and document the consumer.** Run the wrapper through existing THC
   modes on matched Jam HotSpot and Native Image packages. Measure the extra
   registrations, temporary retention and finalizer latency for the opted-in
   path. Ordinary weak users continue through their existing path. Reuse existing
   fixtures, API packages and runtime manifests; do not wait for a new provider
   ABI or build a private descriptor runtime. Document any remaining lifecycle
   limits before calling the language support complete.

The withdrawn collector-entry research is available in Git history. It is not an
active dependency or a fallback for this implementation.
