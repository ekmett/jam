// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

/// \file
/// \brief Platform implementation of virtual-memory aliases.
module;
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <native/attributes.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif !defined(__linux__) && !defined(_WIN32)
#error "jam requires macOS, Linux or Windows virtual-memory aliases"
#endif

#include <cstdint>

module jam.mapping;

namespace jam::detail {
#if defined(_WIN32)
struct heap_mapping::backing {
  HANDLE handle;
  explicit backing(std::size_t bytes) noexcept
    : handle(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr)) {
    if (!handle) system_failure("create heap backing");
  }
  ~backing() noexcept { ::CloseHandle(handle); }
};
#endif

native_cold native_noreturn
void heap_failure(char const * operation, int code) noexcept {
  std::fprintf(stderr, "jam: %s (%d)\n", operation, code);
  std::abort();
}

generation_reservation::generation_reservation(std::size_t old_maximum, std::size_t young_maximum) noexcept
    : bytes_(2 * (old_maximum + young_maximum)) {
#if defined(_WIN32)
  base_ = static_cast<std::byte *>(::VirtualAlloc2(nullptr, nullptr, bytes_,
    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
  if (!base_) heap_failure("reserve generation address space", ::GetLastError());
#else
  auto * p = ::mmap(nullptr, bytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) heap_failure("reserve generation address space", errno);
  base_ = static_cast<std::byte *>(p);
#endif
}

generation_reservation::~generation_reservation() noexcept {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION region{};
  if (!::VirtualQuery(base_, &region, sizeof(region))) heap_failure("query generation reservation", ::GetLastError());
  if (region.RegionSize != bytes_ &&
      !::VirtualFree(base_, bytes_, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    heap_failure("coalesce generation reservation", ::GetLastError());
  if (!::VirtualFree(base_, 0, MEM_RELEASE)) heap_failure("release generation reservation", ::GetLastError());
#else
  if (::munmap(base_, bytes_)) heap_failure("release generation reservation", errno);
#endif
}

[[noreturn]] void heap_mapping::system_failure(char const * operation) noexcept {
#if defined(_WIN32)
  heap_failure(operation, static_cast<int>(::GetLastError()));
#else
  heap_failure(operation, errno);
#endif
}

void heap_mapping::validate_size(std::size_t bytes) noexcept {
  if (!bytes || bytes % page_size() || bytes > std::numeric_limits<std::size_t>::max() / 2)
    heap_failure("capacity must be a nonzero whole-page size");
}

heap_mapping::heap_mapping(reservation, std::size_t bytes) noexcept {
  validate_size(bytes);
#if defined(_WIN32)
  auto const result = ::VirtualAlloc2(nullptr, nullptr, bytes * 2,
    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (!result) system_failure("reserve heap placeholders");
#else
  auto const result = ::mmap(nullptr, bytes * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (result == MAP_FAILED) system_failure("jam: reserve virtual memory");
#endif
  address = static_cast<std::byte *>(result);
  capacity = bytes;
}

#if defined(_WIN32)
void heap_mapping::alias(span const & source, std::byte * target, std::size_t count,
                  std::size_t offset) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (!::VirtualQuery(target, &region, sizeof(region))) system_failure("query heap placeholder");
  // VirtualQuery starts at the queried page, which can be inside a placeholder.
  // Query its allocation base before deciding whether either end needs splitting.
  if (region.State == MEM_RESERVE && region.BaseAddress != region.AllocationBase &&
      !::VirtualQuery(region.AllocationBase, &region, sizeof(region)))
    system_failure("query whole heap placeholder");
  auto const begin = static_cast<std::byte *>(region.BaseAddress);
  auto const prefix = static_cast<std::size_t>(target - begin);
  if (region.State != MEM_RESERVE || prefix > region.RegionSize || count > region.RegionSize - prefix)
    heap_failure("heap alias requires a containing placeholder");
  if (prefix && !::VirtualFree(begin, prefix, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    system_failure("split heap placeholder prefix");
  if (count < region.RegionSize - prefix &&
      !::VirtualFree(target, count, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    system_failure("split heap placeholder suffix");
  if (!::MapViewOfFile3(source.section->handle, ::GetCurrentProcess(), target,
        source.offset + offset, count, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
    system_failure("alias heap backing");
}
#endif

#if !defined(_WIN32)
void heap_mapping::alias(std::byte * source, std::byte * target, std::size_t count) noexcept {
#if defined(__APPLE__)
  auto destination = reinterpret_cast<mach_vm_address_t>(target);
  vm_prot_t current = 0, maximum = 0;
  auto const result = ::mach_vm_remap(::mach_task_self(), &destination, count, 0,
    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, ::mach_task_self(),
    reinterpret_cast<mach_vm_address_t>(source), false, &current, &maximum, VM_INHERIT_SHARE);
  if (result != KERN_SUCCESS)
    heap_failure("alias virtual memory", result);
  assert(destination == reinterpret_cast<mach_vm_address_t>(target));
#else
  auto const result = ::mremap(source, 0, count, MREMAP_MAYMOVE | MREMAP_FIXED, target);
  if (result == MAP_FAILED) system_failure("jam: alias virtual memory");
  assert(result == target);
#endif
}
#endif

void heap_mapping::fresh(std::size_t begin, std::size_t count) noexcept {
  if (!count) return;
#if defined(_WIN32)
  spans.push_back({begin, begin + count, std::make_shared<backing>(count), 0});
  alias(spans.back(), address + begin, count);
#else
  auto const result = ::mmap(address + begin, count, PROT_READ | PROT_WRITE,
    MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (result == MAP_FAILED) system_failure("jam: map fresh backing");
  spans.push_back({begin, begin + count});
#endif
}

void heap_mapping::finish_aliases() noexcept {
  std::sort(spans.begin(), spans.end(), [](auto const & a, auto const & b) noexcept { return a.begin < b.begin; });
  for (auto const & s : spans)
#if defined(_WIN32)
    alias(s, address + capacity + s.begin, s.end - s.begin);
#else
    alias(address + s.begin, address + capacity + s.begin, s.end - s.begin);
#endif
}

void heap_mapping::discard_gap(std::size_t start, std::size_t live) const noexcept {
#if defined(_WIN32)
  // Advisory discard releases dead physical pages; a partially retained section
  // keeps its commit charge until the last surviving view/handle is released.
  for (auto const & s : spans) {
    auto const discard = [&](std::size_t begin, std::size_t end) noexcept {
      begin = std::max(begin, s.begin);
      end = std::min(end, s.end);
      if (begin < end) static_cast<void>(::VirtualAlloc(address + begin, end - begin, MEM_RESET, PAGE_READWRITE));
    };
    if (start + live <= capacity) {
      discard(0, start);
      discard(start + live, capacity);
    } else discard(start + live - capacity, start);
  }
#else
  auto const begin = (start + live) % capacity;
  auto const count = capacity - live;
  auto const first = std::min(count, capacity - begin);
#if defined(__APPLE__)
  constexpr auto advice = MADV_FREE_REUSABLE;
#else
  constexpr auto advice = MADV_REMOVE;
#endif
  // dev: advisory release; kernels may retain dead pages until memory pressure.
  if (first) static_cast<void>(::madvise(address + begin, first, advice));
  if (count > first) static_cast<void>(::madvise(address, count - first, advice));
#endif
}

void heap_mapping::reset_fixed() noexcept {
  assert(fixed_extent);
#if defined(_WIN32)
  for (auto const & s : spans) {
    if (!::UnmapViewOfFile2(::GetCurrentProcess(), address + s.begin, MEM_PRESERVE_PLACEHOLDER) ||
        !::UnmapViewOfFile2(::GetCurrentProcess(), address + capacity + s.begin, MEM_PRESERVE_PLACEHOLDER))
      system_failure("restore generation placeholders");
  }
  MEMORY_BASIC_INFORMATION region{};
  if (!::VirtualQuery(address, &region, sizeof(region))) system_failure("query generation placeholder");
  if (region.RegionSize < fixed_extent &&
      !::VirtualFree(address, fixed_extent, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    system_failure("coalesce generation placeholders");
#else
  if (::mmap(address, fixed_extent, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
    system_failure("restore generation reservation");
#endif
  spans.clear();
}

heap_mapping::heap_mapping(std::size_t bytes) noexcept : heap_mapping(reservation{}, bytes) {
  spans.reserve(1);
  fresh(0, bytes);
  finish_aliases();
}

heap_mapping::heap_mapping(std::size_t bytes, std::byte * base, std::size_t maximum) noexcept
    : address(base), capacity(bytes), fixed_extent(2 * maximum) {
  validate_size(bytes);
  if (bytes > maximum) heap_failure("generation exceeds reserved address range");
  spans.reserve(1);
  fresh(0, bytes);
  finish_aliases();
}

void heap_mapping::replace(heap_mapping & staged) noexcept {
  if (!fixed_extent) { swap(staged); return; }
  if (staged.capacity * 2 > fixed_extent) heap_failure("generation growth exceeds maximum");
  reset_fixed();
  capacity = staged.capacity;
  for (auto const & s : staged.spans) {
#if defined(_WIN32)
    alias(s, address + s.begin, s.end - s.begin);
#else
    alias(staged.address + s.begin, address + s.begin, s.end - s.begin);
#endif
  }
  spans = staged.spans;
  finish_aliases();
}

heap_mapping::~heap_mapping() noexcept {
  if (!address) return;
  if (fixed_extent) { reset_fixed(); return; }
#if defined(_WIN32)
  for (auto const & s : spans) {
    ::UnmapViewOfFile(address + s.begin);
    ::UnmapViewOfFile(address + capacity + s.begin);
  }
#else
  static_cast<void>(::munmap(address, capacity * 2));
#endif
}

std::size_t heap_mapping::page_size() noexcept {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return static_cast<std::size_t>(::getpagesize());
#endif
}

void heap_mapping::publish(std::size_t start, std::byte * target, std::size_t count) const noexcept {
  if (start >= capacity || start % page_size() || count % page_size() ||
      count > capacity || reinterpret_cast<std::uintptr_t>(target) % page_size())
    heap_failure("invalid published heap arc");
  while (count) {
    auto const where = std::lower_bound(spans.begin(), spans.end(), start,
      [](span const & s, std::size_t value) noexcept { return s.end <= value; });
    assert(where != spans.end() && where->begin <= start);
    auto const part = std::min(count, where->end - start);
#if defined(_WIN32)
    alias(*where, target, part, start - where->begin);
#else
    alias(address + start, target, part);
#endif
    start = (start + part) % capacity;
    target += part;
    count -= part;
  }
}

void heap_mapping::unpublish(std::byte * target, std::size_t count) noexcept {
  if (!count || count % page_size() || reinterpret_cast<std::uintptr_t>(target) % page_size())
    heap_failure("invalid unpublished heap window");
#if defined(_WIN32)
  auto * cursor = target;
  auto remaining = count;
  std::size_t views = 0;
  while (remaining) {
    MEMORY_BASIC_INFORMATION region{};
    if (!::VirtualQuery(cursor, &region, sizeof(region))) system_failure("query published heap view");
    if (region.AllocationBase != cursor || region.BaseAddress != cursor || region.Type != MEM_MAPPED ||
        region.State != MEM_COMMIT || !region.RegionSize || region.RegionSize > remaining)
      heap_failure("published heap window was modified by its host");
    if (!::UnmapViewOfFile2(::GetCurrentProcess(), cursor, MEM_PRESERVE_PLACEHOLDER))
      system_failure("restore published heap placeholder");
    cursor += region.RegionSize;
    remaining -= region.RegionSize;
    ++views;
  }
  if (views > 1 && !::VirtualFree(target, count, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    system_failure("coalesce published heap placeholders");
#else
  if (::mmap(target, count, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
    system_failure("restore published heap reservation");
#endif
}

heap_mapping heap_mapping::resized(std::size_t old_start_bytes, std::size_t live_bytes,
                     std::size_t new_bytes, std::size_t new_start_bytes) const noexcept {
  validate_size(new_bytes);
  auto const page = page_size();
  if (!address || old_start_bytes >= capacity || new_start_bytes >= new_bytes ||
      old_start_bytes % page || new_start_bytes % page ||
      live_bytes > capacity || live_bytes > new_bytes)
    heap_failure("invalid circular remap range");
  auto const live = ((live_bytes + page - 1) / page) * page;
  heap_mapping result(reservation{}, new_bytes);
  result.spans.reserve(spans.size() + 4);
  auto source = old_start_bytes;
  auto target = new_start_bytes;
  auto remaining = live;
  while (remaining) {
    auto const where = std::lower_bound(spans.begin(), spans.end(), source,
      [](span const & s, std::size_t value) noexcept { return s.end <= value; });
    assert(where != spans.end() && where->begin <= source);
    auto const count = std::min({remaining, where->end - source, new_bytes - target});
#if defined(_WIN32)
    alias(*where, result.address + target, count, source - where->begin);
    result.spans.push_back({target, target + count, where->section, where->offset + source - where->begin});
#else
    alias(address + source, result.address + target, count);
    result.spans.push_back({target, target + count});
#endif
    source = (source + count) % capacity;
    target = (target + count) % new_bytes;
    remaining -= count;
  }
  if (!live) result.fresh(0, new_bytes);
  else if (new_start_bytes + live <= new_bytes) {
    result.fresh(0, new_start_bytes);
    result.fresh(new_start_bytes + live, new_bytes - new_start_bytes - live);
  } else result.fresh(new_start_bytes + live - new_bytes, new_bytes - live);
  result.finish_aliases();
  discard_gap(old_start_bytes, live);
  return result;
}
}
