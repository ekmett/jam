// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <cstdint>
#include <new>
import jam.unqualified;

struct node {
  ptr<node> next;
  std::uint64_t data;
  static constexpr auto manifest = make_manifest<node>(&node::next);
};

int main() {
  heap heap{{.workers = 2}};
  heap_scope scope{heap};
  auto const a = mk<node>(nullptr, 42u);
  auto const b = mk<node>(a, 99u);
  a->next = b;
  root answer = a;
  collect_major();
  return answer->data != 42 || answer->next->data != 99 || answer->next->next != answer.get();
}
