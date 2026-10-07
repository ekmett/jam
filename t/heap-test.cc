// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <crtdbg.h>
#else
#include <sys/resource.h>
#include <sys/wait.h>
#endif
#include "../etc/page-size.h"

import jam;

extern "C" void check_compactors() noexcept;

auto const page_bytes = static_cast<std::uint64_t>(system_page_size());

namespace {

using heap_type = jam::heap;

using word = heap_type::word;
using offset = heap_type::offset;
constexpr offset null = heap_type::null;
static_assert(sizeof(heap_type::block) == 16);
char const * executable = nullptr;

void check(bool value, std::string_view message) noexcept {
  if (value) return;
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
  std::abort();
}

word payload(std::size_t index) noexcept {
  return 0x9e3779b97f4a7c15ULL * (index + 1) ^ 0xd1b54a32d192ed03ULL;
}

struct expected {
  std::vector<word> words;
  std::vector<offset> roots;
};

expected oracle(heap_type const & heap,
                std::vector<unsigned char> const & live,
                std::vector<unsigned char> const & pointers,
                std::span<offset const> roots) noexcept {
  check(live.size() == heap.used() && pointers.size() == live.size(), "oracle covers old used words");
  std::vector<offset> forwarded(live.size(), null);
  offset count = 1;
  for (std::size_t i = 1; i < live.size(); ++i)
    if (live[i]) forwarded[i] = count++;
  auto translate = [&](offset old) noexcept -> offset {
    if (old == null) return null;
    check(old < live.size() && live[static_cast<std::size_t>(old)], "fixture reference targets a live word");
    return forwarded[static_cast<std::size_t>(old)];
  };
  expected result{{0}, {}};
  result.words.reserve(static_cast<std::size_t>(count));
  for (std::size_t i = 1; i < live.size(); ++i)
    if (live[i]) result.words.push_back(pointers[i] ? translate(heap[i]) : heap[i]);
  for (auto const root : roots) result.roots.push_back(translate(root));
  return result;
}

void marks(heap_type & heap, std::vector<unsigned char> const & live,
           std::vector<unsigned char> const & pointers) noexcept {
  heap.clear_marks();
  for (std::size_t i = 1; i < live.size();) {
    if (!live[i]) { ++i; continue; }
    auto const begin = i;
    while (i < live.size() && live[i]) ++i;
    heap.mark(static_cast<offset>(begin), i - begin);
  }
  for (std::size_t i = 1; i < pointers.size(); ++i)
    if (pointers[i]) heap.pointer(static_cast<offset>(i));
}

void verify(heap_type const & heap, expected const & result,
            std::vector<offset> const & roots) noexcept {
  check(heap.used() == result.words.size(), "compaction preserves exactly the live words");
  check(roots == result.roots, "external roots are forwarded exactly");
  for (std::size_t i = 0; i < result.words.size(); ++i)
    check(heap[i] == result.words[i], "stable packed payload and pointer forwarding match oracle");
  check(heap.start() < heap.capacity(), "heap start is inside the ring");
  check(heap.used() <= heap.capacity() - heap.reserved(), "the reserved gap remains available");
}

void compact(heap_type & heap, std::vector<unsigned char> const & live,
             std::vector<unsigned char> const & pointers,
             std::vector<offset> & roots) noexcept {
  auto const result = oracle(heap, live, pointers, roots);
  marks(heap, live, pointers);
  heap.compact(std::span<offset>{roots});
  verify(heap, result, roots);
}

void byte_literals() noexcept {
  using namespace jam;
  using namespace jam::units;
  static_assert(0_KiB == 0_B && 1_KiB == 1024_B);
  static_assert(1_MiB == 1048576_B && 1_GiB == 1073741824_B);
  heap_type heap{jam::heap_options{.old = {.capacity = 3_MiB, .reserve = units::ceil<units::pages>(256_KiB)}, .young = {.capacity = 3_MiB, .reserve = units::ceil<units::pages>(256_KiB)}}};
  check(heap.capacity() * 8 == 3 * (1ull << 20), "capacity need not be a power of two");
  check(heap.reserved() * 8 == 256 * 1024, "binary literals retain their byte meaning");
  heap_type rounded{jam::heap_options{.old = {.capacity = jam::units::ceil<jam::units::pages>(jam::units::bytes{3 * page_bytes - 1}), .reserve = jam::units::ceil<jam::units::pages>(jam::units::bytes{page_bytes - 1})}, .young = {.capacity = jam::units::ceil<jam::units::pages>(jam::units::bytes{3 * page_bytes - 1}), .reserve = jam::units::ceil<jam::units::pages>(jam::units::bytes{page_bytes - 1})}}};
  check(rounded.capacity() == 3 * rounded.page_words() && rounded.reserved() == rounded.page_words(),
        "explicit ceiling converts byte counts to whole pages");
  check(rounded.configuration().old.capacity.count() == 3 && rounded.configuration().old.reserve.count() == 1,
        "configuration retains typed page counts");
}

void alias_and_layout() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  auto const page = heap.page_words();
  check(page != 0 && page % 64 == 0, "VM pages contain whole 512-byte mark blocks");
  check(heap.capacity() == 8 * page && heap.reserved() == 2 * page, "capacity and reserve use words");
  check(heap.used() == 1 && heap.start() == 0, "a new heap begins at ring offset zero");
  check(heap.allocate(16) == 1, "the first allocation skips the null cell");
  auto * const data = heap.data();
  data[3] = 123;
  check(data[heap.capacity() + 3] == 123, "the second view observes first-view writes");
  data[heap.capacity() + 7] = 456;
  check(data[7] == 456 && heap[7] == 456, "the first view observes second-view writes");
  heap_type const & view = heap;
  check(view.data()[3] == 123 && view[7] == 456, "const access uses the same backing");
}

