// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
#endif
import jam;

using offset = jam::heap::offset;
using generation = jam::heap::host::generation;
constexpr offset young_bit = 0x80000000u;
constexpr auto page_words = JAM_PAGE_BYTES / 8;
void check(bool ok, char const * message) noexcept {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
jam::heap_options options(unsigned workers) {
  jam::generation_options g{.capacity = jam::units::pages{12}, .reserve = jam::units::pages{2},
                           .maximum = jam::units::pages{12}, .shrink_shift = 0};
  return {.old = g, .young = g, .workers = workers};
}
struct node { offset left, right; std::uint64_t id; };
struct fixture {
  jam::heap storage;
  jam::heap::host host;
  std::size_t alignment = 8;
  explicit fixture(unsigned workers) : storage(options(workers)), host(storage, page_words) {}
  node & at(offset p) noexcept { return *reinterpret_cast<node *>(&storage[p]); }
  offset make(generation g, std::uint64_t id, offset next = 0) noexcept {
    auto p = host.allocate(g, 2);
    at(p) = {next, 0, id};
    return p;
  }
  void trace(offset p, bool old_owners = false, unsigned workers = 1) noexcept {
    auto const caller = std::this_thread::get_id();
    host.trace({&p, 1}, [&](jam::heap::visitor & visit, offset q) noexcept {
      if (workers == 1) check(std::this_thread::get_id() == caller, "serial trace stays on caller");
      if (!(old_owners && !(q & young_bit)) && !visit.claim(q, 2, alignment)) return;
      visit.field(q, 0); visit.field(q, 1);
    }, workers, old_owners);
  }
};
void cycles(unsigned workers) {
  fixture f(workers);
  static_cast<void>(f.make(generation::old, 999));
  auto old = f.make(generation::old, 1);
  for (unsigned i = 0; i != 32; ++i) {
    static_cast<void>(f.make(generation::young, 999));
    auto young = f.make(generation::young, i + 10, old);
    f.at(young).right = young;
    f.at(old).left = young;
    auto const origin = f.storage.old().start();
    f.host.begin(true);
    f.trace(old);
    check(f.host.marked(old) && !f.host.marked(young), "minor skips ordinary old roots");
    f.trace(old, true);
    check(f.host.marked(young), "dirty owner scan reaches young");
    bool const promote = i % 3 == 2;
    check(f.host.prepare(promote), "minor fits");
    auto next = f.host.forward(young);
    check(f.host.forward(old) == old, "minor preserves old offsets");
    f.at(old).left = next; // Host owns remembered-slot repair.
    f.host.finish();
    young = next;
    check(f.storage.old().start() == origin, "minor preserves old origin");
    check(bool(young & young_bit) != promote, "promotion changes generation");
    check(f.at(young).id == i + 10 && f.at(young).left == old && f.at(young).right == young,
          "minor repairs cross-generation and self edges");
    check(f.storage.young().used() == page_words + (promote ? 0 : 2), "guard never promotes");
    f.host.begin(false);
    f.trace(old, false, workers);
    check(f.host.prepare(), "major fits");
    old = f.host.forward(old); young = f.host.forward(young);
    f.host.finish();
    check(f.at(old).left == young && f.at(young).left == old && f.at(young).right == young,
          "major preserves both forwarding tables until both moves complete");
    f.at(old).left = 0;
  }
}
void retry_and_subdivision(std::size_t alignment) {
  fixture f(4);
  f.alignment = alignment;
  auto const stride = static_cast<offset>(alignment == 8 ? 2 : alignment / 8);
  // One raw TLAB allocation contains many individually claimed objects.
  auto const count = page_words * 6 / stride;
  auto old = f.host.allocate(generation::old, count * stride);
  auto young = f.host.allocate(generation::young, count * stride);
  for (offset i = 0; i != count; ++i) {
    f.at(old + stride*i) = {i ? old + stride*i - stride : 0, 0, i};
    f.at(young + stride*i) = {i ? young + stride*i - stride : 0, 0, i};
  }
  old += stride*(count - 1); young += stride*(count - 1);
  f.host.begin(false);
  f.trace(old, false, 4); f.trace(young, false, 4);
  check(f.host.prepare(), "independent major fits despite combined live size");
  old = f.host.forward(old); young = f.host.forward(young);
  f.host.finish();
  f.host.begin(true); f.trace(young, false, 4);
  check(!f.host.prepare(true), "promotion reports insufficient old capacity");
  check(f.host.marked(young), "failed promotion preserves liveness");
  check(f.host.prepare(false), "retaining retry succeeds");
  young = f.host.forward(young); f.host.finish();
  for (auto p : {old, young}) {
    for (auto i = count; i; --i) {
      check(p && f.at(p).id == std::uint64_t(i - 1), "retry preserves graph");
      check((p & ~young_bit) % (alignment / 8) == 0, "retry preserves alignment");
      p = f.at(p).left;
    }
    check(!p, "chain terminates");
  }
}
void external_targets() {
  fixture f(1);
  auto live = f.make(generation::young, 42);
  auto dead = f.make(generation::young, 99);
  auto owner = f.make(generation::young, 7, live);
  f.at(owner).right = dead;
  f.host.begin(false);
  f.host.trace({&owner, 1}, [&](jam::heap::visitor & visit, offset p) noexcept {
    if (!visit.claim(p, 2)) return;
    f.storage.pointer(p, 0); f.storage.pointer(p, 1);
    visit.target(f.at(p).left); // Weak right field is declared, never followed.
  });
  check(f.host.marked(live) && !f.host.marked(dead), "target and weak declaration differ");
  check(f.host.prepare(), "external prepare");
  owner = f.host.forward(owner); live = f.host.forward(live);
  check(f.host.forward(dead) == 0, "dead external handle clears");
  f.host.finish();
  check(f.at(owner).left == live && !f.at(owner).right, "weak field clears during relocation");
}
#if !defined(_WIN32)
void rejects_cpp_weak_roots() {
  auto const child = ::fork();
  check(child >= 0, "fork host exclusion check");
  if (!child) {
    ::rlimit const no_core{0, 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &no_core));
    fixture f(1);
    jam::heap_scope binding{f.storage};
    auto weak = f.storage.weak_root(jam::ptr<unsigned>{f.host.allocate(generation::young, 1)});
    f.host.begin(false);
    ::_exit(0);
  }
  int status = 0;
  check(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
        "host rejects ordinary weak root registrations");
}
void publication() {
  auto * target = static_cast<std::byte *>(::mmap(nullptr, JAM_PAGE_BYTES * 9, PROT_NONE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  check(target != MAP_FAILED, "reserve canonical view");
  {
    fixture f(1);
    for (unsigned i = 0; i != 24; ++i) {
      f.host.publish(generation::young, target, page_words, page_words * 9);
      static_cast<void>(f.make(generation::young, 999));
      auto p = f.make(generation::young, 42);
      auto * published = reinterpret_cast<node *>(target + ((p & ~young_bit) - page_words) * 8);
      check(published->id == 42, "private writes visible in canonical alias");
      published->id = 84;
      check(f.at(p).id == 84, "canonical writes visible in private view");
      f.host.begin(true); f.trace(p); check(f.host.prepare(), "published minor");
      p = f.host.forward(p); f.host.finish();
      f.host.publish(generation::young, target, page_words, page_words * 9);
      published = reinterpret_cast<node *>(target + ((p & ~young_bit) - page_words) * 8);
      check(published->id == 84, "republished alias follows rotated backing");
    }
  }
  check(!::munmap(target, JAM_PAGE_BYTES * 9), "host releases reservation after heap destruction");
}
#endif
int main() {
  cycles(1); cycles(4); retry_and_subdivision(8); retry_and_subdivision(64); external_targets();
#if !defined(_WIN32)
  publication(); rejects_cpp_weak_roots();
#endif
  std::puts("host phases, promotion retry, tracing and publication passed");
}
