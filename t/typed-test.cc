// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstring>
#include <cstdint>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <type_traits>
#include <variant>
#include <utility>
#include <vector>

#include "../etc/page-size.h"

import jam.simd;

auto const page_bytes = static_cast<std::uint64_t>(system_page_size());

// Encoded pointers can be manipulated at compile time without a heap or TLS.
static_assert([] consteval {
  using pointer = jam::ptr<std::uint64_t>;
  std::array<pointer, 2> source{pointer{0x80000001u}, pointer{2u}};
  std::array<pointer, 2> target{};
  jam::assign(target, source);
  pointer moved{std::move(target[0])};
  swap(moved, target[1]);
  return !target[0] && moved.get() == 2u && target[1].get() == 0x80000001u
      && source[0].get() == 0x80000001u && source[1].get() == 2u;
}());

struct payload {
  std::uint64_t bits;
  std::uint32_t small;
};


struct node {
  jam::ptr<node> next;
  jam::ptr<payload> data;
  std::uint64_t value;
  inline static std::atomic<unsigned> visits{0};

  constexpr auto trace(jam::visitor auto & visit) const {
    visits.fetch_add(1, std::memory_order_relaxed);
    return visit(next, data);
  }
};

template<class H, class R>
concept accepts_root = requires(H & heap, R value) { heap.root(value); };
template<class V, class... Rs>
concept accepts_field = requires(V & visit, Rs const &... values) { visit(values...); };


struct alignas(64) aligned_branch {
  jam::ptr<std::uint32_t> data;
  inline static jam::heap * expected_heap = nullptr;

  template<class Visitor>
  void trace(Visitor & visit) const noexcept {
    if (jam::heap::current() != expected_heap) std::abort();
    visit(data);
  }
};

struct fork_node {
  jam::ptr<aligned_branch> left, right;

  template<class Visitor>
  void trace(Visitor & visit) const noexcept { visit(left, right); }
};

struct untraced { std::uint64_t bits; };
struct invalid_trace {
  void trace() const noexcept {}
};
struct trace_result {
  template<class V>
  constexpr auto trace(V &) const noexcept { return 7u; }
};
struct self_link {
  jam::ptr<self_link> next;
  constexpr auto trace(jam::visitor auto & visit) const noexcept { return visit(next); }
};
struct manifest_node {
  jam::ptr<manifest_node> left, right;
  std::uint64_t data;
  static constexpr auto manifest = jam::make_manifest<manifest_node>(
      &manifest_node::left, &manifest_node::right);
};
struct bad_manifest {
  static constexpr auto manifest = 7;
};
struct bad_manifest_member {
  invalid_trace value;
  static constexpr auto manifest = jam::make_manifest<bad_manifest_member>(&bad_manifest_member::value);
};
static_assert(jam::traceable<manifest_node>);
static_assert(!jam::traceable<bad_manifest> && !jam::traceable<bad_manifest_member>);
static_assert(jam::traceable<self_link>);
static_assert(jam::traceable<trace_result>);
static_assert([] {
  std::nullptr_t visit{};
  return jam::tracer<trace_result>::trace(visit, trace_result{}) == 7u;
}());
template<class T>
concept accepts_ptr = requires { typename jam::ptr<T>; };
static_assert(jam::visitor<jam::heap::visitor>);
static_assert(!jam::visitor<int>);

struct incomplete;
static_assert(sizeof(jam::ptr<incomplete>) == 4);
static_assert(sizeof(jam::heap::ptr<incomplete>) == 4);
static_assert(accepts_ptr<untraced>);
static_assert(accepts_ptr<invalid_trace>);
template<class T>
concept allocatable = requires(jam::heap & heap) { heap.template mk<T>(); };
static_assert(allocatable<untraced> && !allocatable<invalid_trace>);
static_assert(accepts_root<jam::heap, jam::ptr<untraced>>);
static_assert(!accepts_root<jam::heap, jam::ptr<invalid_trace>>);
static_assert(accepts_field<jam::heap::visitor, jam::ptr<untraced>>);
static_assert(jam::traceable<node>);

