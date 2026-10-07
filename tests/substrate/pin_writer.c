// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include "pin_writer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct writer {
  pthread_t thread;
  uint64_t * payload;
  uint64_t count;
  atomic_int stop;
};
static void * run(void * argument) {
  struct writer * writer = argument;
  uint64_t count = 0;
  while (!atomic_load_explicit(&writer->stop, memory_order_relaxed))
    __atomic_store_n(writer->payload, ++count, __ATOMIC_RELAXED);
  writer->count = count;
  return NULL;
}
void * jam_pin_start(void * payload) {
  struct writer * writer = calloc(1, sizeof(*writer));
  if (!writer) return NULL;
  writer->payload = payload;
  atomic_init(&writer->stop, 0);
  if (pthread_create(&writer->thread, NULL, run, writer)) { free(writer); return NULL; }
  return writer;
}
int64_t jam_pin_stop(void * argument) {
  struct writer * writer = argument;
  atomic_store_explicit(&writer->stop, 1, memory_order_relaxed);
  if (pthread_join(writer->thread, NULL)) abort();
  int64_t count = (int64_t) writer->count;
  free(writer);
  return count;
}
