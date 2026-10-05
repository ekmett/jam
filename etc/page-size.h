// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#pragma once
#include <cstddef>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
inline std::size_t system_page_size() noexcept {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return static_cast<std::size_t>(::getpagesize());
#endif
}