using word = std::uint32_t;
using word_ptr = jam::ptr<word>;
static_assert(word_ptr{}.get() == 0 && !word_ptr{0});
static_assert(jam::ptr<word>{}.get() == 0);
static_assert(accepts_field<jam::heap::visitor>);
static_assert(accepts_field<jam::heap::visitor, word_ptr, word, word_ptr>);
static_assert(!accepts_field<jam::heap::visitor, word_ptr, invalid_trace>);
static_assert(jam::traceable<bool> && jam::traceable<double>);
static_assert(jam::traceable<std::nullptr_t> && jam::traceable<void *>);
static_assert(jam::traceable<std::tuple<>>);
static_assert(jam::traceable<std::array<word_ptr, 0>>);
static_assert(jam::traceable<std::tuple<word, word_ptr, std::variant<word, word_ptr>>>);
static_assert(!jam::traceable<std::tuple<invalid_trace>>);
static_assert([] {
  // Leaf-only traversal needs no runtime visitor operations.
  std::nullptr_t visit{};
  using value = std::tuple<trace_result, std::array<int, 2>, std::variant<int, double>>;
  jam::tracer<value>::trace(visit, value{{}, {2, 3}, 4});
  return true;
}());

struct embedded {
  jam::ptr<payload> data;
  std::uint32_t scalar;
  constexpr auto trace(jam::visitor auto & visit) const noexcept { return visit(data, scalar); }
};

// Standard libraries need not make std::variant standard-layout. Keep managed
// records portable and test the standard variant adapter independently below.
struct tagged {
  using edge = jam::ptr<payload>;
  union { std::uint32_t scalar; edge pointer; embedded nested; };
  unsigned kind;
  constexpr tagged(std::uint32_t value) noexcept : scalar(value), kind(0) {}
  constexpr tagged(edge value) noexcept : pointer(value), kind(1) {}
  constexpr tagged(embedded value) noexcept : nested(value), kind(2) {}
  constexpr tagged(tagged const & other) noexcept : kind(other.kind) {
    if (kind == 0) std::construct_at(&scalar, other.scalar);
    else if (kind == 1) std::construct_at(&pointer, other.pointer);
    else std::construct_at(&nested, other.nested);
  }
  constexpr ~tagged() noexcept {
    if (kind == 1) std::destroy_at(&pointer);
    else if (kind == 2) std::destroy_at(&nested);
  }
  constexpr tagged & operator=(tagged const & other) noexcept {
    if (this != &other) {
      std::destroy_at(this);
      std::construct_at(this, other);
    }
    return *this;
  }
  constexpr auto trace(jam::visitor auto & visit) const noexcept {
    if (kind == 1) visit(pointer);
    else if (kind == 2) visit(nested);
  }
};
static_assert(std::is_standard_layout_v<tagged> && std::is_nothrow_copy_constructible_v<tagged>);

struct composite {
  using edge = jam::ptr<payload>;
  std::array<tagged, 3> entries;
  std::uint64_t scalar;

  constexpr auto trace(jam::visitor auto & visit) const noexcept {
    visit(std::tie(entries, scalar)); // A view must preserve original field addresses.
    visit();
    visit(std::tuple<>{});
    visit(std::array<edge, 0>{});
  }
};

void check(bool ok, char const * message) noexcept {
  if (ok) return;
  std::fprintf(stderr, "%s\n", message);
  std::abort();
}

