// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "../etc/page-size.h"

import jam;

auto const page_bytes = static_cast<std::uint64_t>(system_page_size());

namespace {

using H = jam::heap;

void check(bool value, std::string_view message) noexcept {
  if (value) return;
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
  std::abort();
}

std::uint64_t payload(std::size_t index) noexcept {
  return 0x9e3779b97f4a7c15ULL * (index + 1) ^ 0xd1b54a32d192ed03ULL;
}

std::uint64_t replace(std::uint64_t word, unsigned slot, H::offset value) noexcept {
  auto const shift = slot * 32;
  return (word & ~(std::uint64_t{0xffffffff} << shift)) | (std::uint64_t{value} << shift);
}

void exact_fields() noexcept {
  using offset = H::offset;
  static_assert(sizeof(H::block) == 16);
  H heap{jam::heap_options{.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}, .workers = 4}};
  static_cast<void>(heap.allocate(31));
  std::vector<offset> records{heap.allocate(4)};
  static_cast<void>(heap.allocate(28));
  records.push_back(heap.allocate(5));
  static_cast<void>(heap.allocate(heap.page_words() - 1 - heap.used()));
  records.push_back(heap.allocate(7));
  std::vector<std::size_t> const widths{4, 5, 7};
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  struct field { offset cell; unsigned slot; offset target; };
  std::vector<field> fields;
  for (std::size_t i = 0; i < records.size(); ++i) {
    auto const start = records[i];
    fields.push_back({start, 0, records[(i + 1) % records.size()]});
    fields.push_back({start, 1, start});
    heap.set_field(start + 2, std::numeric_limits<offset>::max(), 1); // The undeclared half must remain raw data.
    fields.push_back({static_cast<offset>(start + 2), 0, H::null});
  }
  for (auto const & f : fields) { heap.set_field(f.cell, f.target, f.slot); heap.pointer(f.cell, f.slot); }
  std::vector<unsigned char> live(heap.used(), 0);
  for (std::size_t i = 0; i < records.size(); ++i)
    std::fill_n(live.begin() + records[i], widths[i], 1);
  std::vector<offset> forwarded(heap.used(), H::null);
  offset count = 1;
  for (std::size_t i = 0; i < live.size(); ++i) if (live[i]) forwarded[i] = count++;
  std::vector<std::uint64_t> expected{0};
  for (std::size_t i = 0; i < live.size(); ++i) if (live[i]) expected.push_back(heap[i]);
  for (auto const & f : fields)
    expected[forwarded[f.cell]] = replace(expected[forwarded[f.cell]], f.slot,
      f.target == H::null ? H::null : forwarded[f.target]);
  std::vector<offset> roots = records;
  roots.push_back(static_cast<offset>(records.back() + widths.back() - 1));
  roots.push_back(H::null);
  auto expected_roots = roots;
  for (auto & root : expected_roots) if (root != H::null) root = forwarded[root];
  heap.clear_marks();
  for (std::size_t i = 0; i < records.size(); ++i) heap.mark(records[i], widths[i]);
  heap.compact(std::span<offset>{roots});
  check(roots == expected_roots && heap.used() == expected.size(), "exact field mode forwards roots and live words");
  for (std::size_t i = 0; i < expected.size(); ++i)
    check(heap[i] == expected[i], "both pointer halves and undeclared data survive packing");
}

