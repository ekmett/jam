// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include "jam_vm.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr uint32_t young_bit = 0x80000000u;
static void check(bool value, char const * message) {
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
struct object { uint64_t header; uint32_t left, right; uint64_t identity; };
struct heap {
  size_t page = static_cast<size_t>(getpagesize());
  size_t bytes = page * 8;
  size_t span = (1ull << 34) + page + bytes;
  void * base = mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  jam_vm * vm;
  explicit heap(size_t workers) {
    check(base != MAP_FAILED, "sparse canonical reservation");
    vm = jam_vm_create(base, page, bytes, bytes, page * 2, workers);
  }
  ~heap() { jam_vm_destroy(vm); check(!munmap(base, span), "release reservation"); }
  object & at(uint32_t o) { return *reinterpret_cast<object *>(static_cast<char *>(base) + 8ull * o); }
  uint32_t make(bool young, uint64_t id, uint32_t left = 0, uint32_t right = 0) {
    auto o = jam_vm_allocate(vm, 3, young);
    check(o && bool(o & young_bit) == young, "allocation generation encoding");
    at(o) = {0x8000000100000800ull, left, right, id};
    return o;
  }
  static void scan(void * ctx, jam_vm_visit * v, uint32_t o) {
    auto & h = *static_cast<heap *>(ctx);
    if (!jam_vm_claim(v, o, 3)) return;
    check(h.at(o).header == 0x8000000100000800ull, "opaque JVM header intact");
    uint64_t fields[] = {uint64_t(o) * 2 + 2, uint64_t(o) * 2 + 3};
    jam_vm_fields(v, fields, 2, 1);
  }
  void trace(uint32_t o) { jam_vm_trace(vm, &o, 1, scan, this, 1); }
  void weak_close() { jam_vm_weak_close(vm, scan, this); jam_vm_weak_finalizers(vm, scan, this); }
};
static void cycles(size_t workers, bool require_simd) {
  heap h(workers);
  if (require_simd) check(std::strcmp(jam_vm_compactor(h.vm), "baseline") != 0, "SIMD selected");
  auto old = h.make(false, 1);
  // Every third cycle promotes; others rotate only young. Cross-generation edges
  // and both halves of compressed fields are rewritten by jam's existing kernel.
  for (unsigned n = 0; n != 40; ++n) {
    h.make(true, 999);
    auto young = h.make(true, n + 10, old);
    h.at(young).right = young;
    h.at(old).left = young;
    auto const old_origin = jam_vm_origin(h.vm, 0);
    auto const old_before = old;
    bool promote = n % 3 == 2;
    jam_vm_begin(h.vm, 1);
    h.trace(old); // Ordinary old roots must not cause a scan of the whole old graph.
    check(!jam_vm_marked(h.vm, young), "minor old root is not recursively traversed");
    jam_vm_trace_old(h.vm, &old, 1, heap::scan, &h);
    check(jam_vm_marked(h.vm, young), "remembered owner traces young");
    check(jam_vm_prepare(h.vm, promote), "minor prepare");
    old = jam_vm_forward(h.vm, old);
    young = jam_vm_forward(h.vm, young);
    h.at(old).left = young; // Host repairs old fields before movement.
    jam_vm_finish(h.vm);
    check(old == old_before && jam_vm_origin(h.vm, 0) == old_origin, "minor preserves old addresses and ring");
    check(bool(young & young_bit) != promote, "promotion changes generation bit exactly");
    check(h.at(young).identity == n + 10 && h.at(young).left == old && h.at(young).right == young,
          "young payload and cross-generation/self pointers");
    check(jam_vm_used(h.vm, 1) == h.page / 8 + (promote ? 0 : 3), "young guard retained but never promoted");
    // A major traces a mixed cycle and compacts both independently.
    jam_vm_begin(h.vm, 0);
    h.trace(old);
    check(jam_vm_prepare(h.vm, 0), "major prepare");
    old = jam_vm_forward(h.vm, old);
    young = jam_vm_forward(h.vm, young);
    jam_vm_finish(h.vm);
    check(h.at(old).left == young && h.at(young).left == old && h.at(young).right == young,
          "both forwarding tables survive both moves");
    h.at(old).left = 0;
  }
  std::printf("both generations: workers=%zu compactor=%s, 40 mixed cycles passed\n", workers, jam_vm_compactor(h.vm));
}
static void capacity_and_retry() {
  heap h(4);
  // Combined live bytes exceed either generation. Major must not promote all.
  size_t count = h.bytes / sizeof(object) * 3 / 4;
  uint32_t old = 0, young = 0;
  for (size_t n = 0; n != count; ++n) { old = h.make(false, n, old); young = h.make(true, n, young); }
  uint32_t roots[] = {old, young};
  jam_vm_begin(h.vm, 0);
  jam_vm_trace(h.vm, roots, 2, heap::scan, &h, 4);
  check(jam_vm_prepare(h.vm, 0), "major fits combined live data exceeding old capacity");
  old = jam_vm_forward(h.vm, old); young = jam_vm_forward(h.vm, young);
  jam_vm_finish(h.vm);
  auto id = jam_vm_weak_create(h.vm, old, young, 0);
  jam_vm_begin(h.vm, 1);
  h.weak_close(); // Old key retains the complete young chain without a young root.
  check(!jam_vm_prepare(h.vm, 1), "promotion refuses insufficient old capacity");
  check(jam_vm_weak_value(h.vm, id) == young, "failed prepare does not forward weak registry");
  check(jam_vm_marked(h.vm, young), "failed prepare remains marking phase");
  check(jam_vm_prepare(h.vm, 0), "failed promotion retries retaining minor");
  young = jam_vm_forward(h.vm, young);
  jam_vm_finish(h.vm);
  check(jam_vm_weak_value(h.vm, id) == young && (young & young_bit), "retaining minor preserves young weak value");
  auto p = young;
  for (size_t n = count; n; --n) { check(h.at(p).identity == n - 1, "whole chain survived retry"); p = h.at(p).left; }
  check(!p, "chain terminates");
  // A major can finally reject the unrooted old key; minor conservative liveness is scoped.
  jam_vm_begin(h.vm, 0); h.weak_close();
  check(!jam_vm_weak_value(h.vm, id), "old weak key dies on major");
  check(jam_vm_prepare(h.vm, 0), "empty major prepare"); jam_vm_finish(h.vm);
  check(jam_vm_used(h.vm, 0) == h.page / 8 && jam_vm_used(h.vm, 1) == h.page / 8, "both generations reclaimed");
}
static void mixed_weak_batch() {
  heap h(4);
  auto old_key = h.make(false, 1), young_key = h.make(true, 2);
  auto value = h.make(true, 3, young_key), finalizer = h.make(true, 4);
  auto w2 = jam_vm_weak_create(h.vm, young_key, finalizer, 0);
  auto w1 = jam_vm_weak_create(h.vm, old_key, value, 0);
  auto dead_key = h.make(true, 5), capture = h.make(true, 6, dead_key);
  auto w3 = jam_vm_weak_create(h.vm, dead_key, 0, capture);
  auto w4 = jam_vm_weak_create(h.vm, dead_key, finalizer, 0);
  jam_vm_begin(h.vm, 1); h.weak_close();
  check(jam_vm_weak_value(h.vm, w1) && jam_vm_weak_value(h.vm, w2), "mixed-generation weak fixed point");
  check(!jam_vm_weak_value(h.vm, w4) && jam_vm_marked(h.vm, dead_key), "retirement precedes captured key resurrection");
  check(jam_vm_prepare(h.vm, 1), "weak promotion prepare"); jam_vm_finish(h.vm);
  check(!(jam_vm_weak_value(h.vm, w1) & young_bit), "weak values follow promotion");
  uint64_t id;
  auto f = jam_vm_weak_take(h.vm, &id);
  check(id == w3 && f && !(f & young_bit), "pending finalizer follows promotion");
  jam_vm_begin(h.vm, 0); jam_vm_weak_roots(h.vm, heap::scan, &h); h.weak_close();
  check(jam_vm_marked(h.vm, f), "running promoted finalizer is a major root");
  check(jam_vm_prepare(h.vm, 0), "nested major prepare"); jam_vm_finish(h.vm);
  jam_vm_weak_complete(h.vm, id);
  check(!jam_vm_weak_finalize(h.vm, id), "completed finalizer cannot run twice");
}
int main(int argc, char **) { cycles(1, argc > 1); cycles(4, argc > 1); capacity_and_retry(); mixed_weak_batch(); }
