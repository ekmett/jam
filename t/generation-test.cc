// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <variant>
#include <utility>
import jam;
using namespace jam;
void check(bool ok, char const * message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct node {
  ptr<node> next;
  std::uint64_t data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};
struct array_node {
  std::array<ptr<node>, 3> edges;
  static constexpr auto manifest = make_manifest<array_node>(&array_node::edges);
};
struct counted_node {
  static inline unsigned traces = 0;
  ptr<node> next;
  constexpr void trace(visitor auto & visit) const noexcept {
    ++traces;
    visit(next);
  }
};
struct changing_node {
  union { ptr<node> edge; ptr<std::uint64_t> number; std::uint32_t bits; };
  unsigned kind = 0;
  changing_node() noexcept : edge(nullptr) {}
  ~changing_node() noexcept {
    if (kind == 0) std::destroy_at(&edge);
    else if (kind == 1) std::destroy_at(&number);
  }
  void trace(visitor auto & visit) const noexcept {
    if (kind == 0) visit(edge);
    else if (kind == 1) visit(number);
  }
};
struct alignas(64) aligned_node {
  ptr<aligned_node> next;
  std::uint64_t data;
  static constexpr auto manifest = make_manifest<aligned_node>(&aligned_node::next);
};
// Allocation tracing must dispatch before claiming a base-sized prefix.
struct polymorphic_base {
  ptr<polymorphic_base> next;
  explicit polymorphic_base(ptr<polymorphic_base> p) noexcept : next(p) {}
  virtual void claim_and_trace(heap::visitor &) const noexcept = 0;
  virtual std::uint64_t value() const noexcept = 0;
};
struct alignas(32) small_polymorphic final : polymorphic_base {
  std::uint64_t data;
  small_polymorphic(ptr<polymorphic_base> p, std::uint64_t n) noexcept : polymorphic_base(p), data(n) {}
  void claim_and_trace(heap::visitor & visit) const noexcept override {
    if (visit.claim_target(this)) visit(next);
  }
  std::uint64_t value() const noexcept override { return data; }
};
struct alignas(64) large_polymorphic final : polymorphic_base {
  std::array<std::uint64_t, 33> data{};
  ptr<polymorphic_base> other;
  large_polymorphic(ptr<polymorphic_base> p, std::uint64_t n) noexcept : polymorphic_base(p), other(p) { data.back() = n; }
  void claim_and_trace(heap::visitor & visit) const noexcept override {
    if (visit.claim_target(this)) visit(next, other);
  }
  std::uint64_t value() const noexcept override { return data.back(); }
};
struct throwing_allocation_hook {
  void claim_and_trace(heap::visitor &) const;
};
struct wrong_allocation_hook {
  int claim_and_trace(heap::visitor &) const noexcept;
  static constexpr auto manifest = make_manifest<wrong_allocation_hook>();
};
struct virtual_polymorphic : virtual polymorphic_base {};
static_assert(!traceable<throwing_allocation_hook> && !traceable<wrong_allocation_hook>);
static_assert(!std::convertible_to<ptr<virtual_polymorphic>, ptr<polymorphic_base>>);
static_assert(traceable<polymorphic_base> && traceable<large_polymorphic>);
static_assert(sizeof(large_polymorphic) > heap::block_words * sizeof(heap::word));
void polymorphic_records(unsigned workers) {
  heap h{{.workers = workers}};
  heap_scope scope{h};
  static_cast<void>(mk<node>(nullptr, 99u));
  root<polymorphic_base> first = mk<small_polymorphic>(nullptr, 11u);
  root<polymorphic_base> second = mk<large_polymorphic>(first.get(), 22u);
  first->next = second.get();
  auto verify = [&] {
    check(first->value() == 11 && second->value() == 22, "virtual dispatch preserves derived payloads");
    check(first->next == second.get() && second->next == first.get(), "polymorphic cycle survives forwarding");
    auto const * large = static_cast<large_polymorphic const *>(second.operator->());
    check(large->other == first.get(), "derived pointer beyond the base extent is tagged and forwarded");
    check(reinterpret_cast<std::uintptr_t>(large) % 64 == 0, "dynamic claim preserves derived alignment");
  };
  h.collect_minor(); verify();
  h.collect_minor(true); verify();
  h.collect_major(); verify();
  // Only the old field retains this young allocation; its remembered callback
  // is keyed by the abstract base and must still dispatch to the derived hook.
  first->next = mk<large_polymorphic>(first.get(), 33u);
  check(h.remembered_size() == 1, "base pointer assignment registers the young target");
  for (unsigned round = 0; round != 4; ++round) {
    if (round == 0) h.collect_minor();
    else if (round == 1) h.collect_minor(true);
    else h.collect_major();
    check(first->next->value() == 33 && first->next->next == first.get(),
          "remembered polymorphic edge survives minor, promotion and major collection");
    auto const * large = static_cast<large_polymorphic const *>(first->next.operator->());
    check(large->other == first.get() && reinterpret_cast<std::uintptr_t>(large) % 64 == 0,
          "polymorphic dynamic extent and alignment survive repeated movement");
  }
}

void growth(unsigned workers) {
  heap h{{.old = {.capacity = units::pages{4}, .reserve = units::pages{1}, .maximum = 4_MiB},
          .young = {.capacity = units::pages{4}, .reserve = units::pages{1}, .maximum = 4_MiB}, .workers = workers}};
  heap_scope scope{h};
  root seed = mk<node>(nullptr, 0x123456789abcdef0ULL);
  auto fill = [&] {
    while (h.young().used() < h.young().capacity() - h.young().reserved() - 1)
      static_cast<void>(mk<std::uint64_t>(0u));
  };
  fill();
  auto before = h.young().capacity();
  root clone = mk<node>(*seed);
  check(h.young().capacity() > before && clone->data == seed->data,
        "cloning a borrowed record snapshots before nursery growth");
  fill();
  before = h.young().capacity();
  root fields = mk<node>(seed->next, seed->data);
  check(h.young().capacity() > before && fields->data == seed->data && !fields->next,
        "borrowed field constructor arguments survive nursery growth");
  root anchor = mk<node>(nullptr, 0u);
  h.collect_major();
  root<aligned_node> chain;
  constexpr unsigned count = 4096;
  for (unsigned round = 0; round != 3; ++round) {
    anchor->next = mk<node>(nullptr, round + 100u);
    for (unsigned i = 0; i != count; ++i) chain = mk<aligned_node>(chain.get(), i + round * count);
    h.collect_minor(true);
    check(anchor->next->data == round + 100u, "remembered source survives old arena growth during promotion");
    check(!chain.get().is_young(), "grown chain is promoted");
    auto at = chain.get();
    for (unsigned remaining = (round + 1) * count; remaining; --remaining) {
      check(at->data == remaining - 1, "cross-generation chain retains all predecessors");
      check(reinterpret_cast<std::uintptr_t>(h.address(at)) % 64 == 0, "promotion preserves 64-byte record alignment");
      at = at->next;
    }
    check(!at && h.old().data()[0] == 0 && h.young().data()[0] == 0, "both generations preserve reserved null cells");
  }
  h.collect_major();
  check(chain->data == 3 * count - 1, "full collection survives repeated growth and promotion");
}
void pointer_moves() {
  heap h;
  heap_scope scope{h};
  root source = mk<node>(nullptr, 1u);
  root target = mk<node>(nullptr, 2u);
  h.collect_major();
  source->next = mk<node>(nullptr, 11u);
  ptr<node> local = std::move(source->next);
  check(!source->next && local->data == 11 && h.remembered_size() == 0,
        "move construction clears and forgets an old source slot");
  target->next = std::move(local);
  check(!local && h.remembered_size() == 1, "move assignment remembers its old destination");
  source->next = std::move(target->next);
  check(!target->next && h.remembered_size() == 1, "old-to-old move transfers the remembered slot");
  auto & same = source->next;
  same = std::move(source->next);
  check(same && h.remembered_size() == 1, "self move preserves the pointer and barrier");
  h.collect_minor();
  check(source->next->data == 11 && !target->next, "moved edge survives minor collection");
  ptr<node> old = target.get();
  source->next = std::move(old);
  check(!old && h.remembered_size() == 0, "moving an old target replaces a remembered young edge");
  h.collect_minor();
  check(h.young().used() == 1 && source->next == target.get(), "replaced young target is reclaimed");
  ptr<node> empty;
  source->next = std::move(empty);
  check(!empty && !source->next, "moving null clears the destination");
  h.collect_major();
  check(!source->next && source->data == 1 && target->data == 2, "move bookkeeping survives major collection");

  using std::swap;
  source->next = mk<node>(nullptr, 701u);
  target->next = mk<node>(nullptr, 702u);
  swap(source->next, target->next);
  check(source->next->data == 702 && target->next->data == 701 && h.remembered_size() == 2,
        "ADL swap exchanges young targets without changing their remembered slots");
  target->next = source.get();
  swap(source->next, target->next);
  swap(source->next, source->next);
  check(source->next == source.get() && target->next->data == 702 && h.remembered_size() == 1,
        "mixed-generation swap transfers registration; self swap preserves it");
  h.collect_minor();
  check(source->next == source.get() && target->next->data == 702, "swapped edges survive collection");
  swap(target->next, empty);
  check(!target->next && empty->data == 702 && h.remembered_size() == 0,
        "heap-to-stack swap with null removes the remembered source slot");
  root kept = empty;
  h.collect_major();
  check(kept->data == 702 && source->next == source.get(), "rooted swapped target and self edge survive major collection");
}
void pointer_swap_locations() {
  // Location: stack, old, young. Target: null, old, young.
  for (unsigned left_location = 0; left_location != 3; ++left_location)
    for (unsigned right_location = 0; right_location != 3; ++right_location)
      for (unsigned left_kind = 0; left_kind != 3; ++left_kind)
        for (unsigned right_kind = 0; right_kind != 3; ++right_kind) {
          heap h;
          heap_scope scope{h};
          root old_fields = mk<array_node>();
          std::array<root<node>, 2> old_targets{mk<node>(nullptr, 101u), mk<node>(nullptr, 102u)};
          h.collect_major();
          root young_fields = mk<array_node>();
          static_cast<void>(mk<node>(nullptr, 999u)); // Force young targets to move.
          std::array<root<node>, 2> young_targets{mk<node>(nullptr, 201u), mk<node>(nullptr, 202u)};
          std::array<ptr<node>, 2> stack;
          auto slot = [&](unsigned location, unsigned index) -> ptr<node> & {
            if (location == 0) return stack[index];
            if (location == 1) return old_fields->edges[index];
            return young_fields->edges[index];
          };
          auto target = [&](unsigned kind, unsigned index) -> ptr<node> {
            if (kind == 1) return old_targets[index].get();
            if (kind == 2) return young_targets[index].get();
            return nullptr;
          };
          slot(left_location, 0) = target(left_kind, 0);
          slot(right_location, 1) = target(right_kind, 1);
          auto const remembered = [](unsigned location, unsigned kind) { return location == 1 && kind == 2; };
          using std::swap;
          swap(slot(left_location, 0), slot(left_location, 0));
          check(h.remembered_size() == remembered(left_location, left_kind) + remembered(right_location, right_kind),
                "self swap preserves remembered slots in every storage location");
          swap(slot(left_location, 0), slot(right_location, 1));
          check(slot(left_location, 0) == target(right_kind, 1) && slot(right_location, 1) == target(left_kind, 0),
                "swap exchanges null/old/young targets across every storage pair");
          check(h.remembered_size() == remembered(left_location, right_kind) + remembered(right_location, left_kind),
                "only old slots now holding young targets are remembered");
          root<node> left_stack_root, right_stack_root;
          if (left_location == 0) left_stack_root = stack[0];
          if (right_location == 0) right_stack_root = stack[1];
          old_targets = {}; young_targets = {};
          auto verify = [&] {
            auto const left = left_location == 0 ? left_stack_root.get() : slot(left_location, 0);
            auto const right = right_location == 0 ? right_stack_root.get() : slot(right_location, 1);
            check(right_kind ? left && left->data == right_kind * 100 + 2 : !left,
                  "swapped left target survives through its slot or external root");
            check(left_kind ? right && right->data == left_kind * 100 + 1 : !right,
                  "swapped right target survives through its slot or external root");
          };
          h.collect_minor(); verify();
          h.collect_major(); verify();
          check(h.remembered_size() == 0, "major collection clears swap registrations");
        }
}
void collection_schedule() {
  heap h{{.minor_collections = 2}};
  heap_scope scope{h};
  root parent = mk<counted_node>();
  collect();
  check(parent.get().is_young(), "a new heap starts with its configured minor countdown");
  collect_major();
  counted_node::traces = 0;
  auto const old_parent = parent.get();
  for (unsigned cycle = 0; cycle != 2; ++cycle) {
    for (unsigned i = 1; i <= 3; ++i) {
      parent->next = mk<node>(nullptr, i);
      collect();
      check(parent->next->data == i, "scheduled collection preserves remembered edges");
      check(parent->next.is_young() == (i != 3), "every third scheduled collection is major");
      check(counted_node::traces == cycle + (i == 3), "only scheduled majors trace old records");
      if (cycle == 0 && i < 3) check(parent.get() == old_parent, "scheduled minors leave old fixed");
    }
  }
  h.collect(); // One tick into the next cycle.
  h.collect_major();
  counted_node::traces = 0;
  parent->next = mk<node>(nullptr, 42u);
  h.collect();
  collect_minor();
  h.collect_minor(true);
  check(counted_node::traces == 0 && !parent->next.is_young(), "explicit minor promotion leaves old untraced");
  h.collect();
  check(counted_node::traces == 0, "explicit major resets and explicit minors preserve the countdown");
  h.collect();
  check(counted_node::traces == 1 && parent->next->data == 42, "reset countdown reaches its next major");

  heap always{{.minor_collections = 0}};
  {
    heap_scope scope{always};
    root value = mk<node>(nullptr, 99u);
    collect();
    check(!value.get().is_young(), "zero minors always collects both generations");
    value = mk<node>(nullptr, 100u);
    always.collect([](heap::visitor &, heap::offset) noexcept { std::abort(); });
    check(!value.get().is_young() && value->data == 100, "callback overload uses the same policy");
  }
}
int main() {
  polymorphic_records(1);
  polymorphic_records(4);
  pointer_moves();
  pointer_swap_locations();
  collection_schedule();
  for (unsigned workers : {1u, 4u}) {
    growth(workers);
    heap h{{.old = {.capacity = 1_MiB, .reserve = units::ceil<units::pages>(256_KiB), .maximum = 4_MiB},
            .young = {.capacity = 1_MiB, .reserve = units::ceil<units::pages>(256_KiB), .maximum = 4_MiB}, .workers = workers}};
    heap_scope scope{h};
    check(reinterpret_cast<std::uintptr_t>(h.old().data()) < reinterpret_cast<std::uintptr_t>(h.young().data()), "old is below young");
    root parent = mk<node>(nullptr, 7u);
    check(parent.get().is_young(), "typed allocation starts in young");
    h.collect_major();
    check(!parent.get().is_young(), "full collection promotes into old");
    auto const old_parent = parent.get();
    for (unsigned i = 0; i != 100; ++i) parent->next = mk<node>(nullptr, i);
    check(h.remembered_size() == 1, "repeated writes deduplicate the source slot");
    collect_minor();
    check(parent.get() == old_parent && parent->next->data == 99, "minor GC retains current edge and leaves old fixed");
    check(parent->next.is_young(), "minor GC retains the young tag");
    parent->next = nullptr;
    h.collect_minor();
    check(h.young().used() == 1, "overwritten targets are reclaimed");
    parent->next.unsafe_assign(mk<node>(nullptr, 123u));
    h.remember(parent->next);
    h.collect_minor();
    check(parent->next->data == 123, "unsafe assignment plus explicit registration retains the edge");
    h.collect_major();
    check(!parent->next.is_young() && h.remembered_size() == 0 && h.young().used() == 1, "full GC clears young and the remembered set");

    root array = mk<array_node>();
    root counted = mk<counted_node>();
    h.collect_major();
    counted_node::traces = 0;
    auto const old_array = array.get();
    array->edges = {mk<node>(nullptr, 201u), mk<node>(nullptr, 202u), nullptr};
    counted->next = mk<node>(nullptr, 203u);
    h.collect_minor();
    check(counted_node::traces == 0, "minor collection does not trace old objects");
    check(array.get() == old_array && array->edges[0]->data == 201 && array->edges[1]->data == 202,
          "array assignment registers each source slot");
    assign(array->edges, std::array<ptr<node>, 3>{nullptr, mk<node>(nullptr, 204u), mk<node>(nullptr, 205u)});
    h.collect_minor(true);
    check(counted_node::traces == 0 && array.get() == old_array, "promotion does not walk or move old objects");
    check(!array->edges[1].is_young() && !array->edges[2].is_young() && array->edges[2]->data == 205,
          "bulk array assignment and promotion preserve young targets");
    check(h.young().used() == 1 && h.remembered_size() == 0, "promotion empties young and clears remembered slots");

    // Both virtual aliases name one remembered source slot.
    auto * primary = h.address(parent.get());
    auto const base = reinterpret_cast<std::uintptr_t>(h.old().data() - h.old().start());
    auto const bytes = h.old().capacity() * sizeof(heap::word);
    auto const address = reinterpret_cast<std::uintptr_t>(primary);
    auto * alias = reinterpret_cast<node *>(address < base + bytes ? address + bytes : address - bytes);
    primary->next = mk<node>(nullptr, 301u);
    alias->next = mk<node>(parent.get(), 302u);
    check(h.remembered_size() == 1, "double-mapped aliases deduplicate source slots");
    h.collect_minor();
    check(parent->next->data == 302 && parent->next->next == parent.get(),
          "minor collection reads the latest aliased write and preserves an old-young cycle");

    root changing = mk<changing_node>();
    h.collect_major();
    changing->edge = mk<node>(nullptr, 401u);
    check(h.remembered_size() == 1, "active union pointer is remembered");
    std::destroy_at(&changing->edge);
    std::construct_at(&changing->bits, 0xffffffffu);
    changing->kind = 2;
    check(h.remembered_size() == 0, "ending pointer lifetime forgets the source slot");
    h.collect_minor();
    check(changing->bits == 0xffffffffu && h.young().used() == 1, "scalar replacement is never interpreted as a pointer");
    std::construct_at(&changing->number, mk<std::uint64_t>(501u));
    changing->kind = 1;
    h.collect_minor();
    check(*changing->number == 501u, "reused source slot uses the new target's tracer");
    h.collect_major();
    check(!changing->number.is_young() && *changing->number == 501u, "full collection traces the current union alternative");
  }
  std::puts("generational checks passed");
}
