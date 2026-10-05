// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

module;
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

module jam;
import :compact;
import native;

extern "C" void check_compactors() noexcept {
  using jam::detail::heap_block;
  constexpr std::size_t blocks = 8, cells = blocks * 32;
  constexpr std::uint64_t sentinel = 0xa5a5a5a5a5a5a5a5ULL;
  auto const cpu = native::observe_cpu();
  auto const variants = jam::detail::compactor_variants();
  auto const baseline = variants.back();
  auto check = [](bool valid) noexcept { if (!valid) std::abort(); };
  for (auto const & variant : variants) {
    if (!native::classify_isa(cpu, variant.requirement).admitted()) {
      std::printf("compactor %s: not supported by this CPU/OS\n", variant.name);
      continue;
    }
    for (unsigned pattern = 0; pattern != 256; ++pattern) {
      std::array<heap_block, blocks> metadata{};
      std::vector<std::uint32_t> live;
      std::uint32_t destination = 0;
      for (unsigned block = 0; block != blocks; ++block) {
        auto const mask = std::rotl(pattern * 0x01010101u, static_cast<int>(block));
        metadata[block].live = mask | (block == 0 ? 1u : 0u);
        metadata[block].destination = destination;
        destination += std::popcount(metadata[block].live);
        for (unsigned bit = 0; bit != 32; ++bit)
          if (metadata[block].live & (1u << bit)) live.push_back(block * 32 + bit);
      }
      std::array<std::uint64_t, cells> source;
      for (unsigned cell = 0; cell != cells; ++cell) {
        std::uint32_t fields[2]{0xdead0000u + cell, 0xbeef0000u + cell};
        if (metadata[cell / 32].live & (1u << (cell % 32))) {
          for (unsigned slot = 0; slot != 2; ++slot)
            if ((cell + slot + pattern) % 3) {
              fields[slot] = live[(cell * 17 + slot * 3) % live.size()];
              metadata[cell / 32].pointers |= std::uint64_t{1} << ((cell % 32) * 2 + slot);
            }
        }
        source[cell] = std::uint64_t{fields[0]} | (std::uint64_t{fields[1]} << 32);
      }
      // Unaligned destinations and untouched prefix/suffix expose overstores.
      std::array<std::uint64_t, cells + 2> expected, actual;
      expected.fill(sentinel); actual.fill(sentinel);
      baseline.move(source.data(), expected.data() + 1, metadata.data(), 0, blocks);
      variant.move(source.data(), actual.data() + 1, metadata.data(), 0, blocks);
      check(actual == expected);
      check(actual[0] == sentinel);
      for (auto i = destination + 1; i != actual.size(); ++i) check(actual[i] == sentinel);
      auto packed_expected = metadata, packed_actual = metadata;
      auto const new_blocks = (destination + 31) / 32;
      baseline.pack(packed_expected.data(), blocks, new_blocks);
      variant.pack(packed_actual.data(), blocks, new_blocks);
      for (std::size_t i = 0; i != blocks; ++i) {
        check(packed_actual[i].pointers == packed_expected[i].pointers);
        check(packed_actual[i].live == metadata[i].live);
        check(packed_actual[i].destination == metadata[i].destination);
      }
    }
    std::printf("compactor %s: 256 mask/forwarding/store checks passed\n", variant.name);
  }
}
