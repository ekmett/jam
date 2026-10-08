// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <new>
#include <atomic>
#include <cstdlib>
import jam;
import work;

extern "C" void check_work_pushing() noexcept {
  // Collections nested in a work callback keep marking on that caller. A
  // nested heap must not wait for markers that cannot enter the inner phase.
  work::pool pool{2};
  std::atomic<unsigned> completed{0};
  auto group = pool.gig<unsigned>([&](unsigned value) noexcept {
    jam::heap h{{.workers = 4}};
    jam::heap_scope scope{h};
    jam::root answer = jam::mk<unsigned>(value);
    h.collect_major();
    if (*answer != value) std::abort();
    completed.fetch_add(1, std::memory_order_relaxed);
  });
  group.push(41); group.push(42); group.close(); group.join();
  if (completed != 2) std::abort();
}
