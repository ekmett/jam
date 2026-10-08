# Lifted weak references

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

**Plan agreed October 7, 2026; not implemented.** This replaces the proposed
collector-integrated Lifted interface and collector-entry compiler plan.

A thunk can die while its answer remains live. A weak reference to the thunk
alone therefore has the wrong identity for a Haskell weak key. We need to
follow the answer, but we need not teach the collector what a thunk is.

`lifted_ptr<T>` and `lifted_weak<T>` are concepts in `jam::vm` and its language
implementations. They are not new core Jam pointer types. The language owns
resolution, forcing states and representation; the VM layer supplies ordinary
weak associations and finalizer execution. The names here describe the design,
not shipped C++ templates or Java classes.

## Use the finalizer we already have

An ordinary association is `K ⇒ (V, F)`: an independently live key retains the
value and finalizer. The key and value are separate, even when they initially
name the same object. The value can be a backing object carrying ownership or
finalization state, such as a foreign-resource owner. Its return edge to the
key cannot make the association live by itself.

For an unlifted key, register that association directly. For a lifted key,
the language first checks whether it can resolve the thunk without evaluating
it. If an ordinary target is already available, register against that target.
Otherwise register against the thunk, with a bootstrap finalizer:

```text
register(thunk, value, bootstrap(thunk, value, real_finalizer, control))

when bootstrap runs:
    inspect the thunk using the language's resolution rules
    if a replacement is available:
        install the association against the replacement
        publish its registration in control
    otherwise:
        retire the logical weak handle and invoke the real finalizer
```

This is lifecycle pseudocode, not a new Jam API. An unresolved replacement can
need the same bootstrap strategy again; an ordinary result uses a normal weak
registration. Resolve available chains without forcing, and handle self/cyclic
indirections without endlessly reinstalling equivalent registrations. The
language must distinguish an ordinary terminal value from an unresolved thunk;
`resolve()` returning null alone does not establish that distinction.

The bootstrap runs when the normal finalizer pump invokes it, outside GC.
It can use ordinary language methods, allocate, synchronize and install another
weak association. There is no collector callback to `resolve()` or `project()`.
Existing Java references keep their ordinary types and identities.

## Hold the right things through the handoff

The bootstrap finalizer must capture the thunk, backing value and real finalizer.
While its key is live, these are conditional edges. When the registration
retires, the ordinary finalizer machinery retains the queued/running callback
and its captures. That keeps the thunk and its current answer safe to inspect,
and keeps the backing object available even though the retiring association's
value is no longer dereferenceable.

The replacement registration must be installed and published before releasing
those captures. A callback that triggers collection must obey the usual rooting
and borrowing rules. Reuse the existing finalizer claim/completion protocol;
there must be no interval in which the backing value or real finalizer is lost.
Once the handoff completes, the old callback must release its obsolete captures.
Do not accidentally make the new callback retain the original thunk forever.
References deliberately present in the user's value or finalizer retain their
normal meaning.

The user-facing handle needs stable control state across registrations. That
state may hold tokens and lifecycle state, but the handle must not strongly
retain the thunk, value or callbacks through it. The callbacks may refer to the
control state; the reverse strong path would defeat weak reachability. Dropping
the handle must not cancel finalization.

Bootstrap retirement is not logical retirement. Dereferencing during a queued
or running handoff, explicit finalization racing the pump, nested collection,
and failure while installing a replacement all need a defined language-level
policy. A null result from the expired bootstrap token is not sufficient to
permanently expire the outer handle. The implementation plan requires these
cases to be settled before delivery. Real finalization remains at most once,
and resurrection never rearms a logically retired handle.

## Why this approach

The old plan required collector-safe Java method compilation, special dispatch,
slot rewriting and lifetime rules for installed code. A terminal field-descriptor
API added a competing resolution protocol without implementing the general one.
Both approaches made the collector responsible for a language's indirection
strategy. Neither is the implementation path for this plan.

Here the language chooses how to resolve its values. Jam sees ordinary weak
registrations and ordinary finalizers. No Lifted interface, pointer-kind switch,
class/field registration or resolver metadata is added to the core collector.
Existing weak users incur no new resolution dispatch or bootstrap bookkeeping.
Only users of this strategy pay for its registrations, captured state and handoffs.

The price is delayed reclamation and finalization. A bootstrap callback retains
the thunk and its answer even if both were otherwise dead. Installing a new weak
association can therefore require another collection before the real finalizer
becomes eligible. Longer chains and pump scheduling can add further delay; there
is no fixed one-collection guarantee. Retained captures can also affect other
weak associations during that interval. This is not a claim of identical
collection-by-collection behavior to collector-time normalization or GHC.

This weak strategy does not contract ordinary strong references or discard
selector environments during GC. Such optimizations are separate language/VM
work, not prerequisites for this handoff and not implemented by it.

## Ownership and delivery

THC owns its resolution policy and the language wrapper/state machine. `jam::vm`
provides the existing `jam.vm.Weak` association and pump protocol and checks its
generic retention guarantees on HotSpot and Native Image. Core Jam needs no
change for this plan. Any discovered backend defect must be fixed as an ordinary
weak-association defect, not by adding a lifted-specific hook.

The [implementation plan](lifted-dispatch-plan.md) records the lifecycle and
qualification work. The [weak-pointer contract](weak-pointers.md) remains the
underlying collector contract; this document does not change its fixed point,
batch retirement, generational treatment or finalizer ordering.
