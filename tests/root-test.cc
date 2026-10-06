// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../etc/page-size.h"

import jam;

auto const page_bytes = static_cast<std::uint64_t>(system_page_size());

namespace {

using heap_type = jam::heap;
using offset = heap_type::offset;
using root_handle = heap_type::root_handle;

static_assert(std::is_nothrow_default_constructible_v<root_handle>);
static_assert(std::is_nothrow_copy_constructible_v<root_handle>);
static_assert(std::is_nothrow_move_constructible_v<root_handle>);
static_assert(std::is_nothrow_copy_assignable_v<root_handle>);
static_assert(std::is_nothrow_move_assignable_v<root_handle>);

void check(bool value, std::string_view message) noexcept {
  if (value) return;
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
  std::abort();
}

offset leaf(heap_type & heap, std::uint64_t value) noexcept {
  auto const start = heap.allocate(3);
  heap.set_field(start, heap_type::null);
  heap.set_field(static_cast<offset>(start + 1), heap_type::null);
  heap[start + 2] = value;
  return start;
}

void trace_leaf(heap_type::visitor & visitor, offset start) noexcept {
  if (!visitor.claim(start, 3)) return;
  static_cast<void>(visitor.field(start));
  static_cast<void>(visitor.field(static_cast<offset>(start + 1)));
}

void cycles_and_shared_children() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  static_cast<void>(heap.allocate(13));
  auto const a = leaf(heap, 1);
  static_cast<void>(heap.allocate(7));
  auto const b = leaf(heap, 2);
  static_cast<void>(heap.allocate(11));
  auto const shared = leaf(heap, 3);
  static_cast<void>(leaf(heap, 4)); // Unreachable and never traced.
  heap.set_field(a, b);
  heap.set_field(static_cast<offset>(a + 1), shared);
  heap.set_field(b, a);
  heap.set_field(static_cast<offset>(b + 1), shared);
  heap.set_field(static_cast<offset>(shared + 1), shared);
  auto root = heap.root(a);
  for (unsigned round = 0; round != 4; ++round) {
    std::array<std::atomic<unsigned>, 4> visits{};
    heap.collect([&](heap_type::visitor & visitor, offset start) noexcept {
      if (!visitor.claim(start, 3)) return;
      auto const id = heap[start + 2];
      check(id >= 1 && id <= visits.size(), "a discovered record retains its payload tag");
      visits[id - 1].fetch_add(1, std::memory_order_relaxed);
      static_cast<void>(visitor.field(start));
      static_cast<void>(visitor.field(static_cast<offset>(start + 1)));
    });
    check(heap.used() == 10 && root.get() == 1, "one root keeps exactly the reachable cyclic graph");
    check(*root == root.get(), "dereference observes the current forwarded root offset");
    auto const next = heap.field(root.get());
    auto const child = heap.field(static_cast<offset>(root.get() + 1));
    check(next == 4 && child == 7, "stable compaction preserves the graph's record order");
    check(heap.field(next) == root.get() && heap.field(static_cast<offset>(next + 1)) == child,
          "a cycle and a shared child retain both incoming edges");
    check(heap.field(child) == heap_type::null
          && heap.field(static_cast<offset>(child + 1)) == child,
          "null and self edges survive root-driven collection");
    for (std::size_t i = 0; i != 3; ++i)
      check(visits[i].load(std::memory_order_relaxed) == 1,
            "only one winning tracer visits each shared or cyclic record");
    check(visits[3].load(std::memory_order_relaxed) == 0, "an unreachable record is not traced");
    for (offset i = 0; i != 3; ++i)
      check(heap[i * 3 + 3] == i + 1, "tracing and rotation preserve every record payload");
    static_cast<void>(heap.allocate(5 + round));
  }
}

void copied_and_moved_roots_survive_vector_relocation() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  static_cast<void>(heap.allocate(11));
  auto a = heap.root(leaf(heap, 101));
  auto b = a;
  auto moved = std::move(a);
  check(a.get() == heap_type::null, "moving a public root_handle empties its old hook");
  a = b;
  std::vector<root_handle> roots;
  roots.reserve(1);
  roots.push_back(a);
  roots.push_back(std::move(moved));
  check(moved.get() == heap_type::null, "moving into a vector empties the source root_handle");
  for (unsigned i = 0; i != 31; ++i) roots.push_back(i % 2 ? a : b);
  auto copies = roots;
  for (unsigned round = 0; round != 3; ++round) {
    heap.collect(trace_leaf);
    check(heap.used() == 4 && a.get() == 1 && b.get() == 1,
          "copied intrusive hooks all follow their shared record");
    for (auto const & root : roots)
      check(root.get() == a.get(), "vector relocation relinks every moved root hook");
    for (auto const & root : copies)
      check(root.get() == a.get(), "copied vectors register independent root hooks");
    check(heap[a.get() + 2] == 101, "root relocation preserves the shared payload");
    static_cast<void>(heap.allocate(19 + round));
  }
}