void aligned_neighbors_and_rotations() noexcept {
  using offset = H::offset;
  H heap{jam::heap_options{.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}, .workers = 4}};
  static_cast<void>(heap.allocate(H::block_words - 8));
  std::vector<offset> roots{heap.allocate(3, 64)};
  static_cast<void>(heap.allocate(5));
  roots.push_back(heap.allocate(5, 64));
  static_cast<void>(heap.allocate(3));
  static_cast<void>(heap.allocate(heap.page_words() - 8 - heap.used()));
  roots.push_back(heap.allocate(2 * heap.page_words() + 11, 64));
  static_cast<void>(heap.allocate(5));
  std::vector<std::size_t> const widths{3, 5, 2 * heap.page_words() + 11};
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  std::vector<std::vector<std::uint64_t>> originals;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    heap.set_field(roots[i], roots[(i + 1) % roots.size()]);
    heap.pointer(roots[i], 0);
    heap.set_field(static_cast<offset>(roots[i] + widths[i] - 1), H::null);
    heap.pointer(static_cast<offset>(roots[i] + widths[i] - 1), 0);
    heap.set_field(static_cast<offset>(roots[i] + widths[i]), std::numeric_limits<offset>::max());
    heap.pointer(static_cast<offset>(roots[i] + widths[i]), 0); // Dead neighbor: must not be forwarded.
    originals.emplace_back(heap.data() + roots[i], heap.data() + roots[i] + widths[i]);
  }
  auto const capacity = heap.capacity();
  for (unsigned round = 0; round != 10; ++round) {
    heap.clear_marks();
    for (std::size_t i = 0; i < roots.size(); ++i) heap.mark(roots[i], widths[i], 64);
    heap.compact(std::span<offset>{roots});
    check(heap.capacity() == capacity, "aligned live data prevents resizing during rotation");
    for (std::size_t i = 0; i < roots.size(); ++i) {
      check((roots[i] * sizeof(std::uint64_t)) % 64 == 0, "odd-width records retain 64-byte alignment");
      for (std::size_t j = 0; j < widths[i]; ++j) {
        auto expected = originals[i][j];
        if (j == 0) expected = replace(expected, 0, roots[(i + 1) % roots.size()]);
        if (j + 1 == widths[i]) expected = replace(expected, 0, H::null);
        check(heap[roots[i] + j] == expected, "aligned records retain all payload cells and cyclic fields");
      }
      check(heap.field(static_cast<offset>(roots[i] + widths[i]), 0) == std::numeric_limits<offset>::max(),
            "unmarked neighboring pointer bytes are copied without forwarding");
    }
    check(heap.start() + heap.used() > heap.capacity() || round != 0,
          "aligned fixture crosses the ring seam on its first rotation");
  }
}

void aligned_parallel_claims() noexcept {
  using offset = H::offset;
  H heap{jam::heap_options{.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}, .workers = 4}};
  std::vector<std::size_t> const widths{3, 65, heap.page_words() + 3, 5};
  std::vector<offset> roots;
  for (auto const width : widths) {
    static_cast<void>(heap.allocate(1));
    roots.push_back(heap.allocate(width, 64));
  }
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  std::vector<std::vector<std::uint64_t>> originals;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    heap.set_field(roots[i], roots[(i + 1) % roots.size()]);
    heap.set_field(static_cast<offset>(roots[i] + 1), roots[i]);
    originals.emplace_back(heap.data() + roots[i], heap.data() + roots[i] + widths[i]);
  }
  heap.clear_marks();
  std::vector<std::atomic<unsigned>> winners(roots.size());
  std::vector<std::size_t> indices(heap.used(), roots.size());
  for (std::size_t i = 0; i < roots.size(); ++i) indices[roots[i]] = i;
  auto const visit = [&](auto const & self, std::size_t index) noexcept -> void {
    auto const start = roots[index];
    if (!heap.claim(start, widths[index], 64)) return;
    winners[index].fetch_add(1, std::memory_order_relaxed);
    heap.pointer(start, 0);
    heap.pointer(static_cast<offset>(start + 1), 0);
    self(self, indices[heap.field(start, 0)]);
    self(self, index);
  };
  std::barrier gate{4};
  std::vector<std::jthread> markers;
  for (std::size_t worker = 0; worker != 4; ++worker)
    markers.emplace_back([&, worker]() noexcept { gate.arrive_and_wait(); visit(visit, worker); });
  markers.clear();
  for (auto const & winner : winners)
    check(winner.load(std::memory_order_relaxed) == 1, "aligned cyclic record has one claiming marker");
  heap.compact(std::span<offset>{roots});
  for (std::size_t i = 0; i < roots.size(); ++i) {
    check((roots[i] * 8) % 64 == 0, "parallel claims preserve requested alignment");
    for (std::size_t j = 0; j < widths[i]; ++j) {
      auto expected = originals[i][j];
      if (j == 0) expected = replace(expected, 0, roots[(i + 1) % roots.size()]);
      if (j == 1) expected = replace(expected, 0, roots[i]);
      check(heap[roots[i] + j] == expected, "claimed extents and frozen cyclic payload survive compaction");
    }
  }
}

