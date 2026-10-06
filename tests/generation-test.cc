// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <variant>
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
  h.collect();
  root<aligned_node> chain;
  constexpr unsigned count = 4096;
  for (unsigned round = 0; round != 3; ++round) {
    anchor->next = mk<node>(nullptr, round + 100u);
    for (unsigned i = 0; i != count; ++i) chain = mk<aligned_node>(chain.get(), i + round * count);
    h.collect_young(true);
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
  h.collect();
  check(chain->data == 3 * count - 1, "full collection survives repeated growth and promotion");
}
int main() {
  for (unsigned workers : {1u, 4u}) {
    growth(workers);
    heap h{{.old = {.capacity = 1_MiB, .reserve = units::ceil<units::pages>(256_KiB), .maximum = 4_MiB},
            .young = {.capacity = 1_MiB, .reserve = units::ceil<units::pages>(256_KiB), .maximum = 4_MiB}, .workers = workers}};
    heap_scope scope{h};
    check(reinterpret_cast<std::uintptr_t>(h.old().data()) < reinterpret_cast<std::uintptr_t>(h.young().data()), "old is below young");
    root parent = mk<node>(nullptr, 7u);
    check(parent.get().is_young(), "typed allocation starts in young");
    h.collect();
    check(!parent.get().is_young(), "full collection promotes into old");
    auto const old_parent = parent.get();
    for (unsigned i = 0; i != 100; ++i) parent->next = mk<node>(nullptr, i);
    check(h.remembered_size() == 1, "repeated writes deduplicate the source slot");
    collect_young();
    check(parent.get() == old_parent && parent->next->data == 99, "minor GC retains current edge and leaves old fixed");
    check(parent->next.is_young(), "minor GC retains the young tag");
    parent->next = nullptr;
    h.collect_young();
    check(h.young().used() == 1, "overwritten targets are reclaimed");
    parent->next.unsafe_assign(mk<node>(nullptr, 123u));
    h.remember(parent->next);
    h.collect_young();
    check(parent->next->data == 123, "unsafe assignment plus explicit registration retains the edge");
    h.collect();
    check(!parent->next.is_young() && h.remembered_size() == 0 && h.young().used() == 1, "full GC clears young and the remembered set");

    root array = mk<array_node>();
    root counted = mk<counted_node>();
    h.collect();
    counted_node::traces = 0;
    auto const old_array = array.get();
    array->edges = {mk<node>(nullptr, 201u), mk<node>(nullptr, 202u), nullptr};
    counted->next = mk<node>(nullptr, 203u);
    h.collect_young();
    check(counted_node::traces == 0, "minor collection does not trace old objects");
    check(array.get() == old_array && array->edges[0]->data == 201 && array->edges[1]->data == 202,
          "array assignment registers each source slot");
    assign(array->edges, std::array<ptr<node>, 3>{nullptr, mk<node>(nullptr, 204u), mk<node>(nullptr, 205u)});
    h.collect_young(true);
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
    h.collect_young();
    check(parent->next->data == 302 && parent->next->next == parent.get(),
          "minor collection reads the latest aliased write and preserves an old-young cycle");

    root changing = mk<changing_node>();
    h.collect();
    changing->edge = mk<node>(nullptr, 401u);
    check(h.remembered_size() == 1, "active union pointer is remembered");
    std::destroy_at(&changing->edge);
    std::construct_at(&changing->bits, 0xffffffffu);
    changing->kind = 2;
    check(h.remembered_size() == 0, "ending pointer lifetime forgets the source slot");
    h.collect_young();
    check(changing->bits == 0xffffffffu && h.young().used() == 1, "scalar replacement is never interpreted as a pointer");
    std::construct_at(&changing->number, mk<std::uint64_t>(501u));
    changing->kind = 1;
    h.collect_young();
    check(*changing->number == 501u, "reused source slot uses the new target's tracer");
    h.collect();
    check(!changing->number.is_young() && *changing->number == 501u, "full collection traces the current union alternative");
  }
  std::puts("generational checks passed");
}
