// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>

import jam;

struct node {
  jam::ptr<node> next;
  std::uint64_t value;
  constexpr auto trace(jam::visitor auto & visit) const noexcept { return visit(next); }
};

void check(bool value) noexcept { if (!value) std::abort(); }
static_assert(sizeof(jam::ptr<node>) == 4);
static_assert(std::is_nothrow_convertible_v<std::nullptr_t, jam::ptr<node>>);
static_assert([] {
  jam::ptr<node> p = nullptr;
  p = jam::ptr<node>{7};
  p = nullptr;
  return !p && p.get() == 0 && p == nullptr;
}());
static_assert(std::is_convertible_v<jam::ptr<node>, jam::root<node>>);
static_assert(std::is_nothrow_convertible_v<jam::root<node> const &, jam::ptr<node>>);

void nested_scopes() {
  jam::heap first{jam::options{.capacity = 8, .reserve = 2}};
  jam::heap second{jam::options{.capacity = 8, .reserve = 2}};
  jam::root<node> a, b;
  auto const previous = jam::current_heap();
  {
    jam::heap_scope scope{first};
    check(jam::current_heap() == &first);
    a = jam::make<node>(nullptr, std::uint64_t{41});
    a = jam::make<node>(a, std::uint64_t{99});
    a->value += 1;
    check((*a).value == 100 && a->next->value == 41);
    {
      jam::heap_scope nested{second};
      check(jam::current_heap() == &second);
      b = jam::make<node>(nullptr, std::uint64_t{200});
      auto copy = a; // Registration uses the root's owner, not the current heap.
      copy = {};
      jam::collect();
      check(b->value == 200);
    }
    check(jam::current_heap() == &first && a->next->value == 41);
    static_cast<void>(first.allocate(first.capacity()));
    check(a->value == 100); // Dereference observes the remapped base after growth.
    jam::collect();
    check(a->value == 100 && a->next->value == 41);
  }
  check(jam::current_heap() == previous);
  {
    jam::heap_scope reenter{second};
    check(b->value == 200);
    b->value = 201;
    a = {}; // Unregister from first even though second is current.
  }
  {
    jam::heap_scope reenter{first};
    jam::collect();
    check(first.used() == 1);
  }
  {
    jam::heap_scope reenter{second};
    check(b->value == 201);
  }
}

void const_root() {
  jam::heap heap{jam::options{.capacity = 8, .reserve = 2}};
  jam::heap_scope scope{heap};
  static_cast<void>(jam::make<node>());
  jam::root<node> const root = jam::make<node>(nullptr, std::uint64_t{23});
  auto copy = root; // Linking another root also updates the const root's hook.
  auto const before = root.get().get();
  jam::collect();
  check(root.get().get() < before && root->value == 23);
  jam::ptr<node> p = root;
  check(copy.get() == p && p->value == 23);
}

template<std::size_t Alignment>
struct alignas(Alignment) ordered_node {
  jam::ptr<ordered_node> next;
  std::uint64_t value;
  constexpr auto trace(jam::visitor auto & visit) const noexcept { return visit(next); }
};

template<std::size_t Alignment>
void collection_boundary(std::size_t workers) {
  using node_t = ordered_node<Alignment>;
  jam::heap heap{jam::options{.capacity = 8, .reserve = 2,
                             .workers = workers}};
  jam::heap_scope scope{heap};
  static_cast<void>(heap.allocate(heap.page_words()));
  jam::root const first = jam::make<node_t>(nullptr, std::uint64_t{11});
  static_cast<void>(heap.allocate(heap.page_words()));
  jam::root const second = jam::make<node_t>(first, std::uint64_t{22});
  check(first.get() < second.get() && nullptr < first.get());
  check(first.get() <= second.get() && second.get() > first.get());
  check((first.get() <=> first.get()) == 0);
  auto const first_before = first.get().get();
  auto const second_before = second.get().get();
  auto const old_start = heap.start();
  first->next = second;
  first->value = 101;
  second->value = 202;
  jam::collect();
  check(jam::current_heap() == &heap && heap.start() != old_start);
  check(first.get().get() < first_before && second.get().get() < second_before);
  check(first->next == second.get() && second->next == first.get());
  check(first->next->value == 202 && second->next->value == 101);
  check(first.get() < second.get() && nullptr < first.get());
  check(first.get() <= second.get() && second.get() > first.get());
  check((first.get() <=> first.get()) == 0);
  first->value = 303;
  heap.collect(); // Explicit collection has the same visibility boundary.
  check(second->next->value == 303 && first->next->value == 202);
  check(first.get() < second.get() && nullptr < first.get());
  check(first.get() <= second.get() && second.get() > first.get());
  check((first.get() <=> first.get()) == 0);
}

void thread_scopes() {
  std::barrier gate{2};
  auto work = [&](std::uint64_t value) {
    jam::heap heap{jam::options{.capacity = 8, .reserve = 2}};
    jam::heap_scope scope{heap};
    jam::root<node> root = jam::make<node>(nullptr, value);
    gate.arrive_and_wait();
    check(jam::current_heap() == &heap && root->value == value);
    jam::collect();
    gate.arrive_and_wait();
    check(root->value == value);
  };
  std::jthread a{work, 17}, b{work, 29};
}

int main() {
  nested_scopes();
  const_root();
  thread_scopes();
  for (auto workers : {1u, 4u}) {
    collection_boundary<8>(workers);
    collection_boundary<16>(workers);
    collection_boundary<32>(workers);
    collection_boundary<64>(workers);
  }
  std::puts("implicit heap scope checks passed");
}
