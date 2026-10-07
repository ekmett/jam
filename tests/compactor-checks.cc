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
  constexpr std::size_t blocks = 9, cells = blocks * 32;
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
    // The reference retains groups explicitly; it does not call the production
    // dilation helper. Layout zero also checks the null alignment-table fast path.
    for (unsigned layout = 0; layout != 5; ++layout)
    for (unsigned pattern = 0; pattern != 256; ++pattern) {
      std::array<heap_block, blocks> metadata{}, reference{};
      alignas(4) std::array<std::uint8_t, (blocks + 7) / 8 * 4> alignment{};
      auto const * flags = layout ? alignment.data() : nullptr;
      auto expand = [](std::uint32_t bits, unsigned k) noexcept {
        std::uint32_t result = 0;
        auto const width = 1u << k;
        auto const end = static_cast<unsigned>(std::bit_width(bits));
        for (unsigned begin = 0; begin < end; begin += width) {
          auto const group = ((1u << width) - 1) << begin;
          if (bits & group) result |= ((1u << std::min(width, end - begin)) - 1) << begin;
        }
        return result;
      };
      std::vector<std::uint32_t> live;
      std::uint32_t destination = 0;
      for (unsigned block = 0; block != blocks; ++block) {
        auto const mask = std::rotl(pattern * 0x01010101u, static_cast<int>(block));
        metadata[block].live = mask | (block == 0 ? 1u : 0u);
        auto const k = layout ? (block + layout - 1) % 4 : 0;
        alignment[block / 2] |= static_cast<std::uint8_t>(((1u << k) - 1) << ((block % 2) * 4));
        destination = (destination + (1u << k) - 1) & ~((1u << k) - 1);
        metadata[block].destination = destination;
        reference[block] = {expand(metadata[block].live, k), destination, 0};
        destination += std::popcount(reference[block].live);
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
      for (unsigned block = 0; block != blocks; ++block)
        reference[block].pointers = metadata[block].pointers;
      jam::detail::forwarding_tables own_tables{metadata.data(), nullptr, flags, nullptr};
      // Unaligned destinations and untouched prefix/suffix expose overstores.
      std::array<std::uint64_t, cells + 2> expected, actual;
      expected.fill(sentinel); actual.fill(sentinel);
      baseline.move(source.data(), expected.data() + 1, reference.data(), 0, blocks);
      variant.move(source.data(), actual.data() + 1, metadata.data(), 0, blocks, own_tables, flags);
      check(actual == expected);
      check(actual[0] == sentinel);
      for (auto i = destination + 1; i != actual.size(); ++i) check(actual[i] == sentinel);
      // Both tables contribute to full forwarding; minor forwarding must leave
      // old offsets unchanged while retaining young's generation bit.
      auto mixed = source;
      for (unsigned cell = 0; cell != cells; ++cell)
        for (unsigned slot = 0; slot != 2; ++slot) {
          auto const shift = slot * 32;
          auto const value = static_cast<std::uint32_t>(mixed[cell] >> shift);
          if (value && ((cell + slot) & 1)
              && (metadata[cell / 32].pointers & (std::uint64_t{1} << ((cell % 32) * 2 + slot))))
            mixed[cell] |= std::uint64_t{0x80000000u} << shift;
        }
      for (bool minor : {false, true}) {
        auto young = metadata, young_reference = reference;
        auto young_alignment = alignment;
        auto const * young_flags = layout ? young_alignment.data() : nullptr;
        std::uint32_t next = minor ? 0x80000000u : 24u;
        for (unsigned block = 0; block != blocks; ++block) {
          auto const k = layout ? (block + layout) % 4 : 0;
          auto const shift = (block % 2) * 4;
          young_alignment[block / 2] = static_cast<std::uint8_t>((young_alignment[block / 2] & ~(15u << shift))
              | (((1u << k) - 1) << shift));
          next = (next + (1u << k) - 1) & ~((1u << k) - 1);
          young[block].destination = young_reference[block].destination = next;
          young_reference[block].live = expand(young[block].live, k);
          next += std::popcount(young_reference[block].live);
        }
        jam::detail::forwarding_tables tables{minor ? nullptr : metadata.data(), young.data(),
          minor ? nullptr : flags, young_flags};
        jam::detail::forwarding_tables reference_tables{minor ? nullptr : reference.data(), young_reference.data()};
        expected.fill(sentinel); actual.fill(sentinel);
        baseline.move(mixed.data(), expected.data() + 1, reference.data(), 0, blocks, reference_tables);
        variant.move(mixed.data(), actual.data() + 1, metadata.data(), 0, blocks, tables, flags);
        check(actual == expected);
        // Weak edges use the same slot mask. Independently compute forwarding
        // after some targets die, including cells still retained by dilation.
        auto weak_old = metadata, weak_young = young;
        for (unsigned block = 0; block != blocks; ++block) {
          weak_old[block].live &= 0x55555555u;
          weak_young[block].live &= 0xaaaaaaaau;
        }
        jam::detail::forwarding_tables weak_tables{minor ? nullptr : weak_old.data(), weak_young.data(),
          minor ? nullptr : flags, young_flags};
        auto weak_forward = [&](std::uint32_t value) noexcept {
          if (!value || (minor && !(value >> 31))) return value;
          auto const block = (value & 0x7fffffffu) >> 5;
          auto const bit = 1u << (value & 31);
          auto const & target = value >> 31 ? weak_young[block] : weak_old[block];
          if (!(target.live & bit)) return std::uint32_t{0};
          auto const k = !layout ? 0 : (block + layout - (value >> 31 ? 0 : 1)) % 4;
          return target.destination + std::popcount(expand(target.live, k) & (bit - 1));
        };
        expected.fill(sentinel); actual.fill(sentinel);
        for (unsigned block = 0; block != blocks; ++block) {
          auto out = reference[block].destination + 1;
          for (unsigned cell = 0; cell != 32; ++cell) {
            if (!(reference[block].live & (1u << cell))) continue;
            auto value = mixed[block * 32 + cell];
            for (unsigned slot = 0; slot != 2; ++slot) {
              if (!(metadata[block].pointers & (std::uint64_t{1} << (cell * 2 + slot)))) continue;
              auto const shift = slot * 32;
              auto const bits = weak_forward(static_cast<std::uint32_t>(value >> shift));
              value = (value & ~(std::uint64_t{0xffffffffu} << shift)) | (std::uint64_t{bits} << shift);
            }
            expected[out++] = value;
          }
        }
        variant.move(mixed.data(), actual.data() + 1, metadata.data(), 0, blocks, weak_tables, flags);
        check(actual == expected);
      }
      // Loading several chunks ahead must still permit leftward in-place moves.
      auto overlapping = source;
      auto expected_overlap = source;
      baseline.move(expected_overlap.data(), expected_overlap.data(), reference.data(), 0, blocks);
      variant.move(overlapping.data(), overlapping.data(), metadata.data(), 0, blocks, own_tables, flags);
      check(overlapping == expected_overlap);
      auto packed_expected = metadata, packed_actual = metadata;
      for (auto & item : packed_expected) item.pointers = 0;
      for (auto & item : packed_actual) item.pointers = 0;
      baseline.pack(reference.data(), packed_expected.data(), blocks);
      variant.pack(metadata.data(), packed_actual.data(), blocks, flags);
      for (std::size_t i = 0; i != blocks; ++i) {
        check(packed_actual[i].pointers == packed_expected[i].pointers);
        check(packed_actual[i].live == metadata[i].live);
        check(packed_actual[i].destination == metadata[i].destination);
      }
      auto packed_in_place = metadata;
      variant.pack(packed_in_place.data(), packed_in_place.data(), blocks, flags);
      for (std::size_t i = 0; i != blocks; ++i) {
        check(packed_in_place[i].pointers == packed_expected[i].pointers);
        check(packed_in_place[i].live == metadata[i].live);
        check(packed_in_place[i].destination == metadata[i].destination);
      }
      // The pointer-free path must pack every mask without interpreting data.
      auto pointer_free = metadata, reference_free = reference;
      for (auto & item : pointer_free) item.pointers = 0;
      for (auto & item : reference_free) item.pointers = 0;
      expected.fill(sentinel); actual.fill(sentinel);
      baseline.move(source.data(), expected.data() + 1, reference_free.data(), 0, blocks);
      variant.move(source.data(), actual.data() + 1, pointer_free.data(), 0, blocks, own_tables, flags);
      check(actual == expected);
    }
    // Inactive lanes contain deliberately invalid offsets. Neither ordinary
    // data nor a dead cell with a stale pointer declaration may be gathered.
    std::array<heap_block, 1> masked_metadata{{{0x55555555u, 0, ~std::uint64_t{0}}}};
    std::array<std::uint64_t, 32> masked_source;
    masked_source.fill(0xffffffffffffffffULL);
    for (unsigned cell = 0; cell != 32; cell += 2) {
      masked_source[cell] = std::uint64_t{0xfedcba98u} << 32; // Null pointer + data.
      masked_metadata[0].pointers &= ~(std::uint64_t{1} << (cell * 2 + 1));
    }
    std::array<std::uint64_t, 34> masked_expected, masked_actual;
    masked_expected.fill(sentinel); masked_actual.fill(sentinel);
    baseline.move(masked_source.data(), masked_expected.data() + 1, masked_metadata.data(), 0, 1);
    variant.move(masked_source.data(), masked_actual.data() + 1, masked_metadata.data(), 0, 1);
    check(masked_actual == masked_expected);
    // Interior dead cells are retained, but their stale declarations are still
    // data. The dead suffix is dropped. Exercise the nibble table's final dword.
    alignas(4) std::array<std::uint8_t, 4> masked_alignment{3, 0, 0, 0};
    auto masked_reference = masked_metadata;
    masked_reference[0].live = 0x7fffffffu;
    masked_reference[0].pointers &= 0x1111111111111111ULL;
    masked_expected.fill(sentinel); masked_actual.fill(sentinel);
    baseline.move(masked_source.data(), masked_expected.data() + 1, masked_reference.data(), 0, 1);
    variant.move(masked_source.data(), masked_actual.data() + 1, masked_metadata.data(), 0, 1,
        {masked_metadata.data(), nullptr, masked_alignment.data(), nullptr}, masked_alignment.data());
    check(masked_actual == masked_expected);
    std::printf("compactor %s: 1280 mask/alignment/weak-forwarding/store checks passed\n", variant.name);
  }
}
