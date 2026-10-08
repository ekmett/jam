// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

/// \file
/// \brief Runtime-selected SIMD compaction kernels.

module;
#include <native/config.h>
#include <native/targets.h>
#include <hint.h>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if NATIVE_HOST_X86
#include <immintrin.h>
#define NATIVE_TARGET_jam_avx2 "avx2,fma,bmi2"
#define NATIVE_TARGET_jam_avx512 "avx2,fma,avx512f,avx512dq,avx512bw,avx512vl,bmi2"
#define NATIVE_TARGET_jam_avx512_popcnt "avx2,fma,avx512f,avx512dq,avx512bw,avx512vl,avx512vpopcntdq,bmi2"

// Temporary bridge until native exposes per-lane variable shifts.
// Intrinsic calls are owned by the global module fragment.
namespace {
hint_inline hint_const hint_target("avx2")
__m256i shift_left(__m256i value, __m256i counts) noexcept {
  return _mm256_sllv_epi32(value, counts);
}
hint_inline hint_const hint_target("avx512f")
__m512i shift_left(__m512i value, __m512i counts) noexcept {
  return _mm512_sllv_epi32(value, counts);
}
hint_inline hint_const hint_target("avx2")
__m256i shift_right(__m256i value, __m256i counts) noexcept {
  return _mm256_srlv_epi32(value, counts);
}
hint_inline hint_const hint_target("avx512f")
__m512i shift_right(__m512i value, __m512i counts) noexcept {
  return _mm512_srlv_epi32(value, counts);
}
// Native's AVX2 compress_store currently copies through a stack buffer.
hint_inline hint_target("avx2")
void store_prefix(std::uint32_t * target, __m256i value, unsigned count) noexcept {
  auto const mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(count),
                                     _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
  _mm256_maskstore_epi32(reinterpret_cast<int *>(target), mask, value);
}
}
#endif

module jam.packed;
import native.simd;
import native.features;
#if NATIVE_HOST_X86
import native.x86.bmi2;
import native.x86.memory;
import native.x86.vpopcntdq;
#endif

