// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

import jam;

namespace {

[[noreturn]] void failure(std::string_view message) noexcept {
  std::fprintf(stderr, "heap-bench: %.*s\n", static_cast<int>(message.size()), message.data());
  std::exit(1);
}

template<class T> T number(std::string_view text) noexcept {
  T value{};
  auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) failure("invalid number");
  return value;
}

struct options {
  std::size_t bytes = 16 * 1024 * 1024;
  std::size_t reserve = 32;
  std::vector<std::size_t> reserve_list;
  std::size_t workers = 1;
  std::size_t repeats = 7;
  unsigned alignment = 8;
  std::size_t cluster_words = 32;
  double live = 0.75;
  double pointers = 0.125;
};

std::size_t byte_count(std::string text) noexcept {
  std::size_t multiplier = 1;
  for (auto const & unit : {std::string{"KiB"}, std::string{"MiB"}, std::string{"GiB"}})
    if (text.ends_with(unit)) {
      multiplier = unit == "KiB" ? 1024 : unit == "MiB" ? 1024 * 1024 : 1024 * 1024 * 1024;
      text.resize(text.size() - unit.size());
      break;
    }
  auto const value = number<std::size_t>(text);
  if (value > std::numeric_limits<std::size_t>::max() / multiplier)
    failure("invalid byte count");
  return static_cast<std::size_t>(value) * multiplier;
}

std::vector<std::size_t> reserve_counts(std::string_view text) noexcept {
  std::vector<std::size_t> result;
  while (true) {
    auto const comma = text.find(',');
    auto const item = text.substr(0, comma);
    if (item.empty()) failure("empty reserve-list item");
    auto const count = byte_count(std::string{item});
    if (!count || std::ranges::find(result, count) != result.end())
      failure("reserve-list items must be positive and distinct");
    result.push_back(count);
    if (comma == std::string_view::npos) return result;
    text.remove_prefix(comma + 1);
  }
}

options parse(int argc, char ** argv) noexcept {
  options result;
  for (int i = 1; i < argc; ++i) {
    std::string_view const key = argv[i];
    if (key == "--help") {
      std::cout << "heap-bench --bytes 16MiB --reserve 32\n"
                   "           --workers 1 --live 0.75 --pointers 0.125 --repeats 7 --alignment 8 --cluster-words 32\n"
                   "           --reserve-list 32,64,128 interleaves trials; each heap owns its pool\n";
      std::exit(0);
    }
    if (++i == argc) failure("missing option value");
    std::string const value = argv[i];
    if (key == "--bytes") result.bytes = byte_count(value);
    else if (key == "--reserve") result.reserve = byte_count(value);
    else if (key == "--reserve-list") result.reserve_list = reserve_counts(value);
    else if (key == "--workers") result.workers = byte_count(value);
    else if (key == "--repeats") result.repeats = byte_count(value);
    else if (key == "--cluster-words") result.cluster_words = byte_count(value);
    else if (key == "--alignment") result.alignment = static_cast<unsigned>(byte_count(value));
    else if (key == "--live") result.live = number<double>(value);
    else if (key == "--pointers" || key == "--ptr-density") result.pointers = number<double>(value);
    else failure("unknown option: " + std::string(key));
  }
  if (!result.reserve || !result.workers || !result.repeats || result.repeats > 1000
      || result.bytes < 256 || !(result.live > 0 && result.live <= 1)
      || !(result.pointers >= 0 && result.pointers <= 1)
      || (result.alignment != 8 && result.alignment != 16 && result.alignment != 32 && result.alignment != 64)
      || !std::has_single_bit(result.cluster_words) || result.cluster_words < result.alignment / 8
      || result.cluster_words > result.bytes / 8)
    failure("invalid benchmark geometry, fractions, or alignment");
  return result;
}

std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

std::uint64_t checksum(std::uint64_t previous, std::uint64_t word) noexcept {
  return (previous ^ word) * 0x9e3779b97f4a7c15ULL;
}

struct sample {
  double ns;
  std::size_t input_bytes, live_bytes, pointer_fields, capacity_before, capacity_after, page_bytes;
};

