// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
import jam;
import jam.mapping;

void check(bool ok, char const * message) noexcept {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
void mapping_aliases() {
  using jam::detail::heap_mapping;
  auto const page = heap_mapping::page_size();
  heap_mapping source{4 * page};
  source.data()[page + 17] = std::byte{42};
  auto pin = source.alias_run(page, 2 * page);
  auto overlap = source.alias_run(page, page);
  auto * address = pin.data() + 17;
  check(*address == std::byte{42}, "side alias shares source bytes");
  overlap.data()[17] = std::byte{43};
  check(*address == std::byte{43}, "overlapping pins share backing");
  heap_mapping destination{4 * page};
  destination.data()[3 * page] = std::byte{7};
  destination.replace_pages(pin, 0, 0, 2 * page);
  check(destination.data()[17] == std::byte{43}, "canonical destination aliases pin");
  check(destination.data()[3 * page] == std::byte{7}, "replacement preserves unrelated pages");
  heap_mapping fresh{4 * page};
  source.replace(fresh);
  *address = std::byte{44};
  check(destination.data()[17] == std::byte{44}, "pin survives source mapping replacement");
  check(overlap.data()[17] == std::byte{44}, "overlapping alias survives source replacement");
  auto wrapped = destination.alias_run(3 * page, 2 * page);
  check(wrapped.data()[0] == std::byte{7} && wrapped.data()[page + 17] == std::byte{44},
        "side alias supports a circular source run");
}

using offset = jam::heap::offset;
using generation = jam::heap::host::generation;
constexpr offset young_bit = 0x80000000u;
constexpr std::size_t page_words = JAM_PAGE_BYTES / 8;
struct node { offset strong, weak; std::uint64_t value; };

void pinned_collection(bool minor, bool promote) {
  jam::generation_options g{.capacity = jam::units::pages{12}, .reserve = jam::units::pages{2},
                           .maximum = jam::units::pages{12}, .shrink_shift = 0};
  jam::heap storage{{.old = g, .young = g, .workers = 1}};
  jam::heap::host host{storage, page_words};
  auto at = [&](offset p) noexcept -> node & { return *reinterpret_cast<node *>(&storage[p]); };
  auto make = [&](std::uint64_t value) noexcept {
    auto const p = host.allocate(generation::young, 2);
    at(p) = {0, 0, value};
    return p;
  };
  static_cast<void>(host.allocate(generation::young, page_words * 2));
  auto dead = make(99);
  auto first = make(42);
  auto gap = make(999);
  auto second = make(43);
  at(dead).strong = at(gap).strong = 0x7fffffffu; // Never traced, never interpreted as pointers.
  at(first).weak = dead;
  at(first).strong = second;
  at(second).weak = gap;
  auto * p = host.pin_object(first, 2);
  auto * duplicate = host.pin_object(first, 2);
  auto * q = host.pin_object(second, 2);
  auto * const address = static_cast<node *>(p->address());
  auto * const other = static_cast<node *>(q->address());
  check(p == duplicate, "duplicate pins reuse registration");
  for (unsigned pass = 0; pass != 3; ++pass) {
    host.begin(pass == 0 && minor);
    std::array<offset, 2> roots{p->position(), q->position()};
    host.trace(roots, [&](jam::heap::visitor & visit, offset r) noexcept {
      if (!visit.claim(r, 2)) return;
      storage.pointer(r, 0); storage.pointer(r, 1);
      visit.target(at(r).strong);
    });
    if (pass == 0) check(!host.marked(dead) && !host.marked(gap), "padding objects are initially dead");
    check(host.prepare(pass == 0 && promote), "pinned collection fits");
    if (pass == 0) {
      check(!host.forward(dead) && !host.forward(gap), "retained padding does not resurrect weak targets");
      check(address->weak == dead && other->weak == gap, "prepare preserves source fields");
    }
    auto const next = host.forward(p->position()), next_other = host.forward(q->position());
    host.finish();
    check(p->position() == next && q->position() == next_other, "registration follows canonical movement");
    check(p->address() == address && q->address() == other, "native aliases stay fixed");
    check(address->value == 42 + pass && other->value == 43, "pinned payload survives remapping");
    check(!address->weak && !other->weak, "weak pointers into retained gaps clear");
    check(address->strong == next_other, "pinned reference field follows live target");
    check((next & ~young_bit) % page_words == (first & ~young_bit) % page_words,
          "pin preserves physical page residue");
    ++address->value;
    check(at(next).value == address->value, "native writes reach relocated canonical object");
  }
  host.unpin(duplicate);
  check(p->address() == address, "one duplicate close preserves pin");
  host.unpin(p); host.unpin(q);
}
void pinned_boundaries() {
  jam::generation_options g{.capacity = jam::units::pages{16}, .reserve = jam::units::pages{2},
                           .maximum = jam::units::pages{16}, .shrink_shift = 0};
  jam::heap storage{{.old = g, .young = g, .workers = 1}};
  jam::heap::host host{storage, page_words};
  std::array<std::size_t, 8> lengths{page_words - 3, 13, page_words + 9, 9, 65, 3, page_words - 7, 11};
  std::array<offset, 8> roots{};
  for (unsigned i = 0; i < roots.size(); ++i) {
    roots[i] = host.allocate(generation::young, lengths[i]);
    storage[roots[i]] = lengths[i];
    for (std::size_t j = 1; j < lengths[i]; ++j) storage[roots[i] + j] = i * 100000 + j;
  }
  std::array<unsigned, 3> selected{1, 2, 5};
  std::array<jam::heap::host::pin *, 3> pins{};
  std::array<void *, 3> addresses{};
  for (unsigned i = 0; i < pins.size(); ++i) {
    pins[i] = host.pin_object(roots[selected[i]], lengths[selected[i]]);
    addresses[i] = pins[i]->address();
  }
  roots[0] = roots[3] = roots[6] = 0;
  for (unsigned pass = 0; pass != 4; ++pass) {
    host.begin(pass == 1 || pass == 2);
    host.trace(roots, [&](jam::heap::visitor & visit, offset at) noexcept {
      static_cast<void>(visit.claim(at, storage[at]));
    });
    check(host.prepare(pass == 2), "cross-page pin collection fits");
    for (auto & at : roots) at = host.forward(at);
    host.finish();
    for (unsigned i = 0; i < roots.size(); ++i) if (roots[i]) {
      check(storage[roots[i]] == lengths[i], "record length survives adjacent page remaps");
      for (std::size_t j = 1; j < lengths[i]; ++j)
        check(storage[roots[i] + j] == i * 100000 + j, "cross-page record payload remains contiguous");
    }
    for (unsigned i = 0; i < pins.size(); ++i) {
      check(pins[i]->position() == roots[selected[i]] && pins[i]->address() == addresses[i],
            "overlapping cross-page pins retain addresses and identity");
    }
    for (auto const & gap : host.gaps()) for (unsigned i = 0; i < roots.size(); ++i) if (roots[i])
      check(std::uint64_t(gap.at) + gap.words <= roots[i] || std::uint64_t(roots[i]) + lengths[i] <= gap.at,
            "filler gap never overlaps a surviving record");
  }
  for (auto * pin : pins) host.unpin(pin);
}
void pinned_retry() {
  jam::generation_options g{.capacity = jam::units::pages{12}, .reserve = jam::units::pages{2},
                           .maximum = jam::units::pages{12}, .shrink_shift = 0};
  jam::heap storage{{.old = g, .young = g, .workers = 1}};
  jam::heap::host host{storage, page_words};
  static_cast<void>(host.allocate(generation::old, page_words * 8));
  auto const dead = host.allocate(generation::young, 2);
  auto pinned = host.allocate(generation::young, 2);
  auto bulk = host.allocate(generation::young, page_words * 5);
  storage[pinned] = 2; storage[pinned + 1] = 42;
  storage[bulk] = page_words * 5;
  auto * pin = host.pin_object(pinned, 2);
  std::array roots{pinned, bulk};
  host.begin(true);
  host.trace(roots, [&](jam::heap::visitor & visit, offset at) noexcept {
    static_cast<void>(visit.claim(at, storage[at]));
  });
  check(!host.prepare(true), "pinned promotion reports insufficient capacity");
  check(host.marked(pinned) && !host.marked(dead), "retry retains exact liveness despite expanded padding");
  check(host.prepare(false), "pinned retry retains nursery");
  check(!host.forward(dead), "dead padding remains dead after retry");
  pinned = host.forward(pinned); bulk = host.forward(bulk);
  host.finish();
  check(pin->position() == pinned && storage[pinned + 1] == 42 && storage[bulk] == page_words * 5,
        "retaining retry preserves pin and movable neighbor");
  host.unpin(pin);
}
int main() {
  mapping_aliases();
  pinned_boundaries();
  pinned_retry();
  pinned_collection(false, false);
  pinned_collection(true, false);
  pinned_collection(true, true);
}