void allocation_growth_preserves_reserve() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  auto const old_capacity = heap.capacity();
  auto const limit = old_capacity - heap.reserved();
  check(heap.allocate(limit - 1) == 1, "allocation can fill the nonreserved portion");
  check(heap.capacity() == old_capacity && heap.used() == limit, "filling usable capacity does not grow");
  for (std::size_t i = 1; i < limit; ++i) heap[i] = payload(i);
  check(heap.allocate(1) == limit, "growth preserves allocation offsets");
  check(heap.capacity() == 2 * old_capacity, "allocation doubles before consuming the reserve");
  check(heap.used() == limit + 1, "growth advances used by the requested size only");
  for (std::size_t i = 1; i < limit; ++i)
    check(heap[i] == payload(i), "growth preserves existing words");
  check(heap.used() <= heap.capacity() - heap.reserved(), "growth keeps the complete reserve");
}

void cyclic_edges_and_roots() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
  static_cast<void>(heap.allocate(3));
  auto const a = heap.allocate(5);
  static_cast<void>(heap.allocate(7));
  auto const b = heap.allocate(7);
  static_cast<void>(heap.allocate(2));
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  heap[a] = b;
  heap[a + 1] = a;
  heap[a + 2] = null;
  heap[b] = a;
  heap[b + 1] = b + 4;
  heap[b + 2] = null;
  std::vector<unsigned char> live(heap.used(), 0), pointers(heap.used(), 0);
  std::fill_n(live.begin() + static_cast<std::size_t>(a), 5, 1);
  std::fill_n(live.begin() + static_cast<std::size_t>(b), 7, 1);
  for (auto const base : {a, b})
    for (offset i = 0; i != 3; ++i) pointers[static_cast<std::size_t>(base + i)] = 1;
  std::vector<offset> roots{a, b, b + 4, null, a + 4};
  compact(heap, live, pointers, roots);
  check(roots == std::vector<offset>{1, 6, 10, null, 5}, "record and interior roots retain stable order");
  check(heap[1] == 6 && heap[2] == 1 && heap[6] == 1 && heap[7] == 10, "cycles and interior pointers survive packing");
  check(heap[3] == null && heap[8] == null, "null pointer fields are preserved");
}