namespace jam::detail {
using word = std::uint64_t;
using bitmap = std::uint32_t;

constexpr word pointer_cells(bitmap live) noexcept {
  word x = live;
  x = (x | (x << 16)) & 0x0000ffff0000ffffULL;
  x = (x | (x << 8)) & 0x00ff00ff00ff00ffULL;
  x = (x | (x << 4)) & 0x0f0f0f0f0f0f0f0fULL;
  x = (x | (x << 2)) & 0x3333333333333333ULL;
  x = (x | (x << 1)) & 0x5555555555555555ULL;
  return x | (x << 1);
}

constexpr bitmap forward(forwarding_tables tables, bitmap value) noexcept {
  if (!value) return 0;
  auto const * metadata = value >> 31 ? tables.young : tables.old;
  if (!metadata) { assert(!(value >> 31)); return value; }
  auto const index = (value & 0x7fffffffu) >> 5;
  auto const & item = metadata[index];
  auto const * alignment = value >> 31 ? tables.young_alignment : tables.old_alignment;
  auto const bit = bitmap{1} << (value & 31);
  if (!(item.live & bit)) return 0;
  return item.destination + std::popcount(retained(item, alignment, index) & (bit - 1));
}

// Four cells per lookup; one 4 KiB table shared by the portable variants.
inline constexpr auto pack_table = [] consteval {
  std::array<std::array<std::uint8_t, 256>, 16> result{};
  for (unsigned live = 0; live != 16; ++live)
    for (unsigned value = 0; value != 256; ++value) {
      unsigned packed = 0, position = 0;
      for (unsigned cell = 0; cell != 4; ++cell) if (live & (1u << cell)) {
        packed |= ((value >> (cell * 2)) & 3) << position;
        position += 2;
      }
      result[live][value] = static_cast<std::uint8_t>(packed);
    }
  return result;
}();

constexpr word portable_pack(heap_block const & item) noexcept {
  word result = 0;
  unsigned position = 0;
  for (unsigned i = 0; i != 8; ++i) {
    auto const live = (item.live >> (4 * i)) & 15;
    result |= word{pack_table[live][(item.pointers >> (8 * i)) & 255]} << position;
    position += 2 * std::popcount(live);
  }
  return result;
}

void baseline_move(word const * source, word * target, heap_block const * metadata,
                   std::size_t begin, std::size_t end, forwarding_tables tables,
                   std::uint8_t const * alignment) noexcept {
  for (auto i = begin; i != end; ++i) {
    auto item = metadata[i];
    item.pointers &= pointer_cells(item.live);
    item.live = retained(item, alignment, i);
    auto destination = item.destination & 0x7fffffffu;
    if (item.live == ~bitmap{0} && !item.pointers) {
      std::memcpy(target + destination, source + i * 32, 256);
      continue;
    }
    for (auto bits = item.live; bits; bits &= bits - 1) {
      auto const bit = static_cast<unsigned>(std::countr_zero(bits));
      word value;
      std::memcpy(&value, source + i * 32 + bit, sizeof(value));
      auto const fields = (item.pointers >> (bit * 2)) & 3;
      if (fields & 1) value = (value & 0xffffffff00000000ULL) | forward(tables, static_cast<bitmap>(value));
      if (fields & 2) value = (value & 0x00000000ffffffffULL) | (word{forward(tables, static_cast<bitmap>(value >> 32))} << 32);
      std::memcpy(target + destination++, &value, sizeof(value));
    }
  }
}

// Aliased output consumes each descriptor before clearing its destination slots;
// leftward compaction ensures those slots never belong to an unread descriptor.
// Separate output appends to existing masks (promotion may share a partial block).
#define JAM_PACK_MASKS(EXTRACT) \
  bool const in_place = metadata == output; \
  std::size_t cleared = 0; \
  for (std::size_t i = 0; i != old_blocks; ++i) { \
    auto item = metadata[i]; \
    item.pointers &= pointer_cells(item.live); \
    if (!item.pointers) continue; \
    item.live = retained(item, alignment, i); \
    word packed; \
    auto const first = static_cast<unsigned>(std::countr_zero(item.live)); \
    auto const run = item.live >> first; \
    if ((run & (run + bitmap{1})) == 0) packed = item.pointers >> (first * 2); \
    else packed = (EXTRACT); \
    if (!packed) continue; \
    auto const destination = std::size_t{item.destination & 0x7fffffffu} * 2; \
    auto const index = destination >> 6; \
    auto const shift = destination & 63; \
    if (in_place) { \
      assert(index <= i); \
      while (cleared <= index) output[cleared++].pointers = 0; \
    } \
    output[index].pointers |= packed << shift; \
    if (shift) { \
      auto const upper = packed >> (64 - shift); \
      if (upper) { \
        if (in_place) { \
          assert(index + 1 <= i); \
          while (cleared <= index + 1) output[cleared++].pointers = 0; \
        } \
        output[index + 1].pointers |= upper; \
      } \
    } \
  } \
  if (in_place) while (cleared < old_blocks) output[cleared++].pointers = 0;

void baseline_pack(heap_block const * metadata, heap_block * output, std::size_t old_blocks,
                   std::uint8_t const * alignment) noexcept {
  JAM_PACK_MASKS(portable_pack(item))
}

#if NATIVE_HOST_X86
#define JAM_TARGETS jam_avx512_popcnt, jam_avx512, jam_avx2
#define JAM_LANES(ISA) ((ISA).has(native::x86_feature::avx512f) ? 16 : 8)
#define JAM_EXTRACT(ISA) native::pext<ISA>(item.pointers, pointer_cells(item.live))
#elif NATIVE_HOST_NEON
#define JAM_TARGETS neon
#define JAM_LANES(ISA) 4
#define JAM_EXTRACT(ISA) portable_pack(item)
#endif

// Layout consumed by the intrinsic gathers: dword indices, sixteen-byte stride.
static_assert(sizeof(heap_block) == 16 && offsetof(heap_block, live) == 0
              && offsetof(heap_block, destination) == 4);
#if NATIVE_HOST_X86
// Avoid expanding native's scalar from_bitset loop before each AVX2 gather/store.
template<class V>
hint_inline hint_const hint_target(NATIVE_TARGET_jam_avx2)
typename V::mask vector_mask(bitmap bits) noexcept {
  static_assert(V::lanes == 8);
  constexpr std::array<bitmap, 8> positions{1, 2, 4, 8, 16, 32, 64, 128};
  return (V(bits) & V::load(positions.data())) != V(0);
}

template<class V>
hint_inline hint_target(NATIVE_TARGET_jam_avx2)
bitmap pack_store(word * target, bitmap kept, V value) noexcept {
  auto const packed = native::compress(vector_mask<V>(kept), value);
  store_prefix(reinterpret_cast<bitmap *>(target), packed.value.to_native(),
               static_cast<unsigned>(packed.count));
  return static_cast<bitmap>(packed.count / 2);
}

#define JAM_GATHER_BLOCK(ISA) \
if constexpr ((ISA).has(native::x86_feature::avx512vpopcntdq) \
              || !(ISA).has(native::x86_feature::avx512f)) { \
  constexpr unsigned batch = lanes == 16 ? 4 : 2; \
  using W = native::wide<V, batch>; \
  using I = native::simd<std::int32_t, lanes, ISA>; \
  auto const live_fields = pointer_cells(item.live); \
  auto const fields = item.pointers & live_fields; \
  for (unsigned chunk = 0; chunk != 64 / lanes; chunk += batch) { \
    W values{V(0)}; \
    native::wide<M, batch> masks{M::from_bitset(0)}; \
    _Pragma("clang loop unroll(full)") \
    for (unsigned j = 0; j != batch; ++j) { \
      values.registers[j] = V::load(reinterpret_cast<bitmap const *>(source + i * 32) \
                                   + lanes * (chunk + j)); \
      if constexpr (lanes == 16) \
        masks.registers[j] = M::from_bitset(fields >> (lanes * (chunk + j))); \
      else \
        masks.registers[j] = vector_mask<V>(fields >> (lanes * (chunk + j))); \
    } \
    if (fields) { \
      native::wide<I, batch> indices{I(0)}; \
      W live{V(0)}, bases{V(0)}, ranks{V(0)}; \
      auto & [...v] = values; \
      auto & [...m] = masks; \
      auto & [...ix] = indices; \
      auto & [...l] = live; \
      auto & [...b] = bases; \
      auto & [...r] = ranks; \
      ((m &= v != V(0)), ...); \
      if (!tables.old) ((m &= (v & V(0x80000000u)) != V(0)), ...); \
      ((ix = native::reinterpret_bits<std::int32_t>( \
          ((v & V(0x7fffffffu)) >> native::imm<5>) << native::imm<2>)), ...); \
      ((b = v), ...); \
      for (unsigned generation = 0; generation != 2; ++generation) { \
        auto const * table = generation ? tables.young : tables.old; \
        if (!table) continue; \
        auto const * live_address = reinterpret_cast<bitmap const *>(table); \
        auto const * base_address = reinterpret_cast<bitmap const *>( \
            reinterpret_cast<char const *>(table) + offsetof(heap_block, destination)); \
        native::wide<M, batch> selected{M::from_bitset(0)}; \
        auto & [...g] = selected; \
        ((g = m & ((v >> native::imm<31>) == V(generation))), ...); \
        if constexpr (lanes == 16) { \
          ((l = native::mask_vpgatherdd<4>(l, g, live_address, ix)), ...); \
          ((b = native::mask_vpgatherdd<4>(b, g, base_address, ix)), ...); \
        } else { \
          ((l = native::mask_vpgatherdd<4>(l, \
              native::reinterpret_bits<std::int32_t>(native::mask_bits(g)), live_address, ix)), ...); \
          ((b = native::mask_vpgatherdd<4>(b, \
              native::reinterpret_bits<std::int32_t>(native::mask_bits(g)), base_address, ix)), ...); \
        } \
        auto const * alignment_table = generation ? tables.young_alignment : tables.old_alignment; \
        if (alignment_table) { \
          auto const * address = reinterpret_cast<bitmap const *>(alignment_table); \
          if constexpr (lanes == 16) \
            ((r = native::mask_vpgatherdd<4>(r, g, address, \
                native::reinterpret_bits<std::int32_t>((v & V(0x7fffffffu)) >> native::imm<8>))), ...); \
          else \
            ((r = native::mask_vpgatherdd<4>(r, native::reinterpret_bits<std::int32_t>(native::mask_bits(g)), address, \
                native::reinterpret_bits<std::int32_t>((v & V(0x7fffffffu)) >> native::imm<8>))), ...); \
        } \
      } \
      ((ix = native::reinterpret_bits<std::int32_t>(l & V::from_native(shift_left(V(1).to_native(), (v & V(31)).to_native())))), ...); \
      ((b = native::select(m & (ix == I(0)), V(0), b)), ...); \
      ((m &= ix != I(0)), ...); \
      if (tables.old_alignment || tables.young_alignment) { \
        ((r = V::from_native(shift_right(r.to_native(), (((v >> native::imm<5>) & V(7)) << native::imm<2>).to_native())) & V(7)), ...); \
        ((l = native::select((r & V(1)) != V(0), l | ((l >> native::imm<1>) & V(0x55555555u)) | ((l & V(0x55555555u)) << native::imm<1>), l)), ...); \
        ((l = native::select((r & V(2)) != V(0), l | ((l >> native::imm<2>) & V(0x33333333u)) | ((l & V(0x33333333u)) << native::imm<2>), l)), ...); \
        ((l = native::select((r & V(4)) != V(0), l | ((l >> native::imm<4>) & V(0x0f0f0f0fu)) | ((l & V(0x0f0f0f0fu)) << native::imm<4>), l)), ...); \
      } \
      ((r = V::from_native(shift_left(V(1).to_native(), (v & V(31)).to_native()))), ...); \
      ((r = l & (r - V(1))), ...); \
      if constexpr ((ISA).has(native::x86_feature::avx512vpopcntdq)) \
        ((r = native::vpopcntd(r)), ...); \
      else \
        ((r = native::popcount(r)), ...); \
      ((v = native::masked_add(m, b, b, r)), ...); \
    } \
    _Pragma("clang loop unroll(full)") \
    for (unsigned j = 0; j != batch; ++j) { \
      auto const kept = live_fields >> (lanes * (chunk + j)); \
      if constexpr (lanes == 16) { \
        auto const written = native::compress_store(reinterpret_cast<bitmap *>(target + destination), \
            lanes, M::from_bitset(kept), values.registers[j]); \
        destination += static_cast<bitmap>(written / 2); \
      } else if (kept & 255) { \
        destination += pack_store(target + destination, static_cast<bitmap>(kept), values.registers[j]); \
      } \
    } \
  } \
  continue; \
}

#else
#define JAM_GATHER_BLOCK(ISA)
#endif

// Keep AVX-512 compaction as register compression followed by a bounded store;
// avoid memory-form compress-store (the Zen 4 performance constraint).
// A full-width overstore can overwrite a neighboring worker's output.
#define JAM_COMPACTOR(name, ISA, ...) \
  template<native::isa<> A> \
    requires (native::target<A, __VA_ARGS__> == native::target<ISA, __VA_ARGS__>) \
  void name##_move(word const * source, word * target, heap_block const * metadata, \
                   std::size_t begin, std::size_t end, forwarding_tables tables, \
                   std::uint8_t const * alignment) noexcept { \
    constexpr unsigned lanes = JAM_LANES(ISA); \
    using V = native::simd<bitmap, lanes, ISA>; \
    using M = typename V::mask; \
    for (auto i = begin; i != end; ++i) { \
      auto item = metadata[i]; \
      if (!item.live) continue; \
      item.pointers &= pointer_cells(item.live); \
      item.live = retained(item, alignment, i); \
      auto destination = item.destination & 0x7fffffffu; \
      if (item.live == ~bitmap{0} && !item.pointers) { \
        std::memcpy(target + destination, source + i * 32, 256); \
        continue; \
      } \
      JAM_GATHER_BLOCK(ISA) \
      for (unsigned cell = 0; cell != 32; cell += lanes / 2) { \
        auto const live = (item.live >> cell) & ((1u << (lanes / 2)) - 1); \
        if (!live) continue; \
        std::array<bitmap, lanes> values; \
        std::memcpy(values.data(), source + i * 32 + cell, sizeof(values)); \
        auto fields = (item.pointers >> (cell * 2)) & pointer_cells(live); \
        for (; fields; fields &= fields - 1) { \
          auto const lane = std::countr_zero(fields); \
          values[lane] = forward(tables, values[lane]); \
        } \
        auto const written = native::compress_store(reinterpret_cast<bitmap *>(target + destination), \
          lanes, M::from_bitset(pointer_cells(live)), V::load(values.data())); \
        destination += static_cast<bitmap>(written / 2); \
      } \
    } \
  } \
  template<native::isa<> A> \
    requires (native::target<A, __VA_ARGS__> == native::target<ISA, __VA_ARGS__>) \
  void name##_pack(heap_block const * metadata, heap_block * output, std::size_t old_blocks, \
                   std::uint8_t const * alignment) noexcept { \
    JAM_PACK_MASKS(JAM_EXTRACT(ISA)) \
  }
NATIVE_TARGET_VARIANTS(vector, JAM_COMPACTOR, JAM_TARGETS)
#undef JAM_GATHER_BLOCK
#undef JAM_COMPACTOR
#undef JAM_PACK_MASKS

constexpr compactor baseline{baseline_move, baseline_pack, native::scalar, "baseline"};
#define JAM_ENTRY(unused, tag) \
  compactor{vector_move<NATIVE_TARGET_ISA(tag)>, vector_pack<NATIVE_TARGET_ISA(tag)>, \
    NATIVE_TARGET_ISA(tag), #tag},
constexpr compactor variants[]{NATIVE_DETAIL_TARGET_MAP(JAM_ENTRY, unused, JAM_TARGETS) baseline};
#undef JAM_ENTRY
}

namespace jam::detail {
// Internal partition API also lets tests compare every admitted implementation.
std::span<compactor const> compactor_variants() noexcept { return variants; }
compactor choose_compactor() noexcept {
  auto selected = baseline;
  native::with_isa(NATIVE_TARGET_LIST(JAM_TARGETS), native::observe_cpu(), [&]<native::isa<> A> {
    for (auto const & variant : variants) if (variant.requirement == A) selected = variant;
  });
  return selected;
}
}
