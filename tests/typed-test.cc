// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <variant>
#include <utility>
#include <vector>

import jam;

struct payload {
  std::uint64_t bits;
  std::uint32_t small;
};


template<jam::oo O>
struct node {
  jam::ptr<node, O> next;
  jam::ptr<payload, O> data;
  std::uint64_t value;
  inline static std::atomic<unsigned> visits{0};

  template<jam::visitor<O> V>
  constexpr auto trace(V & visit) const noexcept {
    visits.fetch_add(1, std::memory_order_relaxed);
    return visit(next, data);
  }
};

template<class H, class R>
concept accepts_root = requires(H & heap, R value) { heap.root(value); };
template<class V, class... Rs>
concept accepts_field = requires(V & visit, Rs const &... values) { visit(values...); };

static_assert(!accepts_root<jam::heap<std::uint32_t>, jam::ptr<payload, std::uint64_t>>);
static_assert(!accepts_field<jam::heap<std::uint32_t>::visitor, jam::ptr<payload, std::uint64_t>>);
static_assert(!accepts_field<jam::heap<std::uint64_t>::visitor, jam::ptr<payload, std::uint32_t>>);

struct alignas(64) aligned_branch {
  jam::ptr<std::uint32_t, std::uint32_t> data;
  inline static std::barrier<> * rendezvous = nullptr;

  template<class Visitor>
  void trace(Visitor & visit) const noexcept {
    rendezvous->arrive_and_wait(); // Both children must be discovered and run concurrently.
    visit(data);
  }
};

struct fork_node {
  jam::ptr<aligned_branch, std::uint32_t> left, right;

  template<class Visitor>
  void trace(Visitor & visit) const noexcept { visit(left, right); }
};

struct untraced { std::uint64_t bits; };
struct throwing_trace {
  template<class V> void trace(V &) const {}
};
struct trace_result {
  template<class V>
  constexpr auto trace(V &) const noexcept { return 7u; }
};
struct self_link {
  jam::ptr<self_link, std::uint32_t> next;
  template<jam::visitor<std::uint32_t> V>
  constexpr auto trace(V & visit) const noexcept { return visit(next); }
};
static_assert(jam::traceable<self_link, std::uint32_t>);
static_assert(jam::traceable<trace_result, std::uint32_t>);
static_assert([] {
  std::nullptr_t visit{};
  return jam::tracer<trace_result>::trace(visit, trace_result{}) == 7u;
}());
template<class T, class O>
concept accepts_ptr = requires { typename jam::ptr<T, O>; };
static_assert(jam::oo<std::uint32_t> && jam::oo<std::uint64_t>);
static_assert(!jam::oo<bool> && !jam::oo<std::int32_t> && !jam::oo<std::uint16_t>);
static_assert(!jam::oo<std::uint32_t const>);
static_assert(jam::visitor<jam::heap<>::visitor, std::uint32_t>);
static_assert(jam::visitor<jam::heap<std::uint64_t, true>::visitor, std::uint64_t>);
static_assert(!jam::visitor<jam::heap<>::visitor, std::uint64_t>);
static_assert(!jam::visitor<int, std::uint32_t>);

struct incomplete;
static_assert(sizeof(jam::ptr<incomplete, std::uint32_t>) == 4);
static_assert(sizeof(jam::heap<std::uint64_t>::ptr<incomplete>) == 8);
static_assert(accepts_ptr<untraced, std::uint32_t>);
static_assert(accepts_ptr<throwing_trace, std::uint32_t>);
template<class T>
concept allocatable = requires(jam::heap<> & heap) { heap.template make<T>(); };
static_assert(allocatable<untraced> && !allocatable<throwing_trace>);
static_assert(accepts_root<jam::heap<>, jam::ptr<untraced, std::uint32_t>>);
static_assert(!accepts_root<jam::heap<>, jam::ptr<throwing_trace, std::uint32_t>>);
static_assert(accepts_field<jam::heap<>::visitor, jam::ptr<untraced, std::uint32_t>>);
static_assert(jam::traceable<node<std::uint32_t>, std::uint32_t>);
static_assert(!jam::traceable<node<std::uint32_t>, std::uint64_t>);
static_assert(!accepts_root<jam::heap<std::uint64_t>, jam::ptr<node<std::uint32_t>, std::uint64_t>>);

