// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <cstdint>
#include <cstdio>
#include <cstdlib>

import jam;

// User-land policy, not a Jam pointer kind. T is the common representation base,
// so replacing a thunk with a terminal value never changes the slot's type.
template<class T>
class lifted_ptr {
  jam::ptr<T> position;
public:
  explicit lifted_ptr(jam::ptr<T> value = nullptr) noexcept : position(value) {}
  [[nodiscard]] jam::ptr<T> get() const noexcept { return position; }
  void trace(jam::visitor auto & visit) const noexcept { visit(position); }

  // Mutator operation: resolve never allocates, collects, or forces the target.
  // Preserve the original reference on a cycle; no arbitrary chain-depth limit.
  [[nodiscard]] jam::ptr<T> resolve() noexcept {
    auto step = [](jam::ptr<T> p) noexcept { return p ? p->resolve() : nullptr; };
    auto slow = step(position), fast = step(step(position));
    while (slow && fast) {
      if (slow == fast) return position;
      slow = step(slow);
      fast = step(step(fast));
    }
    auto at = position;
    while (auto next = step(at)) at = next;
    position = at; // Ordinary assignment supplies the old-to-young barrier.
    return position;
  }
};

struct lifted {
  virtual jam::ptr<lifted> resolve() const noexcept = 0;
  virtual void claim_and_trace(jam::heap::visitor &) const noexcept = 0;
};

// Int is the terminal representation; its payload is an unboxed integer.
struct alignas(32) integer final : lifted {
  int value;
  explicit integer(int n) noexcept : value(n) {}
  jam::ptr<lifted> resolve() const noexcept override { return nullptr; }
  void claim_and_trace(jam::heap::visitor & visit) const noexcept override {
    static_cast<void>(visit.claim_target(this));
  }
};

struct thunk final : lifted {
  jam::ptr<lifted> answer;
  explicit thunk(jam::ptr<lifted> p = nullptr) noexcept : answer(p) {}
  jam::ptr<lifted> resolve() const noexcept override { return answer; }
  void claim_and_trace(jam::heap::visitor & visit) const noexcept override {
    if (visit.claim_target(this)) visit(answer);
  }
};

struct holder {
  lifted_ptr<lifted> value;
  explicit holder(jam::ptr<lifted> p) noexcept : value(p) {}
  void trace(jam::visitor auto & visit) const noexcept { visit(value); }
};

static void check(bool ok, char const * message) noexcept {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}

static void exercise(unsigned workers) {
  jam::heap heap{{.workers = workers}};
  jam::heap_scope scope{heap};
  lifted_ptr<lifted> empty;
  check(!empty.resolve(), "null stays null");
  jam::root<thunk> pending = jam::mk<thunk>();
  jam::root<holder> handle = jam::mk<holder>(pending.get());
  check(handle->value.resolve() == pending.get(), "unresolved computation stays opaque");
  // Promote the owner and thunk, then publish a young terminal.
  heap.collect_minor(true);
  static_cast<void>(jam::mk<integer>(-1)); // Dead gap forces the terminal to move.
  pending->answer = jam::mk<integer>(42);
  check(handle->value.resolve() == pending->answer, "terminal replaces thunk in the same typed slot");
  pending->answer = nullptr; // Only the old custom slot now retains the young value.
  for (unsigned i = 0; i != 3; ++i) {
    if (i == 0) heap.collect_minor();
    else if (i == 1) heap.collect_minor(true);
    else heap.collect_major();
    auto const p = handle->value.get();
    check(static_cast<integer const *>(p.operator->())->value == 42, "resolved slot survives minor and major forwarding");
    check(handle->value.resolve() == p, "normalization is idempotent");
  }

  // The handle is the only strong root of this chain; normalization discards it.
  jam::weak_root<thunk> obsolete;
  {
    auto tail = jam::mk<thunk>(handle->value.get());
    obsolete = tail;
    jam::ptr<lifted> chain = tail;
    for (unsigned i = 0; i != 256; ++i) chain = jam::mk<thunk>(chain);
    handle = jam::mk<holder>(chain);
  }
  static_cast<void>(handle->value.resolve());
  heap.collect_major();
  check(!obsolete.lock(), "replaced chain is reclaimable");
  check(static_cast<integer const *>(handle->value.get().operator->())->value == 42, "chain reaches its terminal");

  jam::root<thunk> a = jam::mk<thunk>();
  jam::root<thunk> b = jam::mk<thunk>(a.get());
  a->answer = b.get();
  handle = jam::mk<holder>(a.get());
  check(handle->value.resolve() == a.get(), "cycle preserves original identity");
  heap.collect_major();
  check(handle->value.resolve() == a.get(), "cycle remains traceable after compaction");
  a->answer = a.get();
  check(handle->value.resolve() == a.get(), "self-indirection terminates");
}

int main() {
  exercise(1);
  exercise(4);
}