void straddling_records_and_ring_wrap() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}}};
  auto const page = heap.page_words();
  auto const capacity = heap.capacity();
  static_cast<void>(heap.allocate(61));
  std::vector<std::size_t> const widths{9, 65, page + 5, page + 58};
  std::vector<offset> roots;
  for (auto const width : widths) {
    roots.push_back(heap.allocate(width));
    static_cast<void>(heap.allocate(3));
  }
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  std::vector<unsigned char> live(heap.used(), 0), pointers(heap.used(), 0);
  for (std::size_t i = 0; i < roots.size(); ++i)
    std::fill_n(live.begin() + static_cast<std::size_t>(roots[i]), widths[i], 1);
  auto const original = oracle(heap, live, pointers, roots);
  compact(heap, live, pointers, roots);
  check(heap.capacity() == capacity, "enough live data suppresses shrinking");
  check(heap.start() == capacity - heap.reserved(), "compaction rotates backward by the reserved gap");
  check(heap.start() + heap.used() > heap.capacity(), "packed records cross the physical ring seam");
  for (std::size_t round = 1; round != 12; ++round) {
    static_cast<void>(heap.allocate(17 + round));
    live.assign(heap.used(), 0);
    pointers.assign(heap.used(), 0);
    std::fill_n(live.begin(), original.words.size(), 1);
    auto const before = heap.start();
    compact(heap, live, pointers, roots);
    check(heap.start() == (before + capacity - heap.reserved()) % capacity, "repeated compactions wrap ring start exactly");
    check(heap.capacity() == capacity && heap.used() == original.words.size(), "repeated rotations preserve layout size");
    for (std::size_t i = 0; i < original.words.size(); ++i)
      check(heap[i] == original.words[i], "records survive repeated page and ring seam crossings");
  }
}

void parallel_matches_scalar() noexcept {
  heap_type scalar{jam::heap_options{.old = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{4}}, .young = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{4}}}};
  heap_type parallel{jam::heap_options{.old = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{4}}, .young = {.capacity = jam::units::pages{16}, .reserve = jam::units::pages{4}}, .workers = 4}};
  auto const count = 8 * scalar.page_words() + 71;
  static_cast<void>(scalar.allocate(count - 1));
  static_cast<void>(parallel.allocate(count - 1));
  std::vector<unsigned char> live(count, 0), pointers(count, 0);
  for (std::size_t i = 1; i < count; ++i) {
    live[i] = static_cast<unsigned char>(i % 7 != 0);
    scalar[i] = payload(i);
    parallel[i] = payload(i);
  }
  for (std::size_t i = 1; i + 2 < count; i += 257) {
    if (!live[i]) continue;
    auto target = (i + scalar.page_words() + 3) % count;
    while (!live[target]) target = (target + 1) % count;
    pointers[i] = 1;
    scalar[i] = target;
    parallel[i] = target;
  }
  std::vector<offset> scalar_roots{1, 3, static_cast<offset>(count - 1), null};
  while (!live[static_cast<std::size_t>(scalar_roots[2])]) --scalar_roots[2];
  auto parallel_roots = scalar_roots;
  compact(scalar, live, pointers, scalar_roots);
  compact(parallel, live, pointers, parallel_roots);
  check(scalar_roots == parallel_roots, "parallel and scalar roots agree");
  check(scalar.capacity() == parallel.capacity() && scalar.start() == parallel.start()
        && scalar.used() == parallel.used(), "parallel and scalar ring geometry agrees");
  for (std::size_t i = 0; i < scalar.used(); ++i)
    check(scalar[i] == parallel[i], "parallel and scalar compacted words agree");
}

