// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import com.oracle.svm.shared.singletons.traits.BuiltinTraits.AllAccess;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.NoLayeredCallbacks;
import com.oracle.svm.shared.singletons.traits.SingletonTraits;

import org.graalvm.nativeimage.PinnedObject;
import org.graalvm.word.impl.Word;
import com.oracle.svm.core.heap.AbstractPinnedObjectSupport;
import com.oracle.svm.guest.staging.core.jdk.UninterruptibleUtils.AtomicInteger;
import com.oracle.svm.shared.Uninterruptible;
import com.oracle.svm.shared.util.VMError;

/** Pins expose old storage; collection never rotates that storage until every pin closes. */
@SingletonTraits(access = AllAccess.class, layeredCallbacks = NoLayeredCallbacks.class)
final class JamPinnedObjectSupport extends AbstractPinnedObjectSupport {
    private final AtomicInteger count = new AtomicInteger(0);

    @Override
    public PinnedObject create(Object object) {
        if (needsPromotion(object)) {
            JamGC.get().promoteForPin();
        }
        // The tracked local remains valid across promotion and allocation of the pin wrapper.
        return super.create(object);
    }

    @Uninterruptible(reason = "Classify the tracked referent before a possible promotion safepoint.")
    private boolean needsPromotion(Object object) {
        return needsPinning(object) && JamHeap.get().isYoung(Word.objectToUntrackedPointer(object));
    }

    @Override
    @Uninterruptible(reason = "Publish the pin and its old-generation count atomically with respect to GC.", callerMustBe = true)
    protected void pinObject(Object object) {
        VMError.guarantee(!JamHeap.get().isYoung(Word.objectToUntrackedPointer(object)), "A pin must be promoted before its address is exposed");
        VMError.guarantee(count.incrementAndGet() > 0, "Jam pin count overflow");
    }

    @Override
    @Uninterruptible(reason = "Release pin ownership without an intervening GC.", callerMustBe = true)
    protected void unpinObject(Object object) {
        VMError.guarantee(count.decrementAndGet() >= 0, "Jam pin count underflow");
    }

    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    boolean hasPins() { return count.get() != 0; }
}
