// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <jni.h>
#include "jam_vm_Weak.h"

#if defined(_WIN32)
static FARPROC vm_symbol(char const *name) {
  HMODULE module = GetModuleHandleW(L"jvm.dll");
  return module ? GetProcAddress(module, name) : NULL;
}
#else
static void *vm_symbol(char const *name) {
  return dlsym(RTLD_DEFAULT, name);
}
#endif

static void (JNICALL *weak_register_indirection)(JNIEnv *, jclass, jclass, jstring, jint, jstring);
static jlong (JNICALL *weak_create)(JNIEnv *, jclass, jobject, jobject, jobject);
static jobject (JNICALL *weak_deref)(JNIEnv *, jclass, jlong);
static jobject (JNICALL *weak_take)(JNIEnv *, jclass, jlongArray);
static jobject (JNICALL *weak_finalize)(JNIEnv *, jclass, jlong);
static void (JNICALL *weak_complete)(JNIEnv *, jclass, jlong);
static jlong (JNICALL *collections)(JNIEnv *, jclass, jint);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) return JNI_ERR;

  // Resolve at load time so a stock JVM reports a Java linkage error instead
  // of aborting on a missing lazy-bound JVM symbol at the first guest call.
  weak_register_indirection = (void (JNICALL *)(JNIEnv *, jclass, jclass, jstring, jint, jstring))
    vm_symbol("JVM_JamWeakRegisterIndirection");
  weak_create = (jlong (JNICALL *)(JNIEnv *, jclass, jobject, jobject, jobject))
    vm_symbol("JVM_JamWeakCreate");
  weak_deref = (jobject (JNICALL *)(JNIEnv *, jclass, jlong))
    vm_symbol("JVM_JamWeakDeref");
  weak_take = (jobject (JNICALL *)(JNIEnv *, jclass, jlongArray))
    vm_symbol("JVM_JamWeakTake");
  weak_finalize = (jobject (JNICALL *)(JNIEnv *, jclass, jlong))
    vm_symbol("JVM_JamWeakFinalize");
  weak_complete = (void (JNICALL *)(JNIEnv *, jclass, jlong))
    vm_symbol("JVM_JamWeakComplete");
  collections = (jlong (JNICALL *)(JNIEnv *, jclass, jint))
    vm_symbol("JVM_JamCollections");
  if (!weak_register_indirection || !weak_create || !weak_deref || !weak_take || !weak_finalize || !weak_complete || !collections) {
    jclass error = (*env)->FindClass(env, "java/lang/UnsatisfiedLinkError");
    if (error) (*env)->ThrowNew(env, error, "jam-vm requires a JVM exporting the Jam weak hooks");
    return JNI_ERR;
  }
  return JNI_VERSION_1_8;
}

JNIEXPORT void JNICALL Java_jam_vm_Weak_checkAvailable(JNIEnv *env, jclass klass) {
  (void)collections(env, klass, 2);
}

JNIEXPORT void JNICALL Java_jam_vm_Weak_registerIndirection(JNIEnv *env, jclass klass,
    jclass carrier, jstring state_field, jint completed_state, jstring referent_field) {
  weak_register_indirection(env, klass, carrier, state_field, completed_state, referent_field);
}

JNIEXPORT jlong JNICALL Java_jam_vm_Weak_create(JNIEnv *env, jclass klass,
                                               jobject key, jobject value, jobject finalizer) {
  return weak_create(env, klass, key, value, finalizer);
}

JNIEXPORT jobject JNICALL Java_jam_vm_Weak_deref(JNIEnv *env, jclass klass, jlong token) {
  return weak_deref(env, klass, token);
}

JNIEXPORT jobject JNICALL Java_jam_vm_Weak_take(JNIEnv *env, jclass klass, jlongArray token_out) {
  return weak_take(env, klass, token_out);
}

JNIEXPORT jobject JNICALL Java_jam_vm_Weak_finalizeNow(JNIEnv *env, jclass klass, jlong token) {
  return weak_finalize(env, klass, token);
}

JNIEXPORT void JNICALL Java_jam_vm_Weak_complete(JNIEnv *env, jclass klass, jlong token) {
  weak_complete(env, klass, token);
}