sample trial(options const & config) noexcept {
  using H = jam::heap;
  using offset = typename H::offset;
  auto const cluster_words = config.cluster_words;
  auto const page_bytes = static_cast<std::size_t>(::getpagesize());
  auto const words = (config.bytes / 8 / cluster_words) * cluster_words;
  auto const pages = (words * 8 + page_bytes - 1) / page_bytes;
  auto const capacity_pages = std::max(pages + config.reserve, 2 * config.reserve);
  H heap{jam::heap_options{.capacity = capacity_pages * page_bytes, .reserve = config.reserve * page_bytes,
                      .workers = config.workers}};
  auto const first = heap.allocate(words, config.alignment);
  // Pre-touch backing through the first alias, including the copy reserve.
  // The second alias may still incur page-table faults; pages are not locked.
  auto * const touch = static_cast<volatile typename H::word *>(heap.data());
  for (std::size_t i = 0; i < heap.capacity(); i += heap.page_words()) touch[i] = 0;
  auto const clusters = words / cluster_words;
  std::vector<offset> destinations(clusters, H::null), live_starts;
  live_starts.reserve(clusters);
  auto const live_threshold = static_cast<std::uint64_t>(config.live * 9007199254740992.0);
  auto const pointer_threshold = static_cast<std::uint64_t>(config.pointers * 9007199254740992.0);
  offset output_words = first;
  for (std::size_t i = 0; i < clusters; ++i)
    if ((mix(i + 17) >> 11) < live_threshold || i == 0) {
      destinations[i] = output_words;
      live_starts.push_back(static_cast<offset>(first + i * cluster_words));
      output_words = static_cast<offset>(output_words + cluster_words);
    }
  for (std::size_t i = 0; i < words; ++i) heap[first + i] = mix(i + 0x12345);
  auto translate = [&](offset old) noexcept -> offset {
    return old == H::null ? H::null : static_cast<offset>(destinations[(old - first) / cluster_words] + (old - first) % cluster_words);
  };
  auto pointer_at = [&](std::size_t i) noexcept { return (mix(i + 0x54321) >> 11) < pointer_threshold; };
  auto slot_at = [&](std::size_t i) noexcept -> unsigned {
    return static_cast<unsigned>(mix(i + 91) & 1);
  };
  auto target_at = [&](std::size_t i) noexcept -> offset {
    auto const code = mix(i + 123);
    if ((code & 15) == 0) return H::null;
    return static_cast<offset>(live_starts[code % live_starts.size()] + ((code >> 32) % cluster_words));
  };
  std::uint64_t expected_checksum = 0xabcdef;
  for (std::size_t i = 0; i < first; ++i) expected_checksum = checksum(expected_checksum, 0);
  std::size_t pointer_fields = 0;
  heap.clear_marks();
  for (auto const start : live_starts) {
    heap.mark(start, cluster_words, config.alignment);
    for (std::size_t j = 0; j < cluster_words; ++j) {
      auto const index = static_cast<std::size_t>(start) + j;
      auto expected = heap[index];
      if (pointer_at(index)) {
        auto const slot = slot_at(index);
        auto const target = target_at(index);
        heap.set_field(static_cast<offset>(index), target, slot);
        heap.pointer(static_cast<offset>(index), slot);
        expected = heap[index];
        auto const forwarded = translate(target);
        auto const shift = slot * 32;
        expected = (expected & ~(std::uint64_t{0xffffffff} << shift)) | (std::uint64_t{forwarded} << shift);
        ++pointer_fields;
      }
      expected_checksum = checksum(expected_checksum, expected);
    }
  }
  std::vector<offset> roots{live_starts.front(), static_cast<offset>(live_starts.back() + cluster_words - 1), H::null};
  auto expected_roots = roots;
  for (auto & root : expected_roots) root = translate(root);
  auto const before = heap.capacity();
  auto const begin = std::chrono::steady_clock::now();
  heap.compact(std::span<offset>{roots});
  auto const end = std::chrono::steady_clock::now();
  if (heap.used() != output_words || roots != expected_roots)
    failure("benchmark packed size/root verification failed");
  std::uint64_t actual_checksum = 0xabcdef;
  for (std::size_t i = 0; i < heap.used(); ++i) actual_checksum = checksum(actual_checksum, heap[i]);
  if (actual_checksum != expected_checksum) failure("benchmark payload checksum failed");
  for (auto const start : live_starts)
    for (std::size_t j = 0; j < cluster_words; ++j) {
      auto const index = static_cast<std::size_t>(start) + j;
      if (pointer_at(index)
          && heap.field(static_cast<offset>(destinations[(start - first) / cluster_words] + j), slot_at(index)) != translate(target_at(index)))
        failure("benchmark pointer graph verification failed");
    }
  return {std::chrono::duration<double, std::nano>(end - begin).count(), words * 8,
          static_cast<std::size_t>(output_words) * 8, pointer_fields, before * 8, heap.capacity() * 8, page_bytes};
}

