// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import com.oracle.svm.core.genscavenge.FillerObjectUtil;
import java.lang.ref.Reference;
import java.util.ArrayList;
import java.util.List;
import org.graalvm.nativeimage.ImageSingletons;
import org.graalvm.nativeimage.IsolateThread;
import org.graalvm.word.Pointer;
import org.graalvm.word.UnsignedWord;
import org.graalvm.word.impl.Word;
import com.oracle.svm.core.heap.GC;
import com.oracle.svm.core.heap.Heap;
import com.oracle.svm.core.heap.ObjectHeader;
import com.oracle.svm.core.heap.ObjectReferenceVisitor;
import com.oracle.svm.core.heap.ObjectVisitor;
import com.oracle.svm.core.heap.ReferenceAccess;
import com.oracle.svm.core.heap.ReferenceInternals;
import com.oracle.svm.core.heap.RuntimeCodeInfoGCSupport;
import com.oracle.svm.core.hub.InteriorObjRefWalker;
import com.oracle.svm.core.hub.LayoutEncoding;
import com.oracle.svm.core.locks.VMMutex;
import com.oracle.svm.core.memory.NullableNativeMemory;
import com.oracle.svm.core.nmt.NmtCategory;
import com.oracle.svm.core.os.ImageHeapProvider;
import com.oracle.svm.core.thread.VMOperation;
import com.oracle.svm.core.thread.VMThreads;
import com.oracle.svm.core.thread.VMThreads.SafepointBehavior;
import org.graalvm.nativeimage.CurrentIsolate;
import com.oracle.svm.guest.staging.core.UnmanagedMemoryUtil;
import com.oracle.svm.guest.staging.core.graal.KnownIntrinsics;
import com.oracle.svm.guest.staging.core.threadlocal.FastThreadLocalFactory;
import com.oracle.svm.guest.staging.core.threadlocal.FastThreadLocalInt;
import com.oracle.svm.guest.staging.core.threadlocal.FastThreadLocalLong;
import com.oracle.svm.guest.staging.core.threadlocal.FastThreadLocalWord;
import com.oracle.svm.guest.staging.log.Log;
import com.oracle.svm.guest.staging.option.NotifyGCRuntimeOptionKey;
import com.oracle.svm.shared.Uninterruptible;
import com.oracle.svm.guest.staging.core.jdk.UninterruptibleUtils.Math;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.AllAccess;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.DisallowLayered;
import com.oracle.svm.shared.singletons.traits.BuiltinTraits.NoLayeredCallbacks;
import com.oracle.svm.shared.singletons.traits.SingletonTraits;
import com.oracle.svm.shared.util.VMError;
import jdk.graal.compiler.api.replacements.Fold;

/** Java heap ownership and remembered metadata around Jam's native hosted heap. */
@SingletonTraits(access = AllAccess.class, layeredCallbacks = NoLayeredCallbacks.class, other = DisallowLayered.class)
public final class JamHeap extends Heap {
    static final int IMAGE_OFFSET = 1 << 30;
    static final long YOUNG_OFFSET = 1L << 34;
    static final long ADDRESS_SPACE = 1L << 35;
    static final int CARD_SHIFT = 9;
    private static final FastThreadLocalInt allocationSuspended = FastThreadLocalFactory.createInt("Jam.allocationSuspended");
    private static final FastThreadLocalLong allocatedBytes = FastThreadLocalFactory.createLong("Jam.allocatedBytes");
    private static final FastThreadLocalWord<Pointer> threadScope = FastThreadLocalFactory.createWord("Jam.heapScope");
    private final VMMutex allocationLock = new VMMutex("Jam allocation");
    private final JamObjectHeader header = new JamObjectHeader();
    final JamImageHeapInfo imageInfo = new JamImageHeapInfo();
    private final JamGC gc = new JamGC(this);
    private final CardRebuilder cardRebuilder = new CardRebuilder();
    private Pointer handle = Word.nullPointer();
    private Pointer cards = Word.nullPointer();
    private Pointer starts = Word.nullPointer();
    private UnsignedWord prefix = Word.zero();
    private UnsignedWord oldBytes = Word.zero();
    private UnsignedWord youngBytes = Word.zero();
    private UnsignedWord cardCount = Word.zero();
    private Pointer oldTop = Word.nullPointer();
    private Pointer youngTop = Word.nullPointer();
    private long oldPeak;
    private long youngPeak;
    private long oldAfterCollection;
    private long youngAfterCollection;

