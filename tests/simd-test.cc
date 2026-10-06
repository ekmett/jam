// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <native/attributes.h>
#include <native/targets.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
import jam;

void check(bool valid, char const * message) {
  if (!valid) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
struct simd_node {
  jam::ptr<simd_node> next;
  std::uint32_t data;
  float number;
  std::uint64_t large;
  static constexpr auto manifest = jam::make_manifest<simd_node>(&simd_node::next);
};
template<class P> struct simd_holder {
  P pointers;
  native::wide<P, 2> packs;
  std::uint64_t sentinel;
  static constexpr auto manifest = jam::make_manifest<simd_holder>(&simd_holder::pointers, &simd_holder::packs);
};
#if defined(__x86_64__) || defined(_M_X64)
#define JAM_TEST_TARGETS avx512, avx2, scalar
#else
#define JAM_TEST_TARGETS neon, scalar
#endif
#define JAM_SIMD_CHECKS(name, ISA, ...) \
template<native::isa<> A, std::size_t N> \
  requires (native::target<A, __VA_ARGS__> == native::target<ISA, __VA_ARGS__>) \
void check_simd() { \
  using P = native::simd<jam::ptr<simd_node>, N, A>; \
  using W = native::wide<P, 2>; \
  using H = simd_holder<P>; \
  using M = typename P::mask_type; \
  static_assert(sizeof(P) == sizeof(native::simd<std::uint32_t, N, A>)); \
  static_assert(alignof(P) == alignof(native::simd<std::uint32_t, N, A>)); \
  static_assert(!std::is_trivially_copyable_v<P> && std::is_standard_layout_v<P>); \
  for (unsigned workers : {1u, 4u}) { \
    jam::heap heap{{.old = {.capacity=jam::units::pages{128}, .reserve=jam::units::pages{2}}, .young = {.capacity=jam::units::pages{128}, .reserve=jam::units::pages{2}}, .workers=workers}}; \
    jam::heap_scope scope{heap}; \
    for (unsigned i = 0; i != 17; ++i) static_cast<void>(jam::mk<std::uint64_t>(i)); \
    std::array<jam::ptr<simd_node>, N> targets{}; \
    for (std::size_t i = 0; i != N; ++i) if (i % 2 == 0) \
      targets[i] = jam::mk<simd_node>(nullptr, unsigned(i + 11), float(i) + 0.5f, std::uint64_t{1} << (i + 32)); \
    for (std::size_t i = 0; i != N; ++i) if (targets[i]) targets[i]->next = targets[i]; \
    P input{targets}; \
    std::array<jam::ptr<simd_node>, N + 1> copy{}; \
    input.store(copy.data()); \
    check(copy[N] == nullptr && P::load(copy.data())[0] == targets[0], "typed lane memory roundtrip"); \
    auto direct = heap.root(heap.mk<P>(input)); \
    auto packed = heap.root(heap.mk<W>(W{input})); \
    auto outer = heap.root(heap.mk<H>(input, W{input}, std::uint64_t{0xdeadbeef})); \
    auto const old = direct.get().get(); \
    for (unsigned round = 0; round != 3; ++round) { \
      heap.collect(); \
      check(direct.get().get() < old, "SIMD root allocation moves"); \
      auto const p = heap.load(direct.get()); \
      auto const w = heap.load(packed.get()); \
      auto const h = heap.load(outer.get()); \
      auto const active = p != nullptr; \
      auto values = gather(p, &simd_node::data, active); \
      auto defaults = gather(p, &simd_node::data); \
      auto children = gather(p, &simd_node::next, active); \
      auto floats = gather(p, &simd_node::number, active); \
      auto wide_values = gather(w, &simd_node::data); \
      auto wide_explicit = gather(w, &simd_node::data, native::wide<M, 2>{active}); \
      std::array<std::uint32_t, N> data{}, inferred{}, wide_data{}, explicit_data{}; \
      std::array<float, N> numbers{}; \
      values.store(data.data()); defaults.store(inferred.data()); floats.store(numbers.data()); \
      for (std::size_t k = 0; k != 2; ++k) { \
        wide_values.registers[k].store(wide_data.data()); \
        wide_explicit.registers[k].store(explicit_data.data()); \
        check(wide_data == data && explicit_data == data, "wide lift agrees with SIMD gathers"); \
      } \
      for (std::size_t i = 0; i != N; ++i) { \
        check(p[i] == h.pointers[i] && p[i] == h.packs.registers[1][i], "all heap-resident pointer lanes are forwarded"); \
        check(children[i] == p[i], "gathered pointer lanes retain their type and value"); \
        check(data[i] == (i % 2 == 0 ? i + 11 : 0) && data[i] == inferred[i], "masked and inferred gathers agree"); \
        check(numbers[i] == (i % 2 == 0 ? float(i) + 0.5f : 0), "floating member gather preserves bits"); \
      } \
      check(h.sentinel == 0xdeadbeef, "neighboring scalar data remains data"); \
      P invalid = p; \
      invalid[0] = jam::ptr<simd_node>{0xffffffffu}; \
      auto skipped = gather(invalid, &simd_node::data, M::from_bitset(active.to_bitset() & ~std::uint64_t{1})); \
      skipped.store(data.data()); \
      check(data[0] == 0, "inactive invalid addresses are not accessed"); \
      auto empty = gather(P{}, &simd_node::data); \
      empty.store(data.data()); \
      for (auto x : data) check(x == 0, "all-null gather produces zero"); \
      if constexpr (requires { sizeof(native::simd<std::uint64_t, N, A>); }) { \
        auto big = gather(p, &simd_node::large); \
        std::array<std::uint64_t, N> output{}; \
        big.store(output.data()); \
        for (std::size_t i = 0; i != N; ++i) \
          check(output[i] == (i % 2 == 0 ? std::uint64_t{1} << (i + 32) : 0), "64-bit member gather preserves lanes"); \
      } \
    } \
    auto const old_target = (*direct)[0]; \
    auto young_target = jam::mk<simd_node>(nullptr, 71u, 7.5f, std::uint64_t{91}); \
    P mixed{old_target}; \
    mixed[0] = young_target; \
    *direct = mixed; \
    *packed = W{mixed}; \
    outer->pointers = mixed; \
    outer->packs = W{mixed}; \
    check(heap.remembered_size() == 6, "SIMD and wide assignment register exactly the young source lanes"); \
    auto const gathered = gather(mixed, &simd_node::data); \
    std::array<std::uint32_t, N> mixed_data{}; \
    gathered.store(mixed_data.data()); \
    check(mixed_data[0] == 71 && mixed_data[1] == 11, "gather combines old and young bases"); \
    heap.collect_young(); \
    check((*direct)[0]->data == 71 && (*packed).registers[1][0] == (*direct)[0] \
          && outer->pointers[0] == (*direct)[0] && outer->packs.registers[0][0] == (*direct)[0], \
          "minor collection forwards every heap-resident vector lane"); \
    P replacement{jam::mk<simd_node>(nullptr, 81u, 8.5f, std::uint64_t{101})}; \
    jam::unsafe_assign(*direct, replacement); \
    direct->remember(); \
    jam::assign(*packed, W{replacement}); \
    replacement.store(&outer->pointers[0]); \
    heap.collect_young(true); \
    check(!(*direct)[0].is_young() && (*direct)[N - 1]->data == 81 \
          && (*packed).registers[1][0] == (*direct)[0] && outer->pointers[N - 1] == (*direct)[0], \
          "bulk stores and explicit registration survive promotion"); \
    heap.collect(); \
    check((*direct)[0]->data == 81 && outer->packs.registers[0][0]->data == 71, \
          "full collection retains both generations after vector writes"); \
  } \
}
NATIVE_TARGET_VARIANTS(check, JAM_SIMD_CHECKS, JAM_TEST_TARGETS)
#undef JAM_SIMD_CHECKS
int main() {
  auto const cpu = native::observe_cpu();
  auto const run = [&]<native::isa<> A> {
    if (!native::classify_isa(cpu, A).admitted()) return;
    if constexpr (requires { sizeof(native::simd<std::uint32_t, 2, A>); }) check_simd<A, 2>();
    if constexpr (requires { sizeof(native::simd<std::uint32_t, 3, A>); }) check_simd<A, 3>();
    check_simd<A, 4>();
#if defined(__x86_64__) || defined(_M_X64)
    if constexpr (A.has(native::x86_feature::avx2)) check_simd<A, 8>();
    if constexpr (A.has(native::x86_feature::avx512f)) check_simd<A, 16>();
#endif
  };
  run.template operator()<native::simd<std::uint32_t, 4>::architecture>();
#if defined(__x86_64__) || defined(_M_X64)
  run.template operator()<native::avx2>();
  run.template operator()<native::avx512>();
#endif
  std::puts("pointer SIMD tracing and gather checks passed");
}
