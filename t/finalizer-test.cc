// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
import jam;
using namespace jam;
void check(bool ok, char const * message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct node {
  ptr<node> next;
  unsigned data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};
struct finalizer {
  ptr<node> key, value;
  unsigned * calls;
  static constexpr auto manifest = make_manifest<finalizer>(&finalizer::key, &finalizer::value);
};
void count(finalizer * f) noexcept { ++*f->calls; }
void lifecycle(unsigned workers) {
  heap h{{.workers = workers}};
  heap_scope scope{h};
  unsigned calls = 0;
  root key = mk<node>(nullptr, 7u);
  auto value = mk<node>(key.get(), 42u); // Value-to-key cycle is conditional.
  auto weak = mk_weak(key.get(), value, mk<finalizer>(key.get(), value, &calls),
    [](finalizer * f) noexcept {
      ++*f->calls;
      root key = f->key, value = f->value;
      check(key->data == 7 && value->data == 42, "managed finalizer captures forwarded");
      collect_major(); // Ends the F* borrow. Reacquire any needed state through roots.
      check(key->data == 7 && value->data == 42, "callback roots survive nested GC");
    });
  h.collect_major();
  check(calls == 0 && !weak.expired() && weak.lock()->data == 42, "live key retains value");
  key = {};
  h.collect_major();
  check(calls == 1 && weak.expired() && !weak.lock(), "dead key finalizes exactly once");
  h.collect_major();
  check(calls == 1 && h.used() == 1, "retained graph dies after finalizer returns");
}
struct resuscitator {
  ptr<node> key;
  root<node> * saved;
  unsigned * calls;
  static constexpr auto manifest = make_manifest<resuscitator>(&resuscitator::key);
};
void resurrection() {
  heap h;
  heap_scope scope{h};
  root<node> saved;
  unsigned calls = 0;
  auto key = mk<node>(nullptr, 19u);
  auto registration = mk_weak(key, ptr<node>{}, mk<resuscitator>(key, &saved, &calls),
    [](resuscitator * f) noexcept {
      ++*f->calls;
      auto & saved = *f->saved;
      saved = f->key;
      auto next = mk<node>(nullptr, 23u);
      saved->next = next; // Reborrow after allocation; normal old-to-young barrier.
      collect_minor();
      check(saved->data == 19 && saved->next->data == 23, "finalizer can allocate, mutate and collect");
    });
  collect_major();
  check(calls == 1 && saved->data == 19 && registration.expired(), "resurrection does not rearm finalization");
  saved = {};
  collect_major();
  check(calls == 1 && h.used() == 1, "resurrected key later dies without another finalizer");
}
void dropped_handles_and_minors() {
  heap h;
  heap_scope scope{h};
  unsigned calls = 0;
  root key = mk<node>(nullptr, 5u);
  collect_major();
  auto weak = mk_weak(key.get(), mk<node>(nullptr, 11u), mk<finalizer>(key.get(), nullptr, &calls), count);
  key = {};
  collect_minor();
  check(calls == 0 && weak.lock()->data == 11, "old key conservatively retains young value");
  collect_major();
  check(calls == 1 && weak.expired(), "major finalizes unreachable old key");
  auto discarded = mk<node>(nullptr, 17u);
  static_cast<void>(mk_weak(discarded, discarded, mk<finalizer>(discarded, nullptr, &calls),
    [](finalizer * f) noexcept { check(f->key->data == 17, "discarded handle key remains valid"); ++*f->calls; }));
  collect_minor(true);
  check(calls == 2, "discarding a handle does not cancel its finalizer");
  collect_major();
  check(h.used() == 1, "promoted finalizer key is subsequently reclaimed");
}
struct nested_finalizer {
  ptr<nested_finalizer> self;
  ptr<node> key;
  unsigned * first;
  unsigned * second;
  static constexpr auto manifest = make_manifest<nested_finalizer>(&nested_finalizer::self, &nested_finalizer::key);
};
void pending_roots_and_explicit_finalization() {
  heap h;
  heap_scope scope{h};
  unsigned first = 0, second = 0;
  auto k = mk<node>(nullptr, 1u);
  auto f = mk<nested_finalizer>(nullptr, k, &first, &second);
  f->self = f;
  auto a = mk_weak(k, ptr<node>{}, f, [](nested_finalizer * f) noexcept {
    // The runner gets a borrow, not a root. Root self before invalidating it.
    root self = f->self;
    ++*f->first;
    collect_major();
    check(self->key->data == 1 && *self->second == 0, "nested GC retains active F and defers pending callbacks");
  });
  auto k2 = mk<node>(nullptr, 2u);
  auto b = mk_weak(k2, ptr<node>{}, mk<finalizer>(k2, nullptr, &second), [](finalizer * f) noexcept {
    ++*f->calls; check(f->key->data == 2, "pending F and its key forwarded by nested collection");
  });
  collect_major();
  check(first == 1 && second == 1 && a.expired() && b.expired(), "queued callbacks run once");
  root key = mk<node>(nullptr, 3u);
  auto c = mk_weak(key.get(), key.get(), mk<finalizer>(key.get(), nullptr, &first), count);
  auto copy = c;
  c.finalize(); copy.finalize();
  check(first == 2 && copy.expired(), "explicit finalization is shared and idempotent");
}
struct retained_finalizer {
  weak_ptr<node> key;
  root<weak_ptr<retained_finalizer>> * observer;
  unsigned * calls;
  static constexpr auto manifest = make_manifest<retained_finalizer>(&retained_finalizer::key);
};
void implicit_callback_roots() {
  heap h;
  heap_scope scope{h};
  unsigned calls = 0;
  root<weak_ptr<retained_finalizer>> observer;
  auto k = mk<node>(nullptr, 29u);
  auto f = mk<retained_finalizer>(k, &observer, &calls);
  observer = mk<weak_ptr<retained_finalizer>>(f);
  auto registration = mk_weak(k, ptr<node>{}, f, [](retained_finalizer * f) noexcept {
    ++*f->calls;
    auto & observer = *f->observer;
    // Neither this root nor F's weak key retains either target by itself.
    collect_major();
    auto alive = observer->lock();
    check(alive && alive->key.lock()->data == 29, "collector retains active F and K without callback roots");
  });
  collect_major();
  check(calls == 1 && registration.expired(), "active-only roots callback completed");
  collect_major();
  check(observer->expired(), "callback-only retention ends on return");
}
void conditional_value() {
  heap h;
  heap_scope scope{h};
  unsigned calls = 0;
  auto k = mk<node>(nullptr, 1u), v = mk<node>(nullptr, 2u);
  root witness = mk<weak_ptr<node>>(v);
  auto association = mk_weak(k, v, mk<finalizer>(k, nullptr, &calls), count);
  collect_major();
  check(calls == 1 && association.expired() && witness->expired(), "dead key does not retain V without a capture");
}
void ordered_scan() {
  heap h{{.workers = 4}};
  heap_scope scope{h};
  unsigned first = 0, second = 0;
  auto k1 = mk<node>(nullptr, 1u), k2 = mk<node>(nullptr, 2u);
  // Retaining the earlier dead key's F marks the later key before its decision.
  auto a = mk_weak(k1, ptr<node>{}, mk<finalizer>(k1, k2, &first), count);
  auto b = mk_weak(k2, mk<node>(nullptr, 42u), mk<finalizer>(k2, nullptr, &second), count);
  collect_major();
  check(first == 1 && second == 0 && a.expired() && b.lock()->data == 42, "decisions use a drained ordered frontier");
  collect_major();
  check(second == 1 && b.expired(), "later finalizer runs after earlier retention ends");
}
void handle_lifetime() {
  weak<node> surviving;
  unsigned calls = 0;
  {
    heap h;
    heap_scope scope{h};
    auto k = mk<node>(nullptr, 1u);
    surviving = mk_weak(k, k, mk<finalizer>(k, nullptr, &calls), count);
  }
  check(surviving.expired() && !surviving.lock() && calls == 0, "heap teardown expires handles without running callbacks");
  surviving.finalize();
}
void null_finalizer_state() {
  heap h;
  heap_scope scope{h};
  static unsigned calls = 0;
  calls = 0;
  auto runner = [](unsigned * state) noexcept {
    check(state == nullptr, "stateless callback receives nullptr");
    ++calls;
  };
  root key = mk<node>(nullptr, 31u);
  auto association = mk_weak(key.get(), key.get(), ptr<unsigned>{}, runner);
  collect_major();
  check(calls == 0 && association.lock()->data == 31, "null state preserves live-key semantics");
  key = {};
  collect_major(); collect_major();
  check(calls == 1 && association.expired(), "null state still runs once on key death");
  auto young = mk<node>(nullptr, 37u);
  auto minor = mk_weak(young, young, ptr<unsigned>{}, runner);
  collect_minor();
  check(calls == 2 && minor.expired(), "minor collection invokes stateless callback");
  auto explicit_action = mk_weak(ptr<node>{}, ptr<node>{}, ptr<unsigned>{}, runner);
  explicit_action.finalize(); explicit_action.finalize();
  check(calls == 3 && explicit_action.expired(), "explicit stateless finalization runs once");
}
int main() {
  lifecycle(1); lifecycle(4);
  resurrection(); dropped_handles_and_minors(); pending_roots_and_explicit_finalization();
  implicit_callback_roots(); conditional_value(); ordered_scan(); handle_lifetime(); null_finalizer_state();
  std::puts("finalizer checks passed");
}