void growth_of_a_wrapped_view(std::size_t pages = 8) noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{1}}, .young = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{1}}}};
  auto const page = heap.page_words();
  auto const old_capacity = heap.capacity();
  auto const count = 2 * page + 137;
  static_cast<void>(heap.allocate(count - 1));
  for (std::size_t i = 1; i < count; ++i) heap[i] = payload(i);
  std::vector<unsigned char> live(count, 1), pointers(count, 0);
  std::vector<offset> roots{1, static_cast<offset>(count - 1)};
  compact(heap, live, pointers, roots);
  check(heap.start() + heap.used() > old_capacity, "fixture begins with a wrapped view");
  auto const limit = old_capacity - heap.reserved();
  check(heap.allocate(limit - heap.used()) == count, "wrapped allocation reaches the reserved boundary");
  for (std::size_t i = count; i < limit; ++i) heap[i] = payload(i);
  check(heap.allocate(1) == limit, "wrapped growth preserves next logical offset");
  check(heap.capacity() == 2 * old_capacity, "wrapped heap doubles capacity");
  for (std::size_t i = 1; i < limit; ++i)
    check(heap[i] == payload(i), "wrapped growth preserves every existing word and offset");
  check(roots == std::vector<offset>{1, static_cast<offset>(count - 1)}, "growth leaves logical root offsets unchanged");
}

void shrinking_preserves_records_and_gap(std::size_t pages = 16) noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{2}}}};
  auto const page = heap.page_words();
  static_cast<void>(heap.allocate(page + 61));
  auto const a = heap.allocate(3);
  static_cast<void>(heap.allocate(page + 3));
  auto const b = heap.allocate(5);
  static_cast<void>(heap.allocate(2 * page));
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  heap[a] = b;
  heap[b] = a;
  heap[b + 1] = null;
  std::vector<unsigned char> live(heap.used(), 0), pointers(heap.used(), 0);
  std::fill_n(live.begin() + static_cast<std::size_t>(a), 3, 1);
  std::fill_n(live.begin() + static_cast<std::size_t>(b), 5, 1);
  pointers[static_cast<std::size_t>(a)] = pointers[static_cast<std::size_t>(b)]
    = pointers[static_cast<std::size_t>(b + 1)] = 1;
  std::vector<offset> roots{a, b, null};
  compact(heap, live, pointers, roots);
  check(heap.capacity() == (pages / 2) * page, "a sparse collection halves to whole pages only once");
  check(heap.reserved() == 2 * page, "shrinking retains the configured reserve");
  for (std::size_t round = 0; round != 3; ++round) {
    live.assign(heap.used(), 1);
    pointers.assign(heap.used(), 0);
    pointers[1] = pointers[4] = pointers[5] = 1;
    compact(heap, live, pointers, roots);
    check(heap.capacity() == 4 * page, "shrinking stops at twice the reserve");
    check(heap[1] == 4 && heap[4] == 1 && heap[5] == null, "shrinking preserves cycles and null pointers");
  }
}

void full_parallel_waves_preserve_every_word(std::size_t pages = 16) noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{pages}, .reserve = jam::units::pages{2}}, .workers = 4}};
  auto const capacity = heap.capacity();
  auto const count = capacity - heap.reserved();
  auto const page = heap.page_words();
  static_cast<void>(heap.allocate(count - 1));
  for (std::size_t i = 1; i < count; ++i) heap[i] = payload(i);
  heap[1] = count - 1;
  heap[count - 1] = 1;
  heap[page - 1] = null;
  heap[page + 1] = page - 1;
  for (auto const field : {std::size_t{1}, count - 1, page - 1, page + 1})
    heap.pointer(static_cast<offset>(field));
  std::vector<word> const original(heap.data(), heap.data() + count);
  std::vector<offset> roots{1, static_cast<offset>(count - 1), null};
  auto const cycle = capacity / std::gcd(capacity, heap.reserved());
  for (std::size_t round = 0; round < cycle + 3; ++round) {
    auto const previous_start = heap.start();
    heap.clear_marks();
    heap.mark(1, count - 1);
    heap.compact(std::span<offset>{roots});
    check(heap.capacity() == capacity && heap.used() == count,
          "fully live parallel waves retain the complete usable prefix");
    check(heap.start() == (previous_start + capacity - heap.reserved()) % capacity,
          "fully live parallel waves rotate beyond a complete ring cycle");
    check(roots == std::vector<offset>{1, static_cast<offset>(count - 1), null},
          "fully live forwarding carries head, tail and null roots");
    for (std::size_t i = 0; i < count; ++i)
      check(heap[i] == original[i], "fully live parallel waves preserve every word and edge");
  }
}