using word = std::uint32_t;
using word_ptr = jam::ptr<word, word>;
static_assert(word_ptr{}.get() == 0 && !word_ptr{0});
static_assert(jam::ptr<word, std::uint64_t>{}.get() == 0);
static_assert(accepts_field<jam::heap<>::visitor>);
static_assert(accepts_field<jam::heap<>::visitor, word_ptr, word, word_ptr>);
static_assert(!accepts_field<jam::heap<>::visitor, word_ptr, jam::ptr<word, std::uint64_t>>);
static_assert(!accepts_field<jam::heap<>::visitor, word_ptr, throwing_trace>);
static_assert(jam::traceable<bool, word> && jam::traceable<double, word>);
static_assert(jam::traceable<std::nullptr_t, word> && jam::traceable<void *, word>);
static_assert(jam::traceable<std::tuple<>, word>);
static_assert(jam::traceable<std::array<word_ptr, 0>, word>);
static_assert(jam::traceable<std::tuple<word, word_ptr, std::variant<word, word_ptr>>, word>);
static_assert(!jam::traceable<std::tuple<throwing_trace>, word>);
static_assert(!jam::traceable<std::array<jam::ptr<word, std::uint64_t>, 2>, word>);
static_assert(!jam::traceable<std::variant<word, jam::ptr<word, std::uint64_t>>, word>);
static_assert([] {
  // Leaf-only traversal needs no runtime visitor operations.
  std::nullptr_t visit{};
  using value = std::tuple<trace_result, std::array<int, 2>, std::variant<int, double>>;
  jam::tracer<value>::trace(visit, value{{}, {2, 3}, 4});
  return true;
}());

template<jam::oo O>
struct embedded {
  jam::ptr<payload, O> data;
  O scalar;
  template<jam::visitor<O> V>
  constexpr auto trace(V & visit) const noexcept { return visit(data, scalar); }
};

template<jam::oo O>
struct composite {
  using edge = jam::ptr<payload, O>;
  std::array<std::variant<O, edge, embedded<O>>, 3> entries;
  std::uint64_t scalar;

