// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <dlfcn.h>
#include <jni.h>
#include "jam_vm_Weak.h"

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
  weak_create = (jlong (JNICALL *)(JNIEnv *, jclass, jobject, jobject, jobject))
    dlsym(RTLD_DEFAULT, "JVM_JamWeakCreate");
  weak_deref = (jobject (JNICALL *)(JNIEnv *, jclass, jlong))
    dlsym(RTLD_DEFAULT, "JVM_JamWeakDeref");
  weak_take = (jobject (JNICALL *)(JNIEnv *, jclass, jlongArray))
    dlsym(RTLD_DEFAULT, "JVM_JamWeakTake");
  weak_finalize = (jobject (JNICALL *)(JNIEnv *, jclass, jlong))
    dlsym(RTLD_DEFAULT, "JVM_JamWeakFinalize");
  weak_complete = (void (JNICALL *)(JNIEnv *, jclass, jlong))
    dlsym(RTLD_DEFAULT, "JVM_JamWeakComplete");
  collections = (jlong (JNICALL *)(JNIEnv *, jclass, jint))
    dlsym(RTLD_DEFAULT, "JVM_JamCollections");
  if (!weak_create || !weak_deref || !weak_take || !weak_finalize || !weak_complete || !collections) {
    jclass error = (*env)->FindClass(env, "java/lang/UnsatisfiedLinkError");
    if (error) (*env)->ThrowNew(env, error, "jam-vm requires a JVM exporting the Jam weak hooks");
    return JNI_ERR;
  }
  return JNI_VERSION_1_8;
}

JNIEXPORT void JNICALL Java_jam_vm_Weak_checkAvailable(JNIEnv *env, jclass klass) {
  (void)collections(env, klass, 2);
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
