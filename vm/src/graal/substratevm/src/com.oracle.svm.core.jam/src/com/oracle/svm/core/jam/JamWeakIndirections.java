// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import java.util.Arrays;
import org.graalvm.word.Pointer;
import org.graalvm.word.impl.Word;
import com.oracle.svm.core.heap.ReferenceAccess;
import com.oracle.svm.shared.Uninterruptible;
import jdk.internal.misc.Unsafe;

/** Isolate-local strong metadata roots; no descriptor retains a carrier instance. */
final class JamWeakIndirections {
    private static final Unsafe UNSAFE = Unsafe.getUnsafe();
    private static volatile Descriptor[] descriptors = new Descriptor[0];

    private static final class Descriptor {
        final Class<?> carrier;
        final long stateOffset;
        final int completedState;
        final long referentOffset;

        Descriptor(Class<?> carrier, long stateOffset, int completedState, long referentOffset) {
            this.carrier = carrier;
            this.stateOffset = stateOffset;
            this.completedState = completedState;
            this.referentOffset = referentOffset;
        }
    }

    /** Runs per isolate; the build-time plugin supplies offsets from the target layout. */
    static synchronized void register(Class<?> carrier, long stateOffset, int completedState, long referentOffset) {
        Descriptor[] current = descriptors;
        for (Descriptor descriptor : current) {
            if (descriptor.carrier == carrier) {
                if (descriptor.stateOffset != stateOffset || descriptor.completedState != completedState || descriptor.referentOffset != referentOffset) {
                    throw new IllegalArgumentException("conflicting weak indirection descriptor");
                }
                return;
            }
        }
        Descriptor descriptor = new Descriptor(carrier, stateOffset, completedState, referentOffset);
        Descriptor[] next = Arrays.copyOf(current, current.length + 1);
        next[current.length] = descriptor;
        descriptors = next;
    }

    @Uninterruptible(reason = "Bind exact carrier identity once while creating the weak entry.")
    static long descriptor(Object key) {
        Descriptor[] current = descriptors;
        Class<?> carrier = key.getClass();
        for (int i = 0; i < current.length; i++) {
            if (current[i].carrier == carrier) return (long) i + 1;
        }
        return 0;
    }

    @Uninterruptible(reason = "The collector has stopped mutators and has not moved or reclaimed any source objects.")
    static int resolve(long id, int key) {
        Descriptor descriptor = descriptors[(int) id - 1];
        JamHeap heap = JamHeap.get();
        Object carrier = heap.decode(key);
        if (UNSAFE.getIntVolatile(carrier, descriptor.stateOffset) != descriptor.completedState) return key;
        Pointer slot = Word.objectToUntrackedPointer(carrier).add(Word.unsigned(descriptor.referentOffset));
        Pointer referent = ReferenceAccess.singleton().readObjectAsUntrackedPointer(slot, true);
        return referent.isNull() ? key : (int) referent.subtract(heap.base()).unsignedShiftRight(3).rawValue();
    }
}