void mixed_alignment_claims_share_one_rank_block() noexcept {
  using offset = H::offset;
  H heap{jam::heap_options{.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}, .workers = 4}};
  static_cast<void>(heap.allocate(1));
  std::vector<offset> roots{heap.allocate(1, 16)};
  static_cast<void>(heap.allocate(3));
  roots.push_back(heap.allocate(1, 32));
  static_cast<void>(heap.allocate(H::block_words - heap.used()));
  roots.push_back(heap.allocate(3, 64));
  std::vector<std::size_t> const widths{1, 1, 3};
  std::vector<unsigned> const alignments{16, 32, 64};
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  std::vector<std::vector<std::uint64_t>> original;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    heap.set_field(roots[i], roots[(i + 1) % roots.size()]);
    original.emplace_back(heap.data() + roots[i], heap.data() + roots[i] + widths[i]);
  }
  heap.clear_marks();
  std::barrier gate{3};
  std::vector<std::atomic<unsigned>> winners(3);
  std::vector<std::jthread> markers;
  for (std::size_t i = 0; i != roots.size(); ++i)
    markers.emplace_back([&, i]() noexcept {
      gate.arrive_and_wait();
      if (heap.claim(roots[i], widths[i], alignments[i])) {
        winners[i].fetch_add(1, std::memory_order_relaxed);
        heap.pointer(roots[i], 0);
      }
    });
  markers.clear();
  for (auto const & winner : winners)
    check(winner.load(std::memory_order_relaxed) == 1, "mixed-alignment markers own their records once");
  heap.compact(std::span<offset>{roots});
  check(roots == std::vector<offset>{2, 4, 8} && heap.used() == 16,
        "16- and 32-byte marks in one block take max alignment, not 1|2=3");
  for (std::size_t i = 0; i < roots.size(); ++i)
    for (std::size_t j = 0; j < widths[i]; ++j) {
      auto expected = original[i][j];
      if (j == 0) expected = replace(expected, 0, roots[(i + 1) % roots.size()]);
      check(heap[roots[i] + j] == expected, "mixed alignment preserves records and fields");
    }
}

void zero_is_reserved() noexcept {
  static_assert(H::null == 0);
  H heap{jam::heap_options{.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}};
  for (unsigned round = 0; round != 3; ++round) {
    auto const at = heap.allocate(1);
    check(at != 0, "allocation never returns the null offset");
    heap[at] = 42;
    auto root = heap.root(at);
    heap.collect([&](auto & visit, auto start) noexcept {
      static_cast<void>(visit.claim(start));
    });
    check(root.get() == 1 && heap[root.get()] == 42,
          "compaction preserves cell zero and forwards the first object to one");
    root = {};
    heap.collect([](auto &, auto) noexcept { std::abort(); });
    check(heap.used() == 1, "empty collection retains only the reserved null cell");
  }
}

} // namespace

int main() noexcept {
  zero_is_reserved();
  exact_fields();
  aligned_neighbors_and_rotations();
  aligned_parallel_claims();
  mixed_alignment_claims_share_one_rank_block();
  std::puts("5 heap layout checks passed");
  return 0;
}