void mixed_graph() noexcept {
  using H = jam::heap;
  using N = node;
  using R = typename H::template ptr<N>;
  static_assert(std::is_same_v<R, jam::ptr<N>>);
  static_assert(sizeof(R) == sizeof(std::uint32_t));
  static_assert(std::is_standard_layout_v<R> && !std::is_trivially_copyable_v<R>);
  H heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  jam::heap_scope binding{heap};
  static_cast<void>(heap.allocate(23));
  auto const data = heap.template mk<payload>(payload{1, 7});
  auto const a = heap.template mk<N>(N{{}, data, 101});
  static_cast<void>(heap.allocate(5));
  auto const b = heap.template mk<N>(N{a, data, 202});
  heap.store(a, N{b, data, 101});
  auto root = heap.root(a);
  static_assert(std::is_same_v<decltype(root.get()), R>);
  static_assert(std::is_same_v<decltype(root), jam::root<N>>);
  auto copy = root;
  auto moved = std::move(copy);
  check(!copy.get(), "a moved typed root is null");
  std::vector<decltype(root)> roots;
  for (unsigned i = 0; i != 17; ++i) roots.push_back(root);
  auto null_root = heap.root(R{});

  for (unsigned round = 0; round != 4; ++round) {
    N::visits.store(0, std::memory_order_relaxed);
    heap.collect_major();
    check(N::visits.load(std::memory_order_relaxed) == 2,
          "cycles and duplicate roots trace each typed record exactly once");
    auto const first = heap.load(root.get());
    auto const second = heap.load(first.next);
    auto const leaf = heap.load(first.data);
    check(first.value == 101 && second.value == 202, "mixed graph payloads survive");
    check(second.next == root.get(), "typed back edge follows the forwarded root");
    check(first.data == second.data, "different records retain their shared leaf");
    check(leaf.bits == 1 && leaf.small == 7, "leaf integers are never forwarded as pointers");
    check(moved.get() == root.get() && !null_root.get(), "typed moved and null roots survive");
    for (auto const & entry : roots)
      check(entry.get() == root.get(), "vector relocation preserves typed root dispatch");
    check(heap.used() == 7, "unreachable cells are reclaimed");
    static_cast<void>(heap.allocate(heap.page_words()));
  }
  roots.clear();
  root = {};
  moved = {};
  heap.collect_major();
  check(heap.used() == 1, "dropping the last typed roots reclaims the graph");
}

void parallel_typed_discovery() noexcept {
  jam::heap heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  jam::heap_scope binding{heap};
  aligned_branch::expected_heap = &heap;
  static_cast<void>(heap.allocate(31));
  auto const data = heap.mk<std::uint32_t>(77u);
  auto const left = heap.mk<aligned_branch>(aligned_branch{data});
  auto const right = heap.mk<aligned_branch>(aligned_branch{data});
  auto root = heap.root(heap.mk<fork_node>(fork_node{left, right}));
  static_assert(std::is_same_v<decltype(root), jam::root<fork_node>>);
  for (unsigned round = 0; round != 3; ++round) {
    heap.collect_major();
    auto const fork = heap.load(root.get());
    check((fork.left.get() * 8) % 64 == 0 && (fork.right.get() * 8) % 64 == 0,
          "target types preserve their allocation alignment during compaction");
    auto const a = heap.load(fork.left);
    auto const b = heap.load(fork.right);
    check(a.data == b.data && heap.load(a.data) == 77,
          "parallel heterogeneous traversal retains a shared scalar leaf");
  }
}

void composite_graph() noexcept {
  using C = composite;
  using E = typename C::edge;
  using A = std::array<E, 2>;
  using V = tagged;
  jam::heap heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 3}};
  jam::heap_scope binding{heap};
  static_cast<void>(heap.allocate(17));
  auto const first = heap.template mk<payload>(payload{71, 11});
  auto const second = heap.template mk<payload>(payload{83, 13});
  using Part = tagged;
  auto root = heap.root(heap.template mk<C>(C{{Part{first}, Part{std::uint32_t{1}}, Part{embedded{second, 1}}}, 1}));
  auto array_root = heap.root(heap.template mk<A>(A{first, {}}));
  auto variant_root = heap.root(heap.template mk<V>(V{second}));
  for (unsigned round = 0; round != 3; ++round) {
    heap.collect_major();
    auto value = heap.load(root.get());
    auto const a = value.entries[0].pointer;
    auto const part = value.entries[2].nested;
    auto const b = part.data;
    check(part.scalar == 1, "embedded hooks share the outer allocation without reclaiming it");
    check(heap.load(a).bits == 71 && heap.load(b).bits == 83,
          "nested array, tagged records and tuple views follow managed targets");
    check(value.entries[1].scalar == 1 && value.scalar == 1,
          "scalar alternatives and tuple elements remain data");
    auto const pointers = heap.load(array_root.get());
    check(pointers[0] == a && !pointers[1], "array roots forward references and preserve nulls");
    check(heap.load(variant_root.get()).pointer == b,
          "tagged roots visit the active reference alternative");
    static_cast<void>(heap.allocate(heap.page_words()));
  }
  auto value = heap.load(root.get());
  value.entries[0] = std::uint32_t{1}; // The old pointer slot is now scalar data.
  value.entries[2] = E{};
  heap.store(root.get(), value);
  array_root = {};
  variant_root = {};
  heap.collect_major();
  value = heap.load(root.get());
  check(value.entries[0].scalar == 1 && !value.entries[2].pointer,
        "changing a tagged alternative clears the old pointer declaration");
  check(heap.used() == 1 + (sizeof(C) + 7) / 8, "inactive alternatives retain no targets");
  auto const replacement = heap.template mk<payload>(payload{97, 17});
  value.entries[0] = replacement;
  heap.store(root.get(), value);
  heap.collect_major();
  check(heap.load(heap.load(root.get()).entries[0].pointer).bits == 97,
        "a scalar alternative can become a managed edge on the next collection");
}