void assignment_changes_the_root_owner() noexcept {
  heap_type first{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  heap_type second{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  static_cast<void>(first.allocate(5));
  static_cast<void>(second.allocate(17));
  auto assigned = first.root(leaf(first, 111));
  auto source = second.root(leaf(second, 222));
  assigned = source;
  first.collect(trace_leaf);
  second.collect(trace_leaf);
  check(first.used() == 1, "cross-heap copy assignment unregisters the previous owner's hook");
  check(second.used() == 4 && assigned.get() == source.get()
        && second[assigned.get() + 2] == 222,
        "cross-heap copy assignment registers with the new owner");
  auto replacement = first.root(leaf(first, 333));
  assigned = std::move(replacement);
  check(replacement.get() == heap_type::null, "cross-heap move assignment empties its source");
  source = root_handle{};
  first.collect(trace_leaf);
  second.collect(trace_leaf);
  check(first.used() == 4 && first[assigned.get() + 2] == 333,
        "cross-heap move assignment transfers the new root hook");
  check(second.used() == 1, "the old owner's record dies after its last root_handle is reset");
  auto attached_null = second.root(heap_type::null);
  auto copied_null = attached_null;
  assigned = copied_null;
  check(assigned.get() == heap_type::null, "copying an attached null root_handle changes owners safely");
  first.collect(trace_leaf);
  second.collect(trace_leaf);
  check(first.used() == 1 && second.used() == 1, "null hooks never keep a record alive");
  root_handle empty;
  copied_null = empty;
  attached_null = std::move(empty);
  assigned = root_handle{};
  check(empty.get() == heap_type::null && copied_null.get() == heap_type::null
        && attached_null.get() == heap_type::null && assigned.get() == heap_type::null,
        "default pointers copy and move without acquiring an arena record");
}

void dropped_roots_reclaim_records() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 3}};
  static_cast<void>(heap.allocate(23));
  auto kept = heap.root(leaf(heap, 444));
  {
    auto dropped = heap.root(leaf(heap, 555));
    auto copy = dropped;
    check(copy.get() == dropped.get(), "the dropped record initially has two public roots");
  }
  heap.collect(trace_leaf);
  check(heap.used() == 4 && heap[kept.get() + 2] == 444,
        "destructing the last public hook reclaims only its unreachable record");
  kept = root_handle{};
  std::atomic<unsigned> visits{0};
  heap.collect([&](heap_type::visitor &, offset) noexcept {
    visits.fetch_add(1, std::memory_order_relaxed);
  });
  check(heap.used() == 1 && visits.load(std::memory_order_relaxed) == 0,
        "resetting the final root gives an empty trace and an empty heap");
}

void empty_collections_ignore_null_roots() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  root_handle empty;
  auto attached = heap.root(heap_type::null);
  auto copy = attached;
  for (unsigned round = 0; round != 3; ++round) {
    static_cast<void>(heap.allocate(3 + round));
    std::atomic<unsigned> visits{0};
    heap.collect([&](heap_type::visitor &, offset) noexcept {
      visits.fetch_add(1, std::memory_order_relaxed);
    });
    check(heap.used() == 1 && visits.load(std::memory_order_relaxed) == 0,
          "empty collections complete with null hooks and multiple configured workers");
    check(empty.get() == heap_type::null && attached.get() == heap_type::null
          && copy.get() == heap_type::null, "null pointers remain null across collection and resizing");
  }
}

