// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ratio>
#include <type_traits>
import jam;

namespace adl_check {
using jam::operator""_KiB;
static_assert(ceil<jam::units::pages>(1_KiB).count() == 1);
}

using namespace jam;
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
int main() {
  volatile auto source = std::numeric_limits<std::uint64_t>::max();
  auto const value = space_cast<space<std::uint64_t, std::ratio<3>>>(
    space<std::uint64_t, std::ratio<2>>{source});
  if (value.count() != 12297829382473034410ULL) std::abort();
}
