// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
import jam;
using namespace jam;
void check(bool ok, char const * message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct node {
  weak_ptr<node> next;
  unsigned data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};
struct alignas(64) holder {
  std::array<weak_ptr<node>, 3> edges;
  ptr<node> strong;
  std::tuple<weak_ptr<node>> tuple;
  std::variant<unsigned, weak_ptr<node>> variant;
  static constexpr auto manifest = make_manifest<holder>(&holder::edges, &holder::strong, &holder::tuple, &holder::variant);
};
static_assert(sizeof(weak_ptr<node>) == 4);
static_assert([] {
  weak_ptr<unsigned> a = nullptr, b = a;
  swap(a, b);
  a = std::move(b);
  return a.expired() && b.expired();
}());
static_assert(!std::is_convertible_v<weak_ptr<node>, ptr<node>>);
static_assert(std::is_same_v<decltype(std::declval<weak_ptr<node>>().lock()), root<node>>);

void lifecycle(unsigned workers) {
  heap h{{.workers = workers}};
  heap_scope scope{h};
  root box = mk<holder>();
  auto dead = mk<node>(nullptr, 7u);
  auto live = mk<node>(nullptr, 42u);
  dead->next = dead; // Weak self-cycle must not keep this allocation alive.
  box->edges = {dead, live, nullptr};
  box->strong = live;
  std::get<0>(box->tuple) = dead;
  box->variant.emplace<weak_ptr<node>>(live);
  h.collect_major();
  check(box->edges[0].expired() && box->edges[2].expired(), "dead/null weak fields clear");
  check(std::get<0>(box->tuple).expired(), "tuple weak field clears");
  check(box->edges[1].lock()->data == 42, "live weak target forwards");
  check(std::get<weak_ptr<node>>(box->variant).lock()->data == 42, "variant weak field forwards");
  {
    auto locked = box->edges[1].lock();
    box->strong = nullptr;
    h.collect_major();
    check(locked->data == 42 && !box->edges[1].expired(), "lock retains a strong root");
  }
  h.collect_major();
  check(box->edges[1].expired() && !box->edges[1].lock(), "unlock permits reclamation");

  // Old source slots need rewriting without becoming strong minor roots.
  box->edges[0] = mk<node>(nullptr, 9u);
  box->edges[1] = mk<node>(nullptr, 13u);
  box->strong = box->edges[1].lock();
  check(h.remembered_size() == 3, "weak and strong old-to-young slots register");
  h.collect_minor();
  check(box->edges[0].expired(), "minor clears weak-only young target");
  check(box->edges[1].lock()->data == 13, "minor updates surviving weak target");
  check(h.remembered_size() == 2, "expired remembered slot removed");
  h.collect_minor(true);
  check(!box->strong.is_young() && box->edges[1].lock()->data == 13, "promotion updates weak generation");
  check(h.remembered_size() == 0, "promotion clears remembered slots");
  box->strong = nullptr;
  h.collect_minor();
  check(!box->edges[1].expired(), "minor cannot decide old target liveness");
  h.collect_major();
  check(box->edges[1].expired(), "major clears unreachable old target");

  // Assignment, move and swap must track the source slot, not its target.
  root young_box = mk<holder>();
  weak_ptr<node> stack = mk<node>(nullptr, 17u);
  box->edges[0] = mk<node>(nullptr, 18u);
  box->edges[0] = mk<node>(nullptr, 19u);
  check(h.remembered_size() == 1, "repeated weak stores deduplicate");
  swap(stack, box->edges[0]);
  swap(young_box->edges[0], box->edges[0]);
  check(h.remembered_size() == 0, "old slot swapped to null unregisters");
  box->edges[1] = std::move(young_box->edges[0]);
  check(young_box->edges[0].expired() && h.remembered_size() == 1, "move nulls source and registers old destination");
  box->edges[2] = std::move(box->edges[1]);
  check(box->edges[1].expired() && h.remembered_size() == 1, "old-to-old move transfers registration");
  h.collect_minor();
  check(box->edges[2].expired(), "moved weak slot does not retain target");
  check(h.remembered_size() == 0, "cleared moved weak slot unregisters");
  assign(box->edges, std::array<weak_ptr<node>, 3>{mk<node>(nullptr, 23u), nullptr, nullptr});
  check(h.remembered_size() == 1, "bulk assignment registers weak slots");
  assign(box->edges, std::array<weak_ptr<node>, 3>{});
  check(h.remembered_size() == 0, "bulk null assignment removes weak slots");
  box->edges[0].unsafe_assign(weak_ptr{mk<node>(nullptr, 29u)});
  h.remember(box->edges[0]);
  h.collect_minor();
  check(box->edges[0].expired(), "explicit weak barrier does not seed marking");
  root observer = mk<weak_ptr<node>>(mk<node>(nullptr, 31u));
  h.collect_major();
  check(observer->expired(), "rooting a weak field does not root its target");
}
int main() {
  lifecycle(1);
  lifecycle(4);
  std::puts("weak pointer checks passed");
}
