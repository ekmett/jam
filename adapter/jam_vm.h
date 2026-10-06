// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef struct jam_vm jam_vm;
typedef struct jam_vm_visit jam_vm_visit;
typedef void (*jam_vm_scan)(void *, jam_vm_visit *, uint32_t);

/* VM owns a PROT_NONE reservation through base+16GiB+prefix+young_bytes.
 * Old objects start at base+prefix; young objects at base+16GiB+prefix.
 * Both prefixes remain inaccessible. Sizes exclude guards and compaction reserve.
 * All sizes are native-page multiples; pointers use nonzero-base shift 3.
 * Heap operations require stopped mutators or the VM's allocation lock.
 * The reservation must outlive the adapter and must be unmapped by the VM. */
jam_vm * jam_vm_create(void * base, size_t prefix, size_t old_bytes,
                      size_t young_bytes, size_t reserve_bytes, size_t workers);
void jam_vm_destroy(jam_vm *);
uint32_t jam_vm_allocate(jam_vm *, size_t words, int young); /* zero on insufficient space */
size_t jam_vm_used(jam_vm const *, int young); /* cells, including guard prefix */
size_t jam_vm_origin(jam_vm const *, int young); /* private ring origin */
char const * jam_vm_compactor(jam_vm const *);

/* begin -> any number of trace/liveness operations -> prepare -> root repairs
 * using forward -> finish. finish republishes the stable alias. VM root repair
 * must happen before finish (including code relocations and derived pointers). */
void jam_vm_begin(jam_vm *, int minor);
void jam_vm_trace(jam_vm *, uint32_t const * roots, size_t count,
                  jam_vm_scan, void * context, size_t marker_workers);
/* Minor-only, serial scan of distinct dirty old owners. Ordinary trace skips old
 * roots during a minor. The VM deduplicates owners before calling trace_old. */
void jam_vm_trace_old(jam_vm *, uint32_t const * owners, size_t count,
                      jam_vm_scan, void * context);
int jam_vm_marked(jam_vm const *, uint32_t);
/* A failed promotion leaves the marking phase intact; retry with promote=0.
 * Major collection preserves both generations and ignores promote. */
int jam_vm_prepare(jam_vm *, int promote);
uint32_t jam_vm_forward(jam_vm const *, uint32_t);
void jam_vm_finish(jam_vm *);

/* Generalized weak associations are VM side metadata, not strong JNI handles.
 * Dropping the guest token does not cancel finalization. A finalizer is itself
 * a managed object; only its actual outgoing edges can retain the key.
 * IDs are stable and never reused. All mutator operations require the VM lock.
 * close reaches the least live-key fixed point, then retires the whole dead
 * batch before finalizers are traced. Java weak clearing occurs between close
 * and weak_finalizers; Java phantom processing follows weak_finalizers. */
uint64_t jam_vm_weak_create(jam_vm *, uint32_t key, uint32_t value, uint32_t finalizer);
uint32_t jam_vm_weak_value(jam_vm const *, uint64_t id);
void jam_vm_weak_roots(jam_vm *, jam_vm_scan, void * context);
void jam_vm_weak_close(jam_vm *, jam_vm_scan, void * context);
void jam_vm_weak_finalizers(jam_vm *, jam_vm_scan, void * context);
/* take transitions queued -> running; complete releases that finalizer root.
 * Explicit finalize also retires an active entry and transitions to running.
 * Finalizers run in the language runtime, after the collector safepoint. */
uint32_t jam_vm_weak_take(jam_vm *, uint64_t * id);
uint32_t jam_vm_weak_finalize(jam_vm *, uint64_t id);
void jam_vm_weak_complete(jam_vm *, uint64_t id);

/* Scan callback claims the entire object before declaring fields. Slot indices
 * are absolute four-byte offsets from the compressed-oop base. Batch fields to
 * avoid a foreign call per field. Weak fields are declared with follow=0 and
 * processed by VM policy before prepare. No callbacks may unwind across C ABI. */
int jam_vm_claim(jam_vm_visit *, uint32_t object, size_t words);
void jam_vm_targets(jam_vm_visit *, uint32_t const * targets, size_t count);
void jam_vm_fields(jam_vm_visit *, uint64_t const * slots, size_t count, int follow);

#ifdef __cplusplus
}
#endif
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