  template<jam::visitor<O> V>
  constexpr auto trace(V & visit) const noexcept {
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

template<jam::oo O>
void mixed_graph() noexcept {
  using H = jam::heap<O>;
  using N = node<O>;
  using R = typename H::template ptr<N>;
  static_assert(std::is_same_v<R, jam::ptr<N, O>>);
  static_assert(sizeof(R) == sizeof(O));
  static_assert(std::is_standard_layout_v<R> && std::is_trivially_copyable_v<R>);
  static_assert(!std::is_convertible_v<jam::ptr<payload, std::uint32_t>, jam::ptr<payload, std::uint64_t>>);
  H heap{jam::options{.capacity_pages = 8, .reserve_pages = 2,
                     .marking_workers = 4, .compaction_workers = 3}};
  static_cast<void>(heap.allocate(23));
  auto const data = heap.template make<payload>(payload{1, 7});
  auto const a = heap.template make<N>(N{{}, data, 101});
  static_cast<void>(heap.allocate(5));
  auto const b = heap.template make<N>(N{a, data, 202});
  heap.store(a, N{b, data, 101});
  auto root = heap.root(a);
  static_assert(std::is_same_v<decltype(root.get()), R>);
  static_assert(std::is_same_v<decltype(root), jam::root<N, O>>);
  auto copy = root;
  auto moved = std::move(copy);
  check(!copy.get(), "a moved typed root is null");
  std::vector<decltype(root)> roots;
  for (unsigned i = 0; i != 17; ++i) roots.push_back(root);
  auto null_root = heap.root(R{});

  for (unsigned round = 0; round != 4; ++round) {
    N::visits.store(0, std::memory_order_relaxed);
    heap.collect();
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
    check(heap.used() == (sizeof(O) == 4 ? 7 : 9), "unreachable cells are reclaimed");
    static_cast<void>(heap.allocate(heap.page_words()));
  }
  roots.clear();
  root = {};
  moved = {};
  heap.collect();
  check(heap.used() == 1, "dropping the last typed roots reclaims the graph");
}

void parallel_typed_discovery() noexcept {
  jam::heap<std::uint32_t, true> heap{jam::options{.capacity_pages = 8, .reserve_pages = 2,
                                       .marking_workers = 4, .compaction_workers = 2}};
  std::barrier rendezvous{2};
  aligned_branch::rendezvous = &rendezvous;
  static_cast<void>(heap.allocate(31));
  auto const data = heap.make<std::uint32_t>(77u);
  auto const left = heap.make<aligned_branch>(aligned_branch{data});
  auto const right = heap.make<aligned_branch>(aligned_branch{data});
  auto root = heap.root(heap.make<fork_node>(fork_node{left, right}));
  static_assert(std::is_same_v<decltype(root), jam::root<fork_node, std::uint32_t, true>>);
  for (unsigned round = 0; round != 3; ++round) {
    heap.collect();
    auto const fork = heap.load(root.get());
    check((fork.left.get() * 8) % 64 == 0 && (fork.right.get() * 8) % 64 == 0,
          "target types preserve their allocation alignment during compaction");
    auto const a = heap.load(fork.left);
    auto const b = heap.load(fork.right);
    check(a.data == b.data && heap.load(a.data) == 77,
          "parallel heterogeneous traversal retains a shared scalar leaf");
  }
  aligned_branch::rendezvous = nullptr;
}

template<jam::oo O>
void composite_graph() noexcept {
  using C = composite<O>;
  using E = typename C::edge;
  using A = std::array<E, 2>;
  using V = std::variant<O, E>;
  jam::heap<O> heap{jam::options{.capacity_pages = 8, .reserve_pages = 2,
                                .marking_workers = 3, .compaction_workers = 2}};
  static_cast<void>(heap.allocate(17));
  auto const first = heap.template make<payload>(payload{71, 11});
  auto const second = heap.template make<payload>(payload{83, 13});
  using Part = std::variant<O, E, embedded<O>>;
  auto root = heap.root(heap.template make<C>(C{{Part{first}, Part{O{1}}, Part{embedded<O>{second, 1}}}, 1}));
  auto array_root = heap.root(heap.template make<A>(A{first, {}}));
  auto variant_root = heap.root(heap.template make<V>(V{second}));
  for (unsigned round = 0; round != 3; ++round) {
    heap.collect();
    auto value = heap.load(root.get());
    auto const a = std::get<E>(value.entries[0]);
    auto const part = std::get<embedded<O>>(value.entries[2]);
    auto const b = part.data;
    check(part.scalar == 1, "embedded hooks share the outer allocation without reclaiming it");
    check(heap.load(a).bits == 71 && heap.load(b).bits == 83,
          "nested array, variant and tuple views follow managed targets");
    check(std::get<O>(value.entries[1]) == 1 && value.scalar == 1,
          "scalar alternatives and tuple elements remain data");
    auto const pointers = heap.load(array_root.get());
    check(pointers[0] == a && !pointers[1], "array roots forward references and preserve nulls");
    check(std::get<E>(heap.load(variant_root.get())) == b,
          "variant roots visit the active reference alternative");
    static_cast<void>(heap.allocate(heap.page_words()));
  }
  auto value = heap.load(root.get());
  value.entries[0] = O{1}; // The old pointer slot is now scalar data.
  value.entries[2] = E{};
  heap.store(root.get(), value);
  array_root = {};
  variant_root = {};
  heap.collect();
  value = heap.load(root.get());
  check(std::get<O>(value.entries[0]) == 1 && !std::get<E>(value.entries[2]),
        "changing a variant alternative clears the old pointer declaration");
  check(heap.used() == 1 + (sizeof(C) + 7) / 8, "inactive alternatives retain no targets");
  auto const replacement = heap.template make<payload>(payload{97, 17});
  value.entries[0] = replacement;
  heap.store(root.get(), value);
  heap.collect();
  check(heap.load(std::get<E>(heap.load(root.get()).entries[0])).bits == 97,
        "a scalar alternative can become a managed edge on the next collection");
}

int main() {
  mixed_graph<std::uint32_t>();
  mixed_graph<std::uint64_t>();
  parallel_typed_discovery();
  composite_graph<std::uint32_t>();
  composite_graph<std::uint64_t>();
  std::puts("typed graph checks passed in both reference widths");
}
