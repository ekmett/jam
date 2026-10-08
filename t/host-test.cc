// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
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
    if (host.tracks_starts()) host.record_start(p);
    return p;
  }
  void trace(offset p, bool old_owners = false, unsigned workers = 1) noexcept {
    auto const caller = std::this_thread::get_id();
    host.trace({&p, 1}, [&](jam::heap::visitor & visit, offset q) noexcept {
      if (workers == 1) check(std::this_thread::get_id() == caller, "serial trace stays on caller");
      if (!(old_owners && !(q & young_bit)) && !visit.claim(q, 2, alignment)) return;
      if (host.tracks_starts()) host.record_start(q);
      visit.field(q, 0); visit.field(q, 1);
    }, workers, old_owners);
  }
};
void tracked_starts(unsigned workers) {
  fixture f(workers);
  check(f.host.starts(generation::old).empty() && f.host.starts(generation::young).empty(),
        "ordinary host allocates no start bitmap");
  // Enable after allocation: the next major discovers existing starts.
  static_cast<void>(f.make(generation::old, 99));
  auto young = f.make(generation::young, 42);
  auto old = f.make(generation::old, 41, young);
  f.host.track_starts();
  f.host.track_starts(); // Enabling twice must not replace the side channel.
  auto verify = [&] {
    unsigned count = 0;
    for (auto which : {generation::old, generation::young})
      for (auto bits : f.host.starts(which)) count += std::popcount(bits);
    check(count == 2, "only actual live objects have start bits");
    for (auto at : {old, young}) {
      auto bits = f.host.starts(at & young_bit ? generation::young : generation::old);
      auto local = at & ~young_bit;
      check((bits[local / 32] >> (local % 32)) & 1u, "start bit follows its object");
    }
  };
  f.host.begin(false); f.trace(old, false, workers);
  check(f.host.prepare(), "tracked major fits");
  old = f.host.forward(old); young = f.host.forward(young);
  f.host.finish(); verify();
  for (bool promote : {false, true}) {
    static_cast<void>(f.make(generation::young, 999));
    f.host.begin(true); f.trace(young, false, workers);
    check(f.host.prepare(promote), "tracked minor fits");
    young = f.host.forward(young);
    f.at(old).left = young;
    f.host.finish(); verify();
  }
  f.host.begin(false);
  check(f.host.prepare(), "empty tracked major fits");
  f.host.finish();
  for (auto which : {generation::old, generation::young})
    for (auto bits : f.host.starts(which)) check(!bits, "dead objects leave no stale starts");
}
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
#endif
void inaccessible(std::byte * address) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION region{};
  check(::VirtualQuery(address, &region, sizeof(region)) != 0, "query canonical reservation");
  check(region.State == MEM_RESERVE, "guard and unpublished pages remain reserved and inaccessible");
#else
  auto const child = ::fork();
  check(child != -1, "start guard probe");
  if (child == 0) {
    rlimit const limit{0, 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &limit));
    auto const value = *reinterpret_cast<volatile unsigned char *>(address);
    static_cast<void>(value);
    ::_exit(0);
  }
  int status = 0;
  pid_t finished;
  do { finished = ::waitpid(child, &status, 0); } while (finished == -1 && errno == EINTR);
  check(finished == child && WIFSIGNALED(status) &&
    (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS),
    "guard and unpublished pages remain inaccessible");
#endif
}
void publication() {
  constexpr auto page = JAM_PAGE_BYTES;
  // The young window ends exactly at the reservation's end: its initial
  // publication must still split the placeholder prefix.
  constexpr auto bytes = page * 22;
#if defined(_WIN32)
  auto * base = static_cast<std::byte *>(::VirtualAlloc2(nullptr, nullptr, bytes,
    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
  check(base != nullptr, "reserve canonical placeholders");
  // A host-owned mapping separates the two windows. Jam must leave it intact.
  check(::VirtualFree(base, page * 11, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "split sentinel prefix");
  check(::VirtualFree(base + page * 11, page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), "split sentinel suffix");
  auto const sentinel = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, page, nullptr);
  check(sentinel != nullptr, "create unrelated backing");
  check(::MapViewOfFile3(sentinel, ::GetCurrentProcess(), base + page * 11, 0, page,
    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == base + page * 11, "map unrelated backing");
#else
  auto * base = static_cast<std::byte *>(::mmap(nullptr, bytes, PROT_NONE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  check(base != MAP_FAILED, "reserve canonical views");
  check(!::mprotect(base + page * 11, page, PROT_READ | PROT_WRITE), "commit unrelated page");
#endif
  auto * const old_target = base + page;
  auto * const target = base + page * 13;
  auto * const unrelated = reinterpret_cast<std::uint64_t *>(base + page * 11);
  *unrelated = 123456;
  {
    fixture f(1);
    static_cast<void>(f.make(generation::old, 999));
    auto old = f.make(generation::old, 123);
    f.host.publish(generation::old, old_target, page_words, page_words * 9);
    for (unsigned i = 0; i != 24; ++i) {
      f.host.publish(generation::young, target, page_words, page_words * 9);
      static_cast<void>(f.make(generation::young, 999));
      auto p = f.make(generation::young, 42);
      auto * published = reinterpret_cast<node *>(target + ((p & ~young_bit) - page_words) * 8);
      check(published->id == 42, "private writes visible in canonical alias");
      published->id = 84;
      check(f.at(p).id == 84, "canonical writes visible in private view");
      bool const minor = i % 4 != 3;
      f.host.begin(minor); f.trace(p); f.trace(old);
      check(f.host.prepare(), "published collection");
      p = f.host.forward(p); old = f.host.forward(old); f.host.finish();
      f.host.publish(generation::young, target, page_words, page_words * 9);
      if (!minor) f.host.publish(generation::old, old_target, page_words, page_words * 9);
      published = reinterpret_cast<node *>(target + ((p & ~young_bit) - page_words) * 8);
      check(published->id == 84, "republished alias follows rotated backing");
      check(reinterpret_cast<node *>(old_target + (old - page_words) * 8)->id == 123,
        "old publication survives young replacement and follows major rotation");
      check(*unrelated == 123456, "publication preserves unrelated host mappings");
    }
    f.host.unpublish(target, page_words * 9);
    f.host.publish(generation::young, target, page_words, page_words * 9);
    check(reinterpret_cast<node *>(target)->id == 84, "an unpublished window can be published again");
    f.host.unpublish(target, page_words * 9);
    f.host.unpublish(old_target, page_words * 9);
  }
  // Workers have stopped before POSIX guard probes fork.
  for (auto index : {0, 1, 9, 10, 12, 13, 21}) inaccessible(base + index * page);
  check(*unrelated == 123456, "unpublication preserves unrelated host mappings");
#if defined(_WIN32)
  check(::UnmapViewOfFile2(::GetCurrentProcess(), unrelated, MEM_PRESERVE_PLACEHOLDER), "restore unrelated placeholder");
  check(::CloseHandle(sentinel), "release unrelated backing");
  check(::VirtualFree(base, bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS), "coalesce host reservation");
  check(::VirtualFree(base, 0, MEM_RELEASE), "host releases reservation after heap destruction");
#else
  check(!::munmap(base, bytes), "host releases reservation after heap destruction");
#endif
}
int main() {
  tracked_starts(1); tracked_starts(4);
  cycles(1); cycles(4); retry_and_subdivision(8); retry_and_subdivision(64); external_targets();
  publication();
#if !defined(_WIN32)
  rejects_cpp_weak_roots();
#endif
  std::puts("host phases, promotion retry, tracing and publication passed");
}