struct adapter_visitor {
  using heap_type = jam::heap;
  jam::ptr<payload> const * expected;
  unsigned calls = 0;
  void operator()(jam::ptr<payload> const & field) noexcept {
    check(&field == expected, "adapters preserve the active pointer field address");
    ++calls;
  }
  template<class... Ts>
  void operator()(Ts const &... values) noexcept {
    (jam::tracer<Ts>::trace(*this, values), ...);
  }
};

void variant_adapters() noexcept {
  using edge = jam::ptr<payload>;
  using part = std::variant<std::uint32_t, edge, embedded>;
  std::array<part, 3> values{std::uint32_t{1}, edge{7}, embedded{edge{9}, 1}};
  adapter_visitor visitor{&std::get<edge>(values[1])};
  jam::tracer<part>::trace(visitor, values[0]);
  check(visitor.calls == 0, "scalar variant alternatives contain no managed edges");
  jam::tracer<part>::trace(visitor, values[1]);
  check(visitor.calls == 1, "variant adapter visits its active pointer alternative");
  visitor.expected = &std::get<embedded>(values[2]).data;
  jam::tracer<part>::trace(visitor, values[2]);
  check(visitor.calls == 2, "variant adapter visits embedded records by reference");
  values[1] = std::uint32_t{1};
  values[2] = edge{};
  visitor.expected = &std::get<edge>(values[2]);
  visitor(std::tie(values));
  check(visitor.calls == 3, "nested tuple/array/variant adapters follow changed alternatives and nulls");
}


struct walking_node {
  std::array<jam::ptr<walking_node>, 3> edges;
  unsigned value;
  inline static std::atomic<unsigned> calls{0};
  inline static std::uintptr_t heap_begin = 0, heap_end = 0;
  static constexpr void trace(jam::visitor auto & visit, jam::ptr<walking_node> const & at) noexcept {
    for (auto const * p = visit.claim_target(at); p; p = visit.claim(p->edges[0])) {
      auto const address = reinterpret_cast<std::uintptr_t>(p);
      check(address >= heap_begin && address + sizeof(*p) <= heap_end,
            "trace hook must read the claimed allocation in place");
      calls.fetch_add(1, std::memory_order_relaxed);
      visit(p->edges[1], p->edges[2]);
      visit.poll();
    }
  }
};
static_assert(jam::traceable<walking_node>);

void deep_cooperative_graph() noexcept {
  constexpr unsigned count = 2048;
  for (auto workers : {1u, 4u}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{32}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{32}, .reserve = jam::units::pages{2}}, .workers = workers}};
    jam::heap_scope binding{heap};
    std::vector<jam::ptr<walking_node>> nodes;
    for (unsigned i = 0; i != count * 3; ++i) {
      static_cast<void>(heap.allocate(1)); // Every pointer must change on compaction.
      nodes.push_back(heap.mk<walking_node>(walking_node{{}, i}));
    }
    for (unsigned i = 0; i != count; ++i)
      heap.store(nodes[i], walking_node{{nodes[(i + 1) % count],
          nodes[count + i], nodes[2 * count + i]}, i});
    auto root = heap.root(nodes[0]);
    for (unsigned round = 0; round != 5; ++round) {
      auto const & generation = root.get().is_young() ? heap.young() : heap.old();
      walking_node::heap_begin = reinterpret_cast<std::uintptr_t>(generation.data());
      walking_node::heap_end = walking_node::heap_begin + generation.used() * 8;
      walking_node::calls.store(0);
      heap.collect_major();
      check(walking_node::calls.load() == count * 3, "cooperative traversal visits each record once");
      auto p = root.get();
      for (unsigned i = 0; i != count; ++i) {
        auto const value = heap.load(p);
        check(value.value == i, "cooperative walking preserves its cycle");
        check(heap.load(value.edges[1]).value == count + i,
              "queued branch survives cooperative walking");
        check(heap.load(value.edges[2]).value == 2 * count + i,
              "last array field is declared before compaction");
        p = value.edges[0];
      }
      check(p == root.get(), "cycle closes after forwarding");
    }
  }
}

