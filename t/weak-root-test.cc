// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>
import jam.unqualified;
void check(bool ok, char const * message) noexcept {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct node {
  ptr<node> next;
  unsigned data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};
static_assert(sizeof(weak_root<node>) == 4 && sizeof(root<node>) == 4);
static_assert(std::is_nothrow_copy_constructible_v<weak_root<node>>);
static_assert(std::is_nothrow_move_constructible_v<weak_root<node>>);
static_assert(std::is_nothrow_copy_assignable_v<weak_root<node>>);
static_assert(std::is_nothrow_move_assignable_v<weak_root<node>>);
static_assert(!std::is_convertible_v<weak_root<node>, ptr<node>>);
static_assert(!std::is_convertible_v<weak_root<node>, root<node>>);
void lifetime(unsigned workers) {
  heap h{{.workers = workers}};
  heap_scope scope{h};
  static_cast<void>(mk<node>(nullptr, 99u));
  root kept = mk<node>(nullptr, 42u);
  weak_root watch = kept;
  weak_root dead = mk<node>(nullptr, 17u);
  auto copy = watch;
  auto moved = std::move(copy);
  check(copy.expired() && !copy.lock(), "move empties source");
  std::vector<weak_root<node>> slots(64, moved);
  collect_minor();
  check(dead.expired() && !dead.lock(), "weak-only young target dies");
  check(watch.lock().get() == kept.get() && moved.lock()->data == 42, "minor forwards weak slots");
  for (auto const & slot : slots) check(slot.lock().get() == kept.get(), "copies and vector moves preserve registrations");
  collect_minor(true);
  check(!kept.get().is_young() && watch.lock().get() == kept.get(), "promotion forwards weak slots");
  auto locked = watch.lock();
  kept = {};
  collect_major();
  check(locked->data == 42 && !watch.expired(), "lock retains across major collection");
  locked = {};
  collect_minor();
  check(!watch.expired(), "minor cannot decide old liveness");
  collect_major();
  check(watch.expired() && moved.expired(), "weak roots do not retain old target");
  for (auto const & slot : slots) check(slot.expired(), "all copies clear");
  check(h.used() == 1, "weak roots do not retain heap allocations");
  watch.reset();
  watch = mk<node>(nullptr, 81u);
  collect_major();
  check(watch.expired(), "assignment remains weak");
}
void scope_and_slots() {
  heap first, second;
  weak_root<node> a, b;
  {
    heap_scope scope{first};
    root kept = mk<node>(nullptr, 11u);
    a = weak_root{kept};
    auto copy = a;
    std::vector<weak_root<node>> many;
    for (unsigned i = 0; i != 4096; ++i) many.push_back(first.weak_root(kept.get()));
    for (unsigned i = 0; i != many.size(); i += 2) many[i].reset();
    for (unsigned i = 0; i != many.size(); i += 2) many[i] = first.weak_root(kept.get());
    swap(a, copy);
    auto & self = a;
    a = self; a = std::move(self); swap(a, a);
    {
      heap_scope nested{second};
      b = second.weak_root(mk<node>(nullptr, 22u));
      collect_major();
      check(b.expired(), "nested heap has independent root slots");
      b.reset();
    }
    collect_major();
    for (auto const & slot : many) check(slot.lock()->data == 11, "weak slots survive table growth and reuse");
    auto locked = a.lock();
    kept = {};
    collect_major();
    check(locked->data == 11, "lock retains target in current heap");
  }
  {
    heap_scope reenter{first};
    collect_major();
    check(a.expired(), "reentered heap resolves dormant weak handle");
    a.reset();
  }
  weak_root<node> empty;
  check(empty.expired() && !empty.lock(), "detached null needs no heap");
}
struct cleanup {
  weak_root<node> * observation;
  unsigned * calls;
};
void finalization() {
  heap h;
  heap_scope scope{h};
  auto key = mk<node>(nullptr, 73u);
  weak_root watch = key;
  unsigned calls = 0;
  auto registration = mk_weak(key, key, mk<cleanup>(&watch, &calls), [](cleanup * f) noexcept {
    ++*f->calls;
    check(f->observation->lock()->data == 73, "weak roots forwarded before finalizer callback");
  });
  collect_major();
  check(calls == 1 && registration.expired() && !watch.expired(), "finalizer's retained key remains observable");
  collect_major();
  check(watch.expired() && !watch.lock(), "unrescued key dies next collection");
}
int main() {
  lifetime(1); lifetime(4); scope_and_slots(); finalization();
  std::puts("weak root lifetime, relocation, scopes and finalization passed");
}