void mapping_resources_are_released() noexcept {
#if defined(_WIN32)
  DWORD before = 0, after = 0;
  check(::GetProcessHandleCount(::GetCurrentProcess(), &before), "query heap handle baseline");
  for (unsigned round = 0; round != 16; ++round) {
    void const * last_view;
    {
      heap_type heap{{.old = {.capacity = jam::units::pages{7}, .reserve = jam::units::pages{1}}, .young = {.capacity = jam::units::pages{7}, .reserve = jam::units::pages{1}}}};
      static_cast<void>(heap.allocate(15 * heap.page_words()));
      heap.clear_marks();
      heap.mark(1);
      heap.compact();
      last_view = heap.data();
    }
    MEMORY_BASIC_INFORMATION region{};
    check(::VirtualQuery(last_view, &region, sizeof(region)) && region.State == MEM_FREE,
          "heap destruction releases the final placeholder view");
  }
  check(::GetProcessHandleCount(::GetCurrentProcess(), &after) && before == after,
        "growth, shrink and destruction release every backing handle");
#endif
}
void excessive_capacity_throws_before_mapping() noexcept {
  using namespace jam;
  using namespace jam::units;
  for (bool young : {false, true}) {
    heap_options options;
    (young ? options.young : options.old).maximum = 17_GiB;
    bool caught = false;
    try {
      heap_type rejected{options};
    } catch (std::length_error const &) {
      caught = true;
    }
    check(caught, "either generation rejects more than 31 offset bits before allocation");
  }
}

void invalid_requests_leave_heap_unchanged() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  static_cast<void>(heap.allocate(67));
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  auto const capacity = heap.capacity();
  auto const used = heap.used();
  auto const start = heap.start();
  auto const data = heap.data();
  auto unchanged = [&]() noexcept {
    check(heap.capacity() == capacity && heap.used() == used
          && heap.start() == start && heap.data() == data,
          "rejected requests leave heap geometry and mapping unchanged");
    for (std::size_t i = 1; i < used; ++i)
      check(heap[i] == payload(i), "rejected requests leave existing data unchanged");
  };
  for (unsigned scenario = 0; scenario != 10; ++scenario) {
    char const code[]{static_cast<char>('0' + scenario), '\0'};
#if defined(_WIN32)
    auto const status = ::_spawnl(_P_WAIT, executable, executable, "--invalid-request", code,
                                static_cast<char const *>(nullptr));
    check(status == 3, "invalid requests terminate with the CRT abort status");
#else
    auto const child = ::fork();
    check(child != -1, "the invalid-request subprocess starts");
    if (child == 0) {
      rlimit const limit{0, 0};
      static_cast<void>(::setrlimit(RLIMIT_CORE, &limit));
      ::execl(executable, executable, "--invalid-request", code, static_cast<char *>(nullptr));
      ::_exit(127);
    }
    int status = 0;
    pid_t finished;
    do { finished = ::waitpid(child, &status, 0); } while (finished == -1 && errno == EINTR);
    check(finished == child, "the invalid-request subprocess finishes");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "invalid geometry, zero allocations and overflow abort in release builds");
#endif
    unchanged();
  }
}

void invalid_request(unsigned scenario) noexcept {
  if (scenario >= 7) {
    using namespace jam::units;
    volatile auto maximum = std::numeric_limits<std::size_t>::max();
    if (scenario == 7) static_cast<void>(bytes{maximum} + bytes{1});
    if (scenario == 8) static_cast<void>(space_cast<bytes>(kibibytes{maximum}));
    if (scenario == 9) static_cast<void>(space_cast<bytes>(space<double>{std::numeric_limits<double>::infinity()}));
    return;
  }
  struct geometry { std::size_t pages, reserve; };
  constexpr geometry invalid[]{{0, 1}, {1, 1}, {8, 0}, {4, 3},
    {std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1), 1}};
  if (scenario < 5) {
    volatile std::size_t capacity_pages = scenario == 4 ? std::numeric_limits<std::uint64_t>::max()
                                                     : invalid[scenario].pages;
    volatile std::size_t reserve = invalid[scenario].reserve;
    heap_type rejected{jam::heap_options{.old = {.capacity = jam::units::pages{capacity_pages}, .reserve = jam::units::pages{reserve}}, .young = {.capacity = jam::units::pages{capacity_pages}, .reserve = jam::units::pages{reserve}}}};
  } else {
    heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}}};
    volatile std::size_t words = scenario == 5 ? 0 : std::numeric_limits<std::size_t>::max();
    static_cast<void>(heap.allocate(words));
  }
}