extern "C" void check_work_pushing() noexcept;

template<unsigned Depth, unsigned Prefetch>
struct tree_node {
  jam::ptr<tree_node> left, right;
  std::uint64_t value;
  inline static std::atomic<unsigned> * seen = nullptr;

  static constexpr void trace(jam::visitor auto & visit, jam::ptr<tree_node> at) noexcept {
    std::array<jam::ptr<tree_node>, Depth> queue{};
    unsigned head = 0, size = 0;
    auto pushpop = [&](jam::ptr<tree_node> value) noexcept -> jam::ptr<tree_node> {
      if (!value) return {};
      if constexpr (Prefetch == 1) value.prefetch_marks();
      if constexpr (Prefetch == 2) value.prefetch();
      if (size < Depth) {
        queue[(head + size++) % Depth] = value;
        return {};
      }
      auto const result = std::exchange(queue[head], value);
      head = (head + 1) % Depth;
      return result;
    };
    if constexpr (Prefetch == 1) at.prefetch_marks();
    if constexpr (Prefetch == 2) at.prefetch();
    for (;;) {
      if (!at) {
        if (!size) break;
        at = queue[head];
        head = (head + 1) % Depth;
        --size;
      }
      auto const * p = visit.claim_target(std::exchange(at, {}));
      if (!p) continue;
      check(seen[p->value].fetch_add(1, std::memory_order_relaxed) == 0,
            "buffered traversal claims shared targets once");
      visit.pointer(p->left);
      at = pushpop(p->left);
      if (at) visit(p->right);
      else {
        visit.pointer(p->right);
        at = pushpop(p->right);
      }
      visit.poll();
    }
  }
};

template<unsigned Depth, unsigned Prefetch>
void cooperative_tree() noexcept {
  using tree_node = ::tree_node<Depth, Prefetch>;
  constexpr unsigned count = 8191;
  for (auto workers : {1u, 4u}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{2}}, .workers = workers}};
    jam::heap_scope binding{heap};
    std::vector<jam::ptr<tree_node>> nodes(count + 1);
    for (unsigned i = 1; i <= count; ++i) {
      static_cast<void>(heap.allocate(1));
      nodes[i] = heap.mk<tree_node>(nullptr, nullptr, i);
    }
    for (unsigned i = 1; i <= count / 2; ++i)
      heap.store(nodes[i], tree_node{nodes[2 * i], nodes[2 * i + 1], i});
    heap.store(nodes[count], tree_node{nodes[2], nodes[1], count}); // Sharing and a cycle.
    auto root = heap.root(nodes[1]);
    auto duplicate = root;
    std::vector<std::atomic<unsigned>> seen(count + 1);
    tree_node::seen = seen.data();
    for (unsigned round = 0; round != 3; ++round) {
      for (auto & n : seen) n.store(0);
      heap.collect_major();
      for (unsigned i = 1; i <= count; ++i) {
        check(seen[i].load() == 1, "all enqueued subtrees are processed before compaction");
        auto const node = heap.load(jam::ptr<tree_node>{1 + 2 * (i - 1)});
        check(node.value == i, "buffered tree preserves stable record order");
        if (i <= count / 2)
          check(heap.load(node.left).value == 2 * i && heap.load(node.right).value == 2 * i + 1,
                "both enqueued and walked edges are forwarded");
        else if (i == count)
          check(node.right == root.get() && heap.load(node.left).value == 2,
                "shared and cyclic edges are declared even when their claims lose");
        else check(!node.left && !node.right, "tree leaves retain null fields");
      }
      check(heap.used() == 1 + 2 * count && root.get() == duplicate.get(),
            "typed static hooks claim complete records and reclaim garbage gaps");
    }
  }
}