void print_header() noexcept {
  std::cout << "alignment,cluster_words,resident,scheduler,arch,compiler,build,bmi2,avx2,workers,reserve_pages,page_bytes,requested_bytes,input_bytes,live_bytes,copied_bytes,pointer_fields,capacity_before,capacity_after,live_fraction,pointer_density,repeats,median_ns,p10_ns,p90_ns,min_ns,max_ns,samples_ns\n";
}

void print_summary(options const & config, std::size_t workers, sample const & geometry, std::vector<double> const & times) noexcept {
  auto sorted = times;
  std::sort(sorted.begin(), sorted.end());
  auto const median = sorted.size() % 2 ? sorted[sorted.size() / 2]
    : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
#if defined(__aarch64__) || defined(__arm64__)
  constexpr auto arch = "aarch64";
#else
  constexpr auto arch = "x86_64";
#endif
  constexpr auto scheduler = "window";
#ifdef __BMI2__
  constexpr int bmi2 = 1;
#else
  constexpr int bmi2 = 0;
#endif
#ifdef __AVX2__
  constexpr int avx2 = 1;
#else
  constexpr int avx2 = 0;
#endif
#ifdef NDEBUG
  constexpr auto build = "release";
#else
  constexpr auto build = "debug";
#endif
  std::cout << std::defaultfloat << std::setprecision(6)
            << config.alignment << ',' << config.cluster_words << ",1,"
            << scheduler << ',' << arch << ",clang" << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__
            << ',' << build << ',' << bmi2 << ',' << avx2 << ',' << workers << ',' << config.reserve << ','
            << geometry.page_bytes << ',' << config.bytes << ',' << geometry.input_bytes << ',' << geometry.live_bytes << ','
            << geometry.live_bytes << ',' << geometry.pointer_fields << ',' << geometry.capacity_before << ',' << geometry.capacity_after
            << ',' << config.live << ',' << config.pointers << ',' << config.repeats << ',' << std::fixed << std::setprecision(0)
            << median << ',' << sorted[(sorted.size() - 1) / 10] << ',' << sorted[(sorted.size() - 1) * 9 / 10]
            << ',' << sorted.front() << ',' << sorted.back() << ",\"";
  for (std::size_t i = 0; i < times.size(); ++i) { if (i) std::cout << ';'; std::cout << times[i]; }
  std::cout << "\"\n";
}

void run(options const & config) noexcept {
  auto const reserves = config.reserve_list.empty() ? std::vector<std::size_t>{config.reserve} : config.reserve_list;
  std::vector<std::vector<double>> times(reserves.size());
  std::vector<sample> geometries(reserves.size());
  std::vector<std::size_t> order(reserves.size());
  auto current = config;
  for (std::size_t i = 0; i < reserves.size(); ++i) {
    current.reserve = reserves[i];
    times[i].reserve(config.repeats);
    static_cast<void>(trial(current)); // One verified warm-up per N.
  }
  for (std::size_t round = 0; round < config.repeats; ++round) {
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    // Fixed seed and explicit Fisher-Yates keep the interleaving reproducible across hosts.
    auto random = mix(round + 0x6a616d);
    for (std::size_t i = order.size(); i > 1; --i) {
      random = mix(random);
      std::swap(order[i - 1], order[random % i]);
    }
    for (auto const i : order) {
      current.reserve = reserves[i];
      geometries[i] = trial(current);
      times[i].push_back(geometries[i].ns);
    }
  }
  print_header();
  for (std::size_t i = 0; i < reserves.size(); ++i) {
    current.reserve = reserves[i];
    print_summary(current, config.workers, geometries[i], times[i]);
  }
}

}

int main(int argc, char ** argv) noexcept {
  auto const config = parse(argc, argv);
  run(config);
}
