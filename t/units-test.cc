// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ratio>
#include <type_traits>
import jam.units;

namespace adl_check {
using jam::units::operator""_KiB;
static_assert(ceil<jam::units::pages>(1_KiB).count() == 1);
}

using namespace jam::units;

static_assert(std::same_as<decltype(1_KiB), kibibytes>);
static_assert(std::same_as<decltype(1_MB), megabytes>);
static_assert(!std::convertible_to<bytes, std::size_t>);
static_assert(!std::convertible_to<std::size_t, bytes>);
static_assert(std::convertible_to<mebibytes, bytes>);
static_assert(!std::constructible_from<mebibytes, kibibytes>);
static_assert(!std::constructible_from<kilobytes, kibibytes>);
static_assert(bytes{1_MiB}.count() == 1048576);
static_assert(1_MiB == 1024_KiB);
static_assert(1_MiB + 512_KiB == 1536_KiB);
static_assert(1_MiB / 256_KiB == 4);
static_assert(1_MiB % 300_KiB == 124_KiB);
static_assert(3 * 1_KiB == 3072_B);
static_assert(space_cast<mebibytes>(1536_KiB) == 1_MiB);
static_assert(space_cast<kilobytes>(1_KiB) == 1_kB);
static_assert(ceil<pages>(bytes{pages::ratio::num + 1}).count() == 2);
using signed_bytes = space<std::int64_t>;
using signed_kibibytes = space<std::int64_t, kibi>;
static_assert(space_cast<signed_kibibytes>(signed_bytes{-1025}).count() == -1);
static_assert(space_cast<signed_kibibytes>(signed_bytes{-1}).count() == 0);
static_assert(space_cast<bytes>(space<double>{1.25}).count() == 1);
static_assert(space_cast<bytes>(space<double>{-0.5}).count() == 0);
static_assert(space_cast<signed_bytes>(space<double>{-1.25}).count() == -1);
static_assert(space<double, mebi>{1536_KiB}.count() == 1.5);
static_assert(1.5_MiB == 1536_KiB);
static_assert(ceil<bytes>(1.5_B) == 2_B);
static_assert(!std::constructible_from<bytes, space<double>>);
// Multiplication must not overflow an intermediate when the result fits.
static_assert(space_cast<space<std::uint64_t, std::ratio<3>>>(
  space<std::uint64_t, std::ratio<2>>{std::numeric_limits<std::uint64_t>::max()}).count()
  == 12297829382473034410ULL);
static_assert([] {
  auto n = 1_KiB;
  ++n; n += 2_KiB; n *= 3; n /= 2; n %= 4;
  return n == 2_KiB && n-- == 2_KiB && n == 1_KiB;
}());

static_assert(ceil<mebibytes>(1536_KiB) == 2_MiB);
static_assert(floor<signed_kibibytes>(signed_bytes{-1}).count() == -1);
static_assert(round<mebibytes>(1536_KiB) == 2_MiB);
static_assert(round<mebibytes>(2560_KiB) == 2_MiB);
static_assert(abs(signed_bytes{-12}).count() == 12);
static_assert(signed_bytes{-1} < 1_B);
static_assert(ceil<space<std::uint64_t, std::ratio<3>>>(
  space<std::uint64_t, std::ratio<2>>{std::numeric_limits<std::uint64_t>::max() - 1}).count()
  == 12297829382473034410ULL);
namespace {
void check(bool condition) noexcept {
  if (!condition) std::abort();
}

void rounding() noexcept {
  using tick = std::chrono::duration<std::int64_t>;
  using block = std::chrono::duration<std::int64_t, kibi>;
  // Exercise both sides of zero, exact units, and odd/even halfway ties.
  for (auto const count : {-2560, -1536, -1025, -1024, -513, -512, -511, -1,
                          0, 1, 511, 512, 513, 1024, 1025, 1536, 2560}) {
    volatile std::int64_t input = count;
    auto const n = input;
    auto const size = signed_bytes{n};
    check(space_cast<signed_kibibytes>(size).count() == std::chrono::duration_cast<block>(tick{n}).count());
    check(floor<signed_kibibytes>(size).count() == std::chrono::floor<block>(tick{n}).count());
    check(ceil<signed_kibibytes>(size).count() == std::chrono::ceil<block>(tick{n}).count());
    check(round<signed_kibibytes>(size).count() == std::chrono::round<block>(tick{n}).count());
    check(abs(size).count() == (n < 0 ? -n : n));
  }
}

void arithmetic() noexcept {
  volatile std::int64_t input = 7;
  auto n = signed_bytes{input};
  check((+n).count() == 7 && (-n).count() == -7);
  check(n++ == signed_bytes{7} && n == signed_bytes{8});
  check(--n == signed_bytes{7});
  check(n-- == signed_bytes{7} && ++n == signed_bytes{7});
  n += signed_bytes{5};
  n -= signed_bytes{2};
  n *= 3;
  n /= 4;
  n %= signed_bytes{4};
  check(n.count() == 3);
  check((n % 2).count() == 1);
  check((n * 4).count() == 12 && (4 * n).count() == 12);
  check((n / 2).count() == 1);
  check(signed_kibibytes{n.count()} + signed_bytes{8} == signed_bytes{3080});
  check(signed_kibibytes{n.count()} - signed_bytes{8} == signed_bytes{3064});
  check(signed_kibibytes{n.count()} / signed_bytes{512} == 6);
  check(signed_kibibytes{n.count()} % signed_bytes{1000} == signed_bytes{72});
  check(signed_bytes::zero().count() == 0);
  check(signed_bytes::min().count() == std::numeric_limits<std::int64_t>::lowest());
  check(signed_bytes::max().count() == std::numeric_limits<std::int64_t>::max());
}

void floating() noexcept {
  volatile double input = 1.5;
  auto n = space<double, kibi>{input};
  check(space<double>{n}.count() == 1536.0);
  n += space<double, kibi>{0.5};
  n -= space<double, kibi>{0.25};
  n *= 2.0;
  n /= 4.0;
  check(n.count() == 0.875 && n < 1_KiB && n > 512_B);
  check(space_cast<bytes>(n).count() == 896);
  check(floor<kibibytes>(n) == 0_KiB && ceil<kibibytes>(n) == 1_KiB);
  check(round<kibibytes>(n) == 1_KiB);
  auto const nan = space<double>{std::numeric_limits<double>::quiet_NaN()};
  check((nan <=> n) == std::partial_ordering::unordered && nan != nan);
}
}

int main() {
  rounding();
  arithmetic();
  floating();
  volatile auto source = std::numeric_limits<std::uint64_t>::max();
  auto const value = space_cast<space<std::uint64_t, std::ratio<3>>>(
    space<std::uint64_t, std::ratio<2>>{source});
  if (value.count() != 12297829382473034410ULL) std::abort();
}
