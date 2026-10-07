// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <thread>
#include <vector>
import jam.work;

extern "C" void check_work_pushing() noexcept {
  auto check = [](bool value, std::source_location at = std::source_location::current()) noexcept {
    if (value) return;
    std::fprintf(stderr, "work pushing check failed at %s:%u\n", at.file_name(), at.line());
    std::abort();
  };
  // The deadline is accumulated, not reset to the observation time.
  jam::detail::donation_clock timer{7};
  auto const start = timer.deadline;
  auto const late = start + std::chrono::milliseconds{1};
  unsigned attempts = 0;
  timer.catch_up(late, [&] noexcept { ++attempts; });
  check(attempts > 1 && timer.deadline > late);

  // Each graph vertex creates two children, once. Many simultaneous senders,
  // repeated empty transitions, and final delivery racing with retirement.
  constexpr unsigned count = 32767;
  for (unsigned round = 0; round != 12; ++round) {
    std::array<unsigned, 1> roots{1};
    jam::detail::frontier<unsigned> frontier{4, roots};
    std::vector<std::atomic<unsigned>> seen(count + 1);
    std::vector<std::jthread> workers;
    std::atomic<unsigned> receivers{0};
    std::barrier started{4};
    for (unsigned id = 0; id != 4; ++id) workers.emplace_back([&, id] {
      started.arrive_and_wait();
      unsigned job;
      bool worked = false;
      while (frontier.pop(id, job)) {
        worked = true;
        check(seen[job].fetch_add(1, std::memory_order_relaxed) == 0);
        if (job * 2 <= count) {
          frontier.push(id, job * 2);
          frontier.push(id, job * 2 + 1);
        }
        for (unsigned peer = 0; peer != 4; ++peer)
          if (peer != id) frontier.donate(id, peer);
      }
      if (worked) receivers.fetch_add(1, std::memory_order_relaxed);
    });
    workers.clear();
    for (unsigned i = 1; i <= count; ++i) check(seen[i].load() == 1);
    check(receivers.load() > 1);
  }
  // A nested pool runs inline. Count only workers that can actually enter,
  // otherwise an empty nested collection waits for unstarted participants.
  jam::detail::work_pool outer{2};
  outer.run(2, [](void *, std::size_t) noexcept {
    jam::detail::work_pool inner{2};
    if (inner.available_workers() != 1) std::abort();
    jam::detail::frontier<unsigned> nested{inner.available_workers(), {}};
    inner.run(1, [](void * raw, std::size_t id) noexcept {
      unsigned job;
      if (static_cast<jam::detail::frontier<unsigned> *>(raw)->pop(id, job)) std::abort();
    }, &nested);
  }, nullptr);

  // No roots, including workers that enter only after others have retired.
  jam::detail::frontier<unsigned> empty{4, {}};
  std::vector<std::jthread> idle;
  for (unsigned id = 0; id != 4; ++id) idle.emplace_back([&, id] {
    unsigned job;
    check(!empty.pop(id, job));
  });
}