void concurrent_range_marks_preserve_the_union() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  auto const count = 4 * heap.page_words() + 137;
  static_cast<void>(heap.allocate(count - 1));
  for (std::size_t i = 1; i < count; ++i) heap[i] = payload(i);
  std::vector<unsigned char> live(count, 0), pointers(count, 0);
  constexpr std::size_t marker_count = 4;
  constexpr std::size_t begins[]{1, 4, 11, 31};
  constexpr std::size_t widths[]{5, 9, 7, 29};
  auto add_range = [&](std::size_t first, std::size_t width) noexcept {
    if (first < count) std::fill_n(live.begin() + first, std::min(width, count - first), 1);
  };
  auto add_pointer = [&](std::size_t field) noexcept {
    pointers[field] = 1;
    heap[field] = field % 3 == 0 ? null : static_cast<offset>(field);
  };
  for (std::size_t base = 0; base < count; base += 64) {
    for (std::size_t worker = 0; worker < marker_count; ++worker) {
      auto const first = base + begins[worker];
      add_range(first, widths[worker]);
      if (first < count) add_pointer(first);
    }
    add_range(base + 61, 6);
    if (base + 4 < count) add_pointer(base + 4);
  }
  std::vector<offset> roots{1, 4, 12, static_cast<offset>(((count - 1) / 64) * 64 + 1), null};
  auto const result = oracle(heap, live, pointers, roots);
  heap.clear_marks();
  std::barrier gate{static_cast<std::ptrdiff_t>(marker_count)};
  std::vector<std::jthread> markers;
  markers.reserve(marker_count);
  for (std::size_t worker = 0; worker < marker_count; ++worker)
    markers.emplace_back([&, worker]() noexcept {
      for (std::size_t base = 0; base < count; base += 64) {
        gate.arrive_and_wait();
        auto const first = base + begins[worker];
        for (unsigned repeat = 0; repeat != 3; ++repeat) {
          if (first < count) {
            heap.mark(static_cast<offset>(first), std::min(widths[worker], count - first));
            heap.pointer(static_cast<offset>(first));
          }
          if (worker == 0 && base + 61 < count)
            heap.mark(static_cast<offset>(base + 61), std::min(std::size_t{6}, count - base - 61));
          if (base + 4 < count) heap.pointer(static_cast<offset>(base + 4));
        }
      }
    });
  markers.clear(); // Join every marker before observing metadata or starting collection.
  for (std::size_t i = 0; i < count; ++i) {
    auto const & block = heap.blocks()[i / heap_type::block_words];
    auto const bit = word{1} << (i % heap_type::block_words);
    check(((block.live & bit) != 0) == (live[i] != 0), "parallel overlapping marks lose no live bits");
    check(((block.pointers & (word{1} << (2 * (i % heap_type::block_words)))) != 0) == (pointers[i] != 0), "parallel pointer declarations lose no bits");
  }
  heap.compact(std::span<offset>{roots});
  verify(heap, result, roots);
}

