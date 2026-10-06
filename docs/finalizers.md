# Weak associations and finalizers

<!-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com> -->
<!-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0 -->

A `weak_ptr<T>` is an edge the collector rewrites without following it. Sometimes
we want more: keep a value alive while a key is alive, and run an action when the
key dies. `mk_weak(key, value, finalizer, runner)` registers that association and
returns a `weak<V>` handle.

The API is `mk_weak(ptr<K>, ptr<V>, ptr<F>, runner) -> weak<V>`.
Nonnull managed pointers belong to the current heap; the runner must be nonnull.
A null key is dead, and a null value is allowed. The finalizer lives on the heap too;
its manifest or trace describes any captures. The runner is a captureless,
`noexcept` function taking `F*` and returning `void`. A captureless lambda converts
to that function pointer; put managed captures in `F`, not in the runner:

```cpp
#include <cassert>
#include <new>
import jam.unqualified;

struct cleanup {
  ptr<unsigned> key;
  unsigned * calls; // External storage must outlive the registration.
  static constexpr auto manifest = make_manifest<cleanup>(&cleanup::key);
};

int main() {
  unsigned calls = 0;
  heap h;
  heap_scope scope{h};
  auto key = mk<unsigned>(42u);
  auto action = mk<cleanup>(key, &calls);
  auto association = mk_weak(key, key, action, [](cleanup * f) noexcept {
    ++*f->calls;
    assert(*f->key == 42);
    // Publishing f->key in a root or a live object would resurrect it.
    // This registration will not run again.
  });
  collect_major();
  assert(calls == 1 && association.expired());
}
```

Pass `ptr<F>{}` for an action with no managed state. The runner still executes
once, receiving `nullptr`; null state does not cancel the callback. For a nonnull
state, the runner receives its current heap address after forwarding.

The registration alone does not keep its key alive, even if the value or
finalizer points back to the key. When the key is live, the collector traces the
value and finalizer. When it finds a dead key, it expires the registration,
retains the key and finalizer, and queues the runner. The value is retained in
that case only if something else traces it, such as a finalizer capture.

## Running the action

Callbacks run on the collecting thread, after compaction, with the owning heap
bound and ordinary mutation available. A runner gets a borrow of its forwarded
finalizer object, or `nullptr` when no state was supplied. A nonnull borrow
expires on allocation that grows the heap or on collection, like any other `T*`. Copy needed external state and register
roots for managed captures before doing either. Do not keep using `f` afterward.
Rooting an object preserves the object, not an old reference to its address.

Pending and running callbacks retain their keys and finalizer objects through
nested collection. Nested collection can schedule more callbacks; those wait
until the current callback returns. For GC-triggered callbacks, the
initiating collection drains that queue before returning to its caller. No callback unwinds through the collector.
After a callback returns, its temporary retention ends. Objects that were not
resurrected can die on a later collection. Jam does not invoke C++ destructors.

`association.finalize()` retires an active registration and runs its callback now.
Calling it on an expired registration does nothing, including when its callback
is already queued by GC. This explicit operation is synchronous, including
when called from another finalizer. Dropping the handle does not cancel the
registration. Heap destruction expires surviving handles without running their
callbacks; call `finalize()` beforehand when that cleanup matters.

## Keeping a handle

`weak<V>` is an external handle with shared registration state. Keep it outside
the moving heap. Copies observe the same expiration; `lock()` returns a rooted
value while the registration is active, and an empty root after it expires.
Dereferencing that root requires the owning heap current. A null registered value
also produces an empty root. Neither `lock()` nor `expired()` performs collection.
A finalized registration stays expired even when its key is resurrected.

These operations have the same stopped-mutator contract as roots. They do not
provide synchronization with concurrent collection.

## Order matters

After ordinary marking drains, Jam visits registrations in key-offset order,
using registration order to break ties. Each decision drains the tracing it
starts before examining the next key. Retaining an earlier key's finalizer can
therefore keep a later key alive. Decisions already made are not revisited.
Multiple registrations for one key can consequently finalize in different
collections. This is an ordered policy, not GHC's weak-pointer fixed-point rule.

Minor collection treats old keys as live. It retains their young values and
finalizers; a major collection can establish that an old key is dead. Remembered
edges from old objects remain conservative during minors, as usual.

The scan sorts registrations once and walks them once. It does not repeatedly
rescan the list to find closure. Each nonempty tracing batch uses the ordinary
parallel marker; large numbers of registrations can still make those batch
boundaries significant.

[Weak fields and lifetimes](lifetimes.md) · [Inside the collector](collector.md) · [Topic guides](README.md)