    @Fold
    public static JamHeap get() { return ImageSingletons.lookup(JamHeap.class); }

    @Uninterruptible(reason = "Initialize the mapped isolate before allocation begins.")
    void initialize(Pointer nativeHeap, UnsignedWord guard, UnsignedWord oldCapacity, UnsignedWord youngCapacity) {
        handle = nativeHeap;
        prefix = guard;
        oldBytes = oldCapacity;
        youngBytes = youngCapacity;
        cardCount = guard.add(oldCapacity).add((1 << CARD_SHIFT) - 1).unsignedShiftRight(CARD_SHIFT);
        cards = NullableNativeMemory.calloc(cardCount, NmtCategory.GC);
        starts = NullableNativeMemory.calloc(cardCount.multiply(Integer.BYTES), NmtCategory.GC);
        VMError.guarantee(cards.isNonNull() && starts.isNonNull(), "Cannot allocate Jam remembered metadata");
        oldTop = oldBegin();
        youngTop = youngBegin();
    }

    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer nativeHeap() { return handle; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer base() { return KnownIntrinsics.heapBase(); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    UnsignedWord prefixBytes() { return prefix; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    UnsignedWord oldCapacity() { return oldBytes; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    UnsignedWord youngCapacity() { return youngBytes; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer oldBegin() { return base().add(prefix); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer oldEnd() { return oldBegin().add(oldBytes); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer youngBegin() { return base().add(Word.unsigned(YOUNG_OFFSET)).add(prefix); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer youngEnd() { return youngBegin().add(youngBytes); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer oldUsedEnd() { return oldTop; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer youngUsedEnd() { return youngTop; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    VMMutex lock() { return allocationLock; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    Pointer cardTable() { return cards; }

    @Uninterruptible(reason = "Keep encoded and materialized references in one safepoint-free scope.", callerMustBe = true)
    int encode(Object object) {
        return object == null ? 0 : (int) Word.objectToUntrackedPointer(object).subtract(base()).unsignedShiftRight(3).rawValue();
    }
    @Uninterruptible(reason = "Keep encoded and materialized references in one safepoint-free scope.", callerMustBe = true)
    Object decode(int offset) {
        return offset == 0 ? null : base().add(Word.unsigned((offset & 0xffffffffL)).shiftLeft(3)).toObjectNonNull();
    }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    boolean isYoung(Pointer object) { return object.aboveOrEqual(youngBegin()) && object.belowThan(youngTop); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    boolean isOld(Pointer object) { return object.aboveOrEqual(oldBegin()) && object.belowThan(oldTop); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    boolean isManaged(Pointer object) { return isOld(object) || isYoung(object); }

    @Uninterruptible(reason = "Allocate raw object storage under the allocation lock.")
    Pointer allocateRaw(UnsignedWord bytes, boolean young) {
        VMError.guarantee(bytes.and(7).equal(0), "Jam allocation must be word aligned");
        int offset = JamNative.allocate(handle, bytes.unsignedShiftRight(3), young ? 1 : 0);
        if (offset == 0) { return Word.nullPointer(); }
        Pointer result = base().add(Word.unsigned((offset & 0xffffffffL)).shiftLeft(3));
        if (young) {
            youngTop = result.add(bytes);
            youngPeak = Math.max(youngPeak, usedBytes(true));
        } else {
            oldTop = result.add(bytes);
            oldPeak = Math.max(oldPeak, usedBytes(false));
            recordObjectStart(result, bytes);
        }
        return result;
    }

    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long usedBytes(boolean young) { return (young ? youngTop.subtract(youngBegin()) : oldTop.subtract(oldBegin())).rawValue(); }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long peakBytes(boolean young) { return young ? youngPeak : oldPeak; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long collectionBytes(boolean young) { return young ? youngAfterCollection : oldAfterCollection; }
    @Uninterruptible(reason = "Reset peak accounting under the allocator lock.")
    void resetPeak(boolean young) {
        lock().lockNoTransition();
        try {
            if (young) { youngPeak = usedBytes(true); }
            else { oldPeak = usedBytes(false); }
        } finally { lock().unlock(); }
    }

    @Uninterruptible(reason = "Record the first owner overlapping each old card.")
    private void recordObjectStart(Pointer object, UnsignedWord bytes) {
        long first = object.subtract(base()).unsignedShiftRight(CARD_SHIFT).rawValue();
        long last = object.add(bytes).subtract(1).subtract(base()).unsignedShiftRight(CARD_SHIFT).rawValue();
        int encoded = (int) object.subtract(base()).unsignedShiftRight(3).rawValue();
        for (long card = first; card <= last; card++) {
            UnsignedWord index = Word.unsigned(card).multiply(Integer.BYTES);
            if (starts.readInt(index) == 0) { starts.writeInt(index, encoded); }
        }
    }

    @Uninterruptible(reason = "Consume dirty owners at a GC safepoint.", calleeMustBe = false)
    void walkDirtyOldObjects(ObjectVisitor visitor) {
        Pointer visited = Word.nullPointer();
        for (long card = 0; card < cardCount.rawValue(); card++) {
            if (cards.readByte(Word.unsigned(card)) == 0) { continue; }
            int offset = starts.readInt(Word.unsigned(card).multiply(Integer.BYTES));
            if (offset == 0) { continue; }
            Pointer current = base().add(Word.unsigned((offset & 0xffffffffL)).shiftLeft(3));
            Pointer limit = base().add(Word.unsigned(card + 1).shiftLeft(CARD_SHIFT));
            while (current.belowThan(limit) && current.belowThan(oldTop)) {
                Object object = current.toObjectNonNull();
                if (current.aboveThan(visited)) { visitor.visitObject(object); visited = current; }
                current = current.add(LayoutEncoding.getSizeFromObjectInGC(object));
            }
        }
    }

    @Uninterruptible(reason = "Reset remembered metadata at a GC safepoint.")
    void clearCards() { UnmanagedMemoryUtil.fill(cards, cardCount, (byte) 0); }

    @Uninterruptible(reason = "Update metadata after publishing the collected arenas.")
    void finishCollection(boolean minor, boolean promoted) {
        for (UnsignedWord i = Word.zero(); i.belowThan(JamNative.gapCount(handle)); i = i.add(1)) {
            int at = JamNative.gapAt(handle, i);
            Pointer address = base().add(Word.unsigned(at & 0xffffffffL).shiftLeft(3));
            FillerObjectUtil.writeFillerObjectAt(address, JamNative.gapWords(handle, i).shiftLeft(3), false);
        }
        Pointer previousOldTop = oldTop;
        oldTop = base().add(JamNative.used(handle, 0).shiftLeft(3));
        youngTop = base().add(Word.unsigned(YOUNG_OFFSET)).add(JamNative.used(handle, 1).shiftLeft(3));
        oldAfterCollection = usedBytes(false);
        youngAfterCollection = usedBytes(true);
        oldPeak = Math.max(oldPeak, oldAfterCollection);
        youngPeak = Math.max(youngPeak, youngAfterCollection);
        if (minor && !promoted) {
            VMError.guarantee(oldTop.equal(previousOldTop), "A retaining minor cannot move the old arena");
            return;
        }
        if (!minor) { UnmanagedMemoryUtil.fill(starts, cardCount.multiply(Integer.BYTES), (byte) 0); }
        Pointer current = minor ? previousOldTop : oldBegin();
        while (current.belowThan(oldTop)) {
            UnsignedWord bytes = LayoutEncoding.getSizeFromObjectInGC(current.toObjectNonNull());
            recordObjectStart(current, bytes);
            current = current.add(bytes);
        }
        if (minor) {
            VMError.guarantee(youngTop.equal(youngBegin()), "Whole-nursery promotion must leave no young survivors");
            clearCards();
        } else {
            rebuildCards();
        }
    }

    @Uninterruptible(reason = "Reconstruct precise old-to-young cards after movement.")
    void rebuildCards() {
        clearCards();
        Pointer current = oldBegin();
        while (current.belowThan(oldTop)) {
            Object object = current.toObjectNonNull();
            InteriorObjRefWalker.walkObject(object, cardRebuilder);
            if (object instanceof Reference<?> reference) {
                Object referent = ReferenceInternals.getReferent(reference);
                if (referent != null && isYoung(Word.objectToUntrackedPointer(referent))) { dirtyAllReferencesOf(object); }
            }
            current = current.add(LayoutEncoding.getSizeFromObjectInGC(object));
        }
    }

    private static final class CardRebuilder implements ObjectReferenceVisitor {
        @Override
        @Uninterruptible(reason = "Rebuild remembered references.")
        public void visitObjectReferences(Pointer first, boolean compressed, int size, Object holder, int count) {
            for (int i = 0; i < count; i++) {
                Object value = ReferenceAccess.singleton().readObjectAt(first.add(i * size), compressed);
                if (value != null && get().isYoung(Word.objectToUntrackedPointer(value))) {
                    get().dirtyAllReferencesOf(holder);
                    return;
                }
            }
        }
        @Override
        @Uninterruptible(reason = "The corresponding base reference was already visited.")
        public void visitDerivedReference(Pointer baseRef, Pointer derivedRef, boolean compressed, Object holder) { }
    }

    @Override
    @Uninterruptible(reason = "Dirty the old owner before a safepoint.", callerMustBe = true)
    public void dirtyAllReferencesOf(Object object) {
        Pointer address = Word.objectToUntrackedPointer(object);
        if (isOld(address)) {
            cards.writeByte(address.subtract(base()).unsignedShiftRight(CARD_SHIFT), (byte) 1);
        }
    }

    @Override public GC getGC() { return gc; }
    JamGC collector() { return gc; }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public RuntimeCodeInfoGCSupport getRuntimeCodeInfoGCSupport() { return gc.getRuntimeCodeInfoGCSupport(); }
    @Override public void doReferenceHandling() { gc.doReferenceHandling(); }
    @Override public boolean hasReferencePendingList() { return gc.hasReferencePendingList(); }
    @Override public void waitForReferencePendingList() throws InterruptedException { gc.waitForReferencePendingList(); }
    @Override public void wakeUpReferencePendingListWaiters() { gc.wakeUpReferencePendingListWaiters(); }
    @Override public Reference<?> getAndClearReferencePendingList() { return gc.getAndClearReferencePendingList(); }
    @Override public long getMillisSinceLastWholeHeapExamined() { return Math.max(0, System.currentTimeMillis() - gc.lastMajorTime()); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public UnsignedWord getUsedMemoryAfterLastGC() { return gc.collectedMemory(); }
    @Override @Uninterruptible(reason = "The identity hash resides in a permanent field.", callerMustBe = true)
    public long getIdentityHashSalt(Object object) { return 0; }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public ObjectHeader getObjectHeader() { return header; }
    @Override @Fold public int getHeapBaseAlignment() { return 16 * 1024; }
    @Override @Fold public int getImageHeapAlignment() { return 16 * 1024; }
    @Override @Fold public int getImageHeapOffsetInAddressSpace() { return IMAGE_OFFSET; }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public boolean isInImageHeap(Object object) { return object != null && isInImageHeap(Word.objectToUntrackedPointer(object)); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public boolean isInImageHeap(Pointer object) { return object.aboveOrEqual(base().add(IMAGE_OFFSET)) && object.belowThan(base().add(ImageHeapProvider.get().getImageHeapEndOffsetInAddressSpace())); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public boolean isInPrimaryImageHeap(Object object) { return isInImageHeap(object); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public boolean isInPrimaryImageHeap(Pointer object) { return isInImageHeap(object); }
    @Override @Uninterruptible(reason = "Validate startup mapping.")
    public boolean verifyImageHeapMapping() { return handle.isNonNull() && isInImageHeap(JamHeap.class) && oldEnd().belowOrEqual(base().add(IMAGE_OFFSET)); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public int getClassCount() { return imageInfo.classCount; }
    @Override protected List<Class<?>> getClassesInImageHeap() {
        List<Class<?>> result = new ArrayList<>();
        for (int i = 0; i < JamImageHeapInfo.PARTITIONS; i++) {
            if (imageInfo.first(i) == null) { continue; }
            Pointer current = Word.objectToUntrackedPointer(imageInfo.first(i));
            Pointer last = Word.objectToUntrackedPointer(imageInfo.last(i));
            while (current.belowOrEqual(last)) {
                Object object = current.toObjectNonNull();
                if (object instanceof Class<?> clazz) { result.add(clazz); }
                current = current.add(LayoutEncoding.getSizeFromObjectInGC(object));
            }
        }
        return result;
    }
    @Override public void walkObjects(ObjectVisitor visitor) { walkImageHeapObjects(visitor); walkCollectedHeapObjects(visitor); }
    @Override public void walkImageHeapObjects(ObjectVisitor visitor) { imageInfo.walk(visitor, false); }
    void walkImageHeapRoots(ObjectVisitor visitor) { imageInfo.walk(visitor, true); }
    @Override public void walkCollectedHeapObjects(ObjectVisitor visitor) {
        VMOperation.guaranteeInProgressAtSafepoint("Jam heap walking requires a safepoint");
        retireAllTlabs();
        walkRegion(oldBegin(), oldTop, visitor);
        walkRegion(youngBegin(), youngTop, visitor);
    }
    private static void walkRegion(Pointer begin, Pointer end, ObjectVisitor visitor) {
        Pointer current = begin;
        while (current.belowThan(end)) {
            Object object = current.toObjectNonNull();
            visitor.visitObject(object);
            current = current.add(LayoutEncoding.getSizeFromObjectInGC(object));
        }
    }
    @Override @Uninterruptible(reason = "Retire the fast path before allocation is suspended.")
    public void suspendAllocation() {
        JamThreadLocalAllocation.retire(CurrentIsolate.getCurrentThread());
        allocationSuspended.set(allocationSuspended.get() + 1);
    }
    @Override public void resumeAllocation() { allocationSuspended.set(allocationSuspended.get() - 1); }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public boolean isAllocationDisallowed() { return allocationSuspended.get() != 0 || SafepointBehavior.ignoresSafepoints(); }
    @Uninterruptible(reason = "Account for actual objects, excluding unused buffer tails.")
    void recordAllocatedBytes(IsolateThread thread, UnsignedWord bytes) {
        allocatedBytes.set(thread, allocatedBytes.get(thread) + bytes.rawValue());
    }
    @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public long getThreadAllocatedMemory(IsolateThread thread) { return allocatedBytes.get(thread) + JamThreadLocalAllocation.usedBytes(thread).rawValue(); }
    @Override @Uninterruptible(reason = "Initialize per-thread allocation state.")
    public void attachThread(IsolateThread thread) {
        allocatedBytes.set(thread, 0);
        JamThreadLocalAllocation.initialize(thread);
        Pointer scope = JamNative.threadCreate(handle);
        threadScope.set(thread, scope);
        JamNative.threadEnter(scope);
    }
    @Override @Uninterruptible(reason = "Release per-thread allocation state.")
    public void detachThread(IsolateThread thread) {
        JamThreadLocalAllocation.retire(thread);
        releaseThreadScope(thread);
    }
    @Uninterruptible(reason = "Bind Jam before executing Java or VM work on this thread.")
    @Override public void enterThreadContext() {
        Pointer scope = threadScope.get();
        if (scope.isNonNull()) { JamThreadContext.enter(scope); }
    }
    @Uninterruptible(reason = "Restore the enclosing native context before a thread may switch isolates.")
    @Override public void leaveThreadContext() {
        Pointer scope = threadScope.get();
        if (scope.isNonNull()) { JamThreadContext.leave(scope); }
    }
    @Uninterruptible(reason = "Only inactive scopes may be reclaimed from another OS thread.")
    private void releaseThreadScope(IsolateThread thread) {
        Pointer scope = threadScope.get(thread);
        if (scope.isNull()) { return; }
        if (thread.equal(CurrentIsolate.getCurrentThread())) { JamNative.threadLeave(scope); }
        JamNative.threadDestroy(scope);
        threadScope.set(thread, Word.nullPointer());
    }
    @Override public void prepareForSafepoint() { }
    @Override public void endSafepoint() { }
    @Uninterruptible(reason = "Mutators are stopped before their unused buffers become filler objects.")
    void retireAllTlabs() {
        VMOperation.guaranteeInProgressAtSafepoint("Jam TLAB retirement requires a safepoint");
        for (IsolateThread thread = VMThreads.firstThread(); thread.isNonNull(); thread = VMThreads.nextThread(thread)) {
            JamThreadLocalAllocation.retire(thread);
        }
    }
    @Override public void optionValueChanged(NotifyGCRuntimeOptionKey<?> key) { }
    @Override public boolean printLocationInfo(Log log, UnsignedWord value, boolean access, boolean unsafe) {
        Pointer pointer = (Pointer) value;
        if (isManaged(pointer) || isInImageHeap(pointer)) { log.string("Jam heap ").zhex(value); return true; }
        return false;
    }
    @Override @Uninterruptible(reason = "Tear down native heap before releasing canonical reservation.")
    public boolean tearDown() {
        releaseThreadScope(CurrentIsolate.getCurrentThread());
        releaseNativeResources();
        return true;
    }
    @Uninterruptible(reason = "Also called when bootstrap failed before any VM thread existed.")
    void releaseNativeResources() {
        gc.tearDown();
        if (handle.isNonNull()) { JamNative.destroy(handle); handle = Word.nullPointer(); }
        NullableNativeMemory.free(cards);
        NullableNativeMemory.free(starts);
        cards = starts = Word.nullPointer();
    }
}