void mixed_alignment_traces_preserve_records() noexcept {
  using aligned_heap = jam::heap;
  using aligned_offset = aligned_heap::offset;
  aligned_heap heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}, .workers = 4}};
  static_cast<void>(heap.allocate(1));
  std::array<aligned_offset, 3> records{heap.allocate(1, 16), 0, 0};
  static_cast<void>(heap.allocate(3));
  records[1] = heap.allocate(1, 32);
  static_cast<void>(heap.allocate(aligned_heap::block_words - heap.used()));
  records[2] = heap.allocate(3, 64);
  constexpr std::array<std::size_t, 3> widths{1, 1, 3};
  constexpr std::array<std::size_t, 3> alignments{16, 32, 64};
  for (std::size_t i = 1; i != heap.used(); ++i) heap[i] = 0x1234000000000000ULL + i;
  std::array<std::uint64_t, 3> heads{};
  for (std::size_t i = 0; i != records.size(); ++i) {
    heap.set_field(records[i], records[(i + 1) % records.size()]);
    heads[i] = heap[records[i]];
  }
  auto const tail0 = heap[records[2] + 1];
  auto const tail1 = heap[records[2] + 2];
  auto root = heap.root(records[0]);
  std::array<std::atomic<unsigned>, 3> visits{};
  heap.collect([&](aligned_heap::visitor & visitor, aligned_offset start) noexcept {
    std::size_t index = 0;
    while (index != records.size() && records[index] != start) ++index;
    check(index != records.size(), "every mixed-alignment target names a record start");
    if (!visitor.claim(start, widths[index], alignments[index])) return;
    visits[index].fetch_add(1, std::memory_order_relaxed);
    static_cast<void>(visitor.field(start));
  });
  std::array<aligned_offset, 3> const forwarded{
    root.get(), heap.field(root.get()), heap.field(heap.field(root.get()))};
  check(forwarded == std::array<aligned_offset, 3>{2, 4, 8} && heap.used() == 16,
        "traced 16- and 32-byte neighbors use the maximum block alignment");
  for (std::size_t i = 0; i != records.size(); ++i) {
    check((forwarded[i] * 8) % alignments[i] == 0, "visitor claims retain each requested alignment");
    check(visits[i].load(std::memory_order_relaxed) == 1, "each mixed-alignment record is traced once");
    auto const expected = (heads[i] & 0xffffffff00000000ULL) | forwarded[(i + 1) % records.size()];
    check(heap[forwarded[i]] == expected, "compressed forwarding preserves the other half of a cell");
  }
  check(heap[forwarded[2] + 1] == tail0 && heap[forwarded[2] + 2] == tail1,
        "mixed-alignment tracing copies the full odd-width record payload");
}

void a_single_root_discovers_parallel_branches() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 2}};
  static_cast<void>(heap.allocate(9));
  auto const parent = leaf(heap, 0);
  auto const left = leaf(heap, 1);
  auto const right = leaf(heap, 2);
  heap.set_field(parent, left);
  heap.set_field(parent, right, 1);
  auto const untouched = heap[parent + 1];
  auto root = heap.root(parent);
  std::array<std::thread::id, 2> threads{};
  std::array<std::atomic<unsigned>, 3> visits{};
  heap.collect([&](heap_type::visitor & visitor, offset start) noexcept {
    if (!visitor.claim(start, 3)) return;
    auto const id = heap[start + 2];
    check(id < visits.size(), "the parallel branch tracer sees a valid record tag");
    visits[id].fetch_add(1, std::memory_order_relaxed);
    if (id != 0) {
      threads[id - 1] = std::this_thread::get_id();
    }
    auto const first = visitor.field(start);
    if (id == 0) {
      check(first == left && visitor.field(start, 1) == right,
            "visiting either compressed half returns its old target while scheduling it");
    } else {
      check(first == heap_type::null
            && visitor.field(static_cast<offset>(start + 1)) == heap_type::null,
            "visiting a null field returns null without scheduling a record");
    }
  });
  check(threads[0] != std::thread::id{} && threads[1] != std::thread::id{},
        "both discovered branches run; sibling concurrency is not guaranteed");
  for (auto const & count : visits)
    check(count.load(std::memory_order_relaxed) == 1, "parallel discovery traces each record exactly once");
  check(heap.used() == 10 && root.get() == 1 && heap.field(root.get()) == 4
        && heap.field(root.get(), 1) == 7 && heap[root.get() + 1] == untouched,
        "parallel discovery forwards both compressed halves and preserves undeclared payload");
}

} // namespace

int main() noexcept {
  cycles_and_shared_children();
  copied_and_moved_roots_survive_vector_relocation();
  assignment_changes_the_root_owner();
  dropped_roots_reclaim_records();
  empty_collections_ignore_null_roots();
  mixed_alignment_traces_preserve_records();
  a_single_root_discovers_parallel_branches();
  std::puts("7 intrusive root checks passed");
  return 0;
}