struct patterned_node {
  jam::ptr<payload> a;
  std::uint32_t x, y;
  jam::ptr<payload> b;
  std::uint32_t z;
  jam::ptr<patterned_node> next;
};
static_assert(sizeof(patterned_node) == 24);

void cooperative_claims() noexcept {
  for (bool pattern : {false, true}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
    jam::heap_scope binding{heap};
    static_cast<void>(heap.allocate(30));
    // The six-slot pattern straddles a metadata word on the first collection.
    auto const a = heap.mk<patterned_node>();
    static_cast<void>(heap.allocate(7));
    auto const b = heap.mk<patterned_node>();
    auto const data = heap.mk<payload>(payload{71, 83});
    heap.store(a, patterned_node{data, 1, 1, data, 1, b});
    heap.store(b, patterned_node{data, 1, 1, {}, 1, a});
    auto root = heap.root(a.get());
    for (unsigned round = 0; round != 3; ++round) {
      heap.collect_major([&](jam::heap::visitor & visit, jam::heap::offset at) noexcept {
        auto const * current = visit.claim_target(jam::ptr<patterned_node>{at});
        unsigned count = 0;
        while (current) {
          ++count;
          check(!visit.claim_target(jam::ptr<patterned_node>{at}), "duplicate claim loses");
          check(!visit.claim_target(jam::ptr<payload>{}), "null claim loses");
          // Failed claims must preserve the current source context.
          if (pattern) {
            visit.pointers(0);
            if (round == 1) {
              visit.pointers(1);
              visit.pointers(0b101, 3);
            } else visit.pointers(0b101001);
          } else {
            visit.pointer(current->a);
            visit.pointer(current->b);
            visit.pointer(current->next);
          }
          auto const next = current->next;
          auto const second = current->b;
          if (auto const * p = visit.claim_target(current->a))
            check(p->bits == 71 && p->small == 83, "claim returns the live payload address");
          static_cast<void>(visit.claim_target(second));
          current = visit.claim_target(next);
        }
        check(count == 2, "cooperative walking terminates at the back edge");
      });
      auto const first = heap.load(jam::ptr<patterned_node>{root.get()});
      auto const second = heap.load(first.next);
      check(first.x == 1 && first.y == 1 && first.z == 1, "zero mask bits remain scalar data");
      check(first.a == first.b && first.a == second.a && !second.b,
            "pattern forwards shared pointers and preserves null slots");
      check(second.next.get() == root.get() && heap.load(first.a).bits == 71,
            "separate slot declaration and target claim preserve the graph");
    }
  }

  jam::heap heap{{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  jam::heap_scope binding{heap};
  static_cast<void>(heap.allocate(11));
  auto const tail = heap.mk<self_link>();
  auto root = heap.root(heap.mk<self_link>(tail).get());
  for (unsigned round = 0; round != 3; ++round) {
    bool const cycle = round == 1;
    auto const head_at = jam::ptr<self_link>{root.get()};
    heap.store(heap.load(head_at).next, self_link{cycle ? head_at : nullptr});
    heap.collect_major([](jam::heap::visitor & visit, jam::heap::offset at) noexcept {
      unsigned count = 0;
      for (auto const * p = visit.claim_target(jam::ptr<self_link>{at}); p;
           p = visit.claim(p->next)) ++count;
      check(count == 2, "combined claim marks each source slot and walks in place");
    });
    auto const head = heap.load(jam::ptr<self_link>{root.get()});
    check(head.next && heap.load(head.next).next.get() == (cycle ? root.get() : 0),
          "combined claim forwards null and already-claimed back edges");
  }
}

void manifest_graph() noexcept {
  for (auto workers : {1u, 4u}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{1024}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{1024}, .reserve = jam::units::pages{2}}, .workers = workers}};
    jam::heap_scope binding{heap};
    constexpr unsigned length = 100000;
    jam::ptr<manifest_node> at;
    for (unsigned i = 0; i != length; ++i) {
      static_cast<void>(heap.mk<manifest_node>());
      at = heap.mk<manifest_node>(at, at, i); // Shared child; one local edge.
    }
    auto root = heap.root(at);
    for (unsigned round = 0; round != 3; ++round) {
      heap.collect_major();
      auto p = root.get();
      for (unsigned i = length; i; --i) {
        auto const value = heap.load(p);
        check(value.data == i - 1 && value.left == value.right,
              "manifest walk preserves fields and shared children");
        p = value.right;
      }
      check(!p && heap.used() == 1 + length * 2,
            "manifest walk retains the complete graph without garbage");
    }
    heap.store(root.get(), manifest_node{root.get(), root.get(), 17});
    heap.collect_major();
    auto const value = heap.load(root.get());
    check(heap.used() == 3 && value.left == root.get() && value.right == root.get(),
          "manifest cycles stop at an earlier claim and forward both slots");
    auto build = [&](this auto const & self, unsigned depth) -> jam::ptr<manifest_node> {
      if (!depth) return {};
      auto const left = self(depth - 1), right = self(depth - 1);
      return heap.mk<manifest_node>(left, right, depth);
    };
    auto tree = heap.root(build(12));
    heap.collect_major();
    std::vector<jam::ptr<manifest_node>> pending{tree.get()};
    unsigned count = 0;
    while (!pending.empty()) {
      auto const current = heap.load(pending.back());
      pending.pop_back();
      ++count;
      if (current.left) pending.push_back(current.left);
      if (current.right) pending.push_back(current.right);
    }
    check(count == 4095 && heap.used() == 3 + 2 * count,
          "generated traversal retains both distinct branches of a binary tree");
  }
}

