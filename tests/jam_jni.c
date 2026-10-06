// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

#include <jni.h>

extern void JNICALL JVM_JamCollect(JNIEnv *, jclass, jboolean, jboolean);
extern jlong JNICALL JVM_JamCollections(JNIEnv *, jclass, jint);

JNIEXPORT void JNICALL Java_JamWeak_minor(JNIEnv *env, jclass klass, jboolean promote) {
  JVM_JamCollect(env, klass, JNI_TRUE, promote);
}

JNIEXPORT jlong JNICALL Java_JamWeak_collections(JNIEnv *env, jclass klass, jint kind) {
  return JVM_JamCollections(env, klass, kind);
}