void parallel_claim_traverses_each_cyclic_record_once() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{2}}, .workers = 4}};
  std::vector<std::size_t> const widths{3, 67, heap.page_words() + 65, 5, 64, 73, 193, 4};
  std::vector<offset> records;
  for (auto const width : widths) {
    static_cast<void>(heap.allocate(5));
    records.push_back(heap.allocate(width));
  }
  for (std::size_t i = 1; i < heap.used(); ++i) heap[i] = payload(i);
  std::vector<unsigned char> live(heap.used(), 0), pointers(heap.used(), 0);
  for (std::size_t i = 0; i < records.size(); ++i) {
    auto const start = records[i];
    heap[start] = records[(i + 1) % records.size()];
    heap[start + 1] = start;
    heap[start + 2] = null;
    std::fill_n(live.begin() + static_cast<std::size_t>(start), widths[i], 1);
    std::fill_n(pointers.begin() + static_cast<std::size_t>(start), 3, 1);
  }
  std::vector<offset> roots{records[0], records[3], static_cast<offset>(records.back() + widths.back() - 1), null};
  auto const result = oracle(heap, live, pointers, roots);
  heap.clear_marks();
  std::vector<std::atomic<unsigned>> winners(records.size());
  std::vector<std::size_t> indices(heap.used(), records.size());
  for (std::size_t i = 0; i < records.size(); ++i)
    indices[static_cast<std::size_t>(records[i])] = i;
  auto const visit = [&](auto const & self, std::size_t index) noexcept -> void {
    auto const start = records[index];
    if (!heap.claim(start, widths[index])) return;
    winners[index].fetch_add(1, std::memory_order_relaxed);
    heap.pointer(start);
    heap.pointer(start + 1);
    heap.pointer(start + 2);
    self(self, indices[static_cast<std::size_t>(heap[start])]);
    self(self, indices[static_cast<std::size_t>(heap[start + 1])]);
  };
  constexpr std::size_t marker_count = 4;
  std::barrier gate{static_cast<std::ptrdiff_t>(marker_count)};
  std::vector<std::jthread> markers;
  markers.reserve(marker_count);
  for (std::size_t worker = 0; worker < marker_count; ++worker)
    markers.emplace_back([&, worker]() noexcept {
      gate.arrive_and_wait();
      visit(visit, (worker * 2) % records.size());
    });
  markers.clear();
  for (std::size_t i = 0; i < records.size(); ++i) {
    check(winners[i].load(std::memory_order_relaxed) == 1, "exactly one marker owns each cyclic record");
    for (std::size_t j = 0; j < widths[i]; ++j) {
      auto const offset = static_cast<std::size_t>(records[i]) + j;
      check((heap.blocks()[offset / heap_type::block_words].live & (word{1} << (offset % heap_type::block_words))) != 0,
            "the winning claim marks the whole variable-width record");
    }
  }
  heap.compact(std::span<offset>{roots});
  verify(heap, result, roots);
}

void empty_collection_preserves_null_roots() noexcept {
  heap_type heap{jam::heap_options{.old = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}, .young = {.capacity = jam::units::pages{8}, .reserve = jam::units::pages{1}}}};
  static_cast<void>(heap.allocate(73));
  std::vector<unsigned char> live(heap.used(), 0), pointers(heap.used(), 0);
  std::vector<offset> roots{null, null};
  compact(heap, live, pointers, roots);
  check(heap.used() == 1 && heap.capacity() == 4 * heap.page_words(), "an empty heap shrinks one step");
}

} // namespace

int main(int argc, char * argv[]) noexcept {
#if defined(_WIN32)
  ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  static_cast<void>(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE));
  static_cast<void>(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR));
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  executable = argv[0];
  if (argc == 3 && std::string_view{argv[1]} == "--invalid-request") {
    invalid_request(static_cast<unsigned>(argv[2][0] - '0'));
    return 0;
  }
  check_compactors();
  byte_literals();
  alias_and_layout();
  allocation_growth_preserves_reserve();
  cyclic_edges_and_roots();
  straddling_records_and_ring_wrap();
  parallel_matches_scalar();
  growth_of_a_wrapped_view();
  growth_of_a_wrapped_view(7);
  shrinking_preserves_records_and_gap();
  shrinking_preserves_records_and_gap(15);
  empty_collection_preserves_null_roots();
  full_parallel_waves_preserve_every_word();
  full_parallel_waves_preserve_every_word(7);
  mapping_resources_are_released();
  excessive_capacity_throws_before_mapping();
  invalid_requests_leave_heap_unchanged();
  concurrent_range_marks_preserve_the_union();
  parallel_claim_traverses_each_cyclic_record_once();
  std::puts("16 heap checks passed");
  return 0;
}