struct manifest_parts {
  std::array<jam::ptr<payload>, 2> data;
  static constexpr auto manifest = jam::make_manifest<manifest_parts>(&manifest_parts::data);
};
struct manifest_override {
  jam::ptr<payload> data;
  static constexpr auto manifest = jam::make_manifest<manifest_override>();
  inline static std::atomic<unsigned> calls = 0;
  void trace(jam::heap::visitor & visit) const noexcept {
    ++calls;
    visit(data);
  }
};
struct alignas(64) manifest_record {
  std::uint64_t scalar = 1;
  manifest_parts parts;
  std::array<jam::ptr<manifest_record>, 2> links;
  tagged dynamic{0u};
  manifest_override custom;
  std::array<unsigned char, 280> padding{};
  jam::ptr<payload> tail;
  // Deliberately visit the high mask window before the low one.
  static constexpr auto manifest = jam::make_manifest<manifest_record>(
      &manifest_record::tail, &manifest_record::parts, &manifest_record::links,
      &manifest_record::dynamic, &manifest_record::custom);
};

void nested_manifest_graph() noexcept {
  for (auto workers : {1u, 4u}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{32}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{32}, .reserve = jam::units::pages{2}}, .workers = workers}};
    jam::heap_scope binding{heap};
    static_cast<void>(heap.allocate(13));
    auto const a = heap.mk<payload>(71u, 83u);
    auto const b = heap.mk<payload>(97u, 101u);
    auto const c = heap.mk<payload>(107u, 109u);
    auto const first = heap.mk<manifest_record>(manifest_record{
        .parts = {{a, a}}, .links = {}, .dynamic = tagged(b), .custom = {c}, .tail = b});
    static_cast<void>(heap.allocate(7));
    auto const second = heap.mk<manifest_record>(manifest_record{
        .parts = {{b, {}}}, .links = {first, first}, .dynamic = tagged(1u),
        .custom = {c}, .tail = a});
    heap.address(first)->links = {second, second};
    auto root = heap.root(first);
    for (unsigned round = 0; round != 3; ++round) {
      manifest_override::calls = 0;
      heap.collect_major();
      auto const x = heap.load(root.get());
      auto const y = heap.load(x.links[1]);
      check(x.scalar == 1 && y.scalar == 1 && y.dynamic.scalar == 1,
            "unlisted manifest fields and inactive alternatives remain data");
      check(x.parts.data[0] == x.parts.data[1] && x.parts.data[0] == y.tail
            && x.tail == y.parts.data[0] && x.tail == x.dynamic.pointer,
            "nested manifests and dynamic hooks forward original subobject slots");
      check(x.links[0] == x.links[1] && y.links[0] == root.get() && !y.parts.data[1],
            "array edges preserve sharing, cycles and nulls");
      check(heap.load(x.parts.data[0]).bits == 71 && heap.load(x.tail).bits == 97
            && heap.load(x.custom.data).bits == 107 && manifest_override::calls == 2,
            "explicit hooks override manifests and run once with the heap visitor");
      check(reinterpret_cast<std::uintptr_t>(heap.address(root.get())) % 64 == 0
            && reinterpret_cast<std::uintptr_t>(heap.address(x.links[0])) % 64 == 0,
            "generated claims preserve the alignment of spanning records");
    }
  }
}

template<class T>
struct native_record {
  jam::ptr<T> next;
  T value;
  static constexpr auto manifest = jam::make_manifest<native_record>(
      &native_record::next, &native_record::value);
};

template<class T>
void native_storage() {
  static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
  static_assert(jam::traceable<T>);
  // Exercise representation transport without executing an unsupported ISA.
  std::array<std::uint32_t, sizeof(T) / sizeof(std::uint32_t)> lanes;
  for (unsigned i = 0; i != lanes.size(); ++i) lanes[i] = 0x3f800000u + i * 17;
  T const expected = std::bit_cast<T>(lanes);
  for (unsigned workers : {1u, 4u}) {
    jam::heap heap{{.old = {.capacity = jam::units::pages{128}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{128}, .reserve = jam::units::pages{2}}, .workers = workers}};
    jam::heap_scope binding{heap};
    for (unsigned i = 0; i != 97; ++i) static_cast<void>(heap.mk<std::uint64_t>(i));
    auto direct = heap.root(heap.mk<T>(expected));
    auto outer = heap.root(heap.mk<native_record<T>>(direct.get(), expected));
    auto const before = direct.get().get();
    for (unsigned round = 0; round != 3; ++round) {
      heap.collect_major();
      auto const * p = heap.address(direct.get());
      auto const * q = heap.address(outer.get());
      check(direct.get().get() < before, "native payload actually moves during collection");
      check(reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0
            && reinterpret_cast<std::uintptr_t>(&q->value) % alignof(T) == 0,
            "native register and pack alignment survives compaction");
      check(std::memcmp(p, &expected, sizeof(T)) == 0
            && std::memcmp(&q->value, &expected, sizeof(T)) == 0,
            "native numeric lanes remain data in direct and manifest records");
      check(q->next == direct.get(), "pointer adjacent to native payload is forwarded");
      auto const copy = heap.load(direct.get());
      check(std::memcmp(&copy, &expected, sizeof(T)) == 0, "native payload can be loaded by value");
    }
  }
}

void native_heap_storage() {
  using I = native::simd<std::uint32_t, 4>;
  using F = native::simd<float, 4>;
  native_storage<I>();
  native_storage<F>();
  native_storage<native::wide<I, 4>>();
  native_storage<native::wide<F, 4>>();
#if defined(__x86_64__) || defined(_M_X64)
  using I8 = native::simd<std::uint32_t, 8, native::avx2>;
  using I16 = native::simd<std::uint32_t, 16, native::avx512>;
  native_storage<I8>();
  native_storage<I16>();
  native_storage<native::wide<I8, 2>>();
  native_storage<native::wide<I16, 4>>();
  native_storage<native::wide<I16, 8>>(); // Spans collector blocks.
#endif
}

int main() {
  native_heap_storage();
  manifest_graph();
  nested_manifest_graph();
  jam::ptr<incomplete>{}.prefetch();
  jam::ptr<incomplete>{}.prefetch_marks(); // Null needs no current heap or complete T.
  cooperative_tree<1, true>();
  cooperative_tree<32, false>();
  cooperative_tree<32, true>();
  cooperative_tree<32, 2>();
  cooperative_claims();
  check_work_pushing();
  deep_cooperative_graph();
  mixed_graph();
  parallel_typed_discovery();
  composite_graph();
  variant_adapters();
  std::puts("typed graph checks passed");
}
