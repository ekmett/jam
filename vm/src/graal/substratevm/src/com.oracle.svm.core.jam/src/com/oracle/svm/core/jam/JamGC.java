// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import java.lang.ref.PhantomReference;
import java.lang.ref.Reference;
import java.lang.ref.SoftReference;
import java.util.function.BooleanSupplier;
import org.graalvm.nativeimage.CurrentIsolate;
import org.graalvm.nativeimage.IsolateThread;
import org.graalvm.nativeimage.Platform;
import org.graalvm.nativeimage.Platforms;
import org.graalvm.nativeimage.StackValue;
import org.graalvm.nativeimage.c.function.CEntryPoint;
import org.graalvm.nativeimage.c.function.CEntryPointLiteral;
import org.graalvm.nativeimage.c.function.CFunctionPointer;
import org.graalvm.nativeimage.c.struct.RawField;
import org.graalvm.nativeimage.c.struct.RawStructure;
import org.graalvm.nativeimage.c.struct.SizeOf;
import org.graalvm.nativeimage.c.type.CIntPointer;
import org.graalvm.nativeimage.c.type.CLongPointer;
import org.graalvm.word.Pointer;
import org.graalvm.word.UnsignedWord;
import org.graalvm.word.impl.Word;
import com.oracle.svm.core.SubstrateOptions;
import com.oracle.svm.core.code.RuntimeCodeInfoMemory;
import com.oracle.svm.core.graal.nodes.WriteCurrentVMThreadNode;
import com.oracle.svm.core.graal.snippets.CEntryPointSnippets;
import com.oracle.svm.core.handles.ObjectHandlesImpl;
import com.oracle.svm.core.heap.AbstractPinnedObjectSupport;
import com.oracle.svm.core.heap.DerivedReferenceSupport;
import com.oracle.svm.core.heap.GC;
import com.oracle.svm.core.heap.GCCause;
import com.oracle.svm.core.heap.NoAllocationVerifier;
import com.oracle.svm.core.heap.ObjectHeader;
import com.oracle.svm.core.heap.OutOfMemoryUtil;
import com.oracle.svm.core.heap.ReferenceAccess;
import com.oracle.svm.core.heap.ReferenceHandler;
import com.oracle.svm.core.heap.ReferenceInternals;
import com.oracle.svm.core.heap.RuntimeCodeCacheCleaner;
import com.oracle.svm.core.heap.RuntimeCodeInfoGCSupport;
import com.oracle.svm.core.heap.UninterruptibleObjectReferenceVisitor;
import com.oracle.svm.core.heap.UninterruptibleObjectVisitor;
import com.oracle.svm.core.heap.VMOperationInfos;
import com.oracle.svm.core.hub.InteriorObjRefWalker;
import com.oracle.svm.core.hub.LayoutEncoding;
import com.oracle.svm.core.thread.NativeVMOperation;
import com.oracle.svm.core.thread.NativeVMOperationData;
import com.oracle.svm.core.thread.VMOperation;
import com.oracle.svm.core.thread.VMThreads;
import com.oracle.svm.core.threadlocal.VMThreadLocalSupport;
import com.oracle.svm.core.snippets.ImplicitExceptions;
import com.oracle.svm.guest.staging.c.function.CEntryPointOptions;
import com.oracle.svm.guest.staging.core.UnmanagedMemoryUtil;
import com.oracle.svm.guest.staging.core.graal.KnownIntrinsics;
import com.oracle.svm.shared.NeverInline;
import com.oracle.svm.shared.Uninterruptible;
import com.oracle.svm.guest.staging.core.jdk.UninterruptibleUtils.Math;
import com.oracle.svm.shared.util.VMError;
import jdk.graal.compiler.api.replacements.Fold;

/** SubstrateVM owns roots and policy; Jam owns both generation arenas and their movement. */
final class JamGC implements GC {
    private static final CEntryPointLiteral<CFunctionPointer> SCAN = CEntryPointLiteral.create(
                    JamGC.class, "scan", JamScanContext.class, Pointer.class, int.class);
    private static final CEntryPointLiteral<CFunctionPointer> RESOLVE_WEAK = CEntryPointLiteral.create(
                    JamGC.class, "resolveWeak", JamScanContext.class, long.class, int.class);
    private final JamHeap heap;
    private final CollectionOperation operation = new CollectionOperation();
    private final JamNativeList objects = new JamNativeList();
    private final JamNativeList references = new JamNativeList();
    private final JamNativeList roots = new JamNativeList();
    private final JamNativeList derived = new JamNativeList();
    private final RootVisitor rootVisitor = new RootVisitor();
    private final FieldVisitor fieldVisitor = new FieldVisitor();
    private final RepairVisitor repairVisitor = new RepairVisitor();
    private final ImageVisitor imageVisitor = new ImageVisitor();
    private final OldVisitor oldVisitor = new OldVisitor();
    private final JamCodeRoots codeRoots = new JamCodeRoots(this);
    private final RuntimeCodeCacheCleaner codeCleaner = new RuntimeCodeCacheCleaner();
    final JamReferenceQueue referenceQueue = new JamReferenceQueue();
    private JamScanContext context;
    private Pointer scanner;
    private boolean minor;
    private boolean clearSoft;
    private long usedAfterGC;
    private long lastMajorMillis;
    private long collections;
    private long minorCount;
    private long majorCount;
    private long minorNanos;
    private long majorNanos;

    @Platforms(Platform.HOSTED_ONLY.class)
    JamGC(JamHeap heap) { this.heap = heap; }

    @Fold
    static JamGC get() { return (JamGC) JamHeap.get().getGC(); }

    @Fold
    static JamPinnedObjectSupport pins() { return (JamPinnedObjectSupport) AbstractPinnedObjectSupport.singleton(); }

    @Override public String getName() { return "Jam"; }
    @Override @Platforms(Platform.HOSTED_ONLY.class)
    public String getDefaultMaxHeapSize() { return "128 MiB"; }
    @Override public void collect(GCCause cause) { request(false, false); }
    @Override public void collectCompletely(GCCause cause) { request(true, false); }
    @Override public void collectionHint(boolean fullGC) { request(fullGC, false); }

    void collectForAllocation(UnsignedWord bytes, boolean old) {
        request(false, false);
        if (available(old).belowThan(bytes)) request(true, true);
    }

    void promoteForPin() {
        if (request(false, false)) return;
        if (!pins().hasPins()) {
            request(true, true);
            if (request(false, true)) return;
        }
        throw OutOfMemoryUtil.heapSizeExceeded();
    }

    @Uninterruptible(reason = "Read native allocation bounds without an intervening collection.")
    private UnsignedWord available(boolean old) {
        UnsignedWord capacity = old ? heap.oldCapacity() : heap.youngCapacity();
        UnsignedWord used = JamNative.used(heap.nativeHeap(), old ? 0 : 1).multiply(8).subtract(heap.prefixBytes());
        return capacity.subtract(used);
    }

    @Uninterruptible(reason = "The request is held in native stack storage until the operation completes.")
    private boolean request(boolean major, boolean reclaimSoft) {
        int size = SizeOf.get(CollectionData.class);
        CollectionData data = StackValue.get(size);
        UnmanagedMemoryUtil.fill((Pointer) data, Word.unsigned(size), (byte) 0);
        data.setMajor(major);
        data.setClearSoft(reclaimSoft);
        enqueue(data);
        return data.getPromoted();
    }

    @Uninterruptible(reason = "Transition into the safepoint operation.", calleeMustBe = false)
    private void enqueue(CollectionData data) { operation.enqueue(data); }

    private static final class CollectionOperation extends NativeVMOperation {
        private final NoAllocationVerifier verifier = NoAllocationVerifier.factory("Jam collection", false);
        @Platforms(Platform.HOSTED_ONLY.class)
        CollectionOperation() { super(VMOperationInfos.get(CollectionOperation.class, "Jam collection", SystemEffect.SAFEPOINT)); }
        @Override @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
        public boolean isGC() { return true; }
        @Override protected void operate(NativeVMOperationData data) {
            verifier.open();
            ImplicitExceptions.activateImplicitExceptionsAreFatal();
            try { get().collectAtSafepoint((CollectionData) data); }
            catch (Throwable failure) { throw VMError.shouldNotReachHere(failure); }
            finally {
                ImplicitExceptions.deactivateImplicitExceptionsAreFatal();
                verifier.close();
            }
        }
    }

    @RawStructure
    interface CollectionData extends NativeVMOperationData {
        @RawField boolean getMajor();
        @RawField void setMajor(boolean value);
        @RawField boolean getClearSoft();
        @RawField void setClearSoft(boolean value);
        @RawField boolean getPromoted();
        @RawField void setPromoted(boolean value);
    }

    @NeverInline("Start root walking above collector frames containing temporary source objects.")
    @Uninterruptible(reason = "Stop-the-world collection; no Java allocation or safepoints.", calleeMustBe = false)
    private void collectAtSafepoint(CollectionData data) {
        VMOperation.guaranteeGCInProgress("Jam collection requires stopped mutators");
        long started = System.nanoTime();
        heap.retireAllTlabs();
        pins().removeClosedObjectsAndGetFirstOpenObject();
        minor = !data.getMajor() || pins().hasPins();
        clearSoft = data.getClearSoft();
        objects.clear(); references.clear(); roots.clear(); derived.clear();
        context = StackValue.get(JamScanContext.class);
        context.setIsolate(CurrentIsolate.getIsolate());
        context.setThread(CurrentIsolate.getCurrentThread());
        JamNative.begin(heap.nativeHeap(), minor ? 1 : 0);
        JamStackRoots.walk(rootVisitor, KnownIntrinsics.readCallerStackPointer(), true);
        for (IsolateThread thread = VMThreads.firstThread(); thread.isNonNull(); thread = VMThreads.nextThread(thread)) {
            VMThreadLocalSupport.singleton().walk(thread, rootVisitor);
        }
        heap.walkImageHeapRoots(imageVisitor);
        if (minor) heap.walkDirtyOldObjects(oldVisitor);
        JamNative.weakRoots(heap.nativeHeap(), SCAN.getFunctionPointer(), context);
        RuntimeCodeInfoMemory.singleton().walkRuntimeMethodsDuringGC(codeRoots);
        JamNative.weakRetarget(heap.nativeHeap(), RESOLVE_WEAK.getFunctionPointer(), context);
        JamNative.weakClose(heap.nativeHeap(), SCAN.getFunctionPointer(), context);
        long weakLimit = references.size();
        processReferences(0, weakLimit, false);
        JamNative.weakFinalizers(heap.nativeHeap(), SCAN.getFunctionPointer(), context);
        processReferences(weakLimit, references.size(), false);
        processReferences(0, references.size(), true);
        snapshotRoots();
        boolean promoted = minor && JamNative.prepare(heap.nativeHeap(), 1) != 0;
        if (!promoted) VMError.guarantee(JamNative.prepare(heap.nativeHeap(), 0) != 0, "Retaining collection must fit");
        repairRoots();
        for (long i = 0; i < objects.size(); i++) {
            int at = (int) objects.get(i);
            Object object = heap.decode(at);
            InteriorObjRefWalker.walkObjectInline(object, repairVisitor);
            if (object instanceof Reference<?>) repairReference((Reference<?>) object, at);
        }
        repairDerived();
        JamNative.finish(heap.nativeHeap());
        heap.finishCollection(minor, promoted);
        RuntimeCodeInfoMemory.singleton().walkRuntimeMethodsDuringGC(codeCleaner);
        usedAfterGC = JamNative.used(heap.nativeHeap(), 0).add(JamNative.used(heap.nativeHeap(), 1)).multiply(8).subtract(heap.prefixBytes().multiply(2)).rawValue();
        if (!minor) lastMajorMillis = System.currentTimeMillis();
        collections++;
        long elapsed = System.nanoTime() - started;
        if (minor) { minorCount++; minorNanos += elapsed; }
        else { majorCount++; majorNanos += elapsed; }
        context = Word.nullPointer();
        data.setPromoted(promoted);
    }

    static final class Enabled implements BooleanSupplier {
        @Override public boolean getAsBoolean() { return SubstrateOptions.useJamGC(); }
    }

    /** C++ may use reserved registers internally, even on the already attached GC thread. */
    static final class ScannerPrologue implements CEntryPointOptions.Prologue {
        @Uninterruptible(reason = "Establish callback registers from native stack storage.")
        static void enter(JamScanContext context) {
            CEntryPointSnippets.initBaseRegisters(context.getIsolate());
            WriteCurrentVMThreadNode.writeCurrentVMThread(context.getThread());
        }
    }

    @CEntryPoint(include = Enabled.class, publishAs = CEntryPoint.Publish.NotPublished)
    @CEntryPointOptions(prologue = ScannerPrologue.class, epilogue = CEntryPointOptions.NoEpilogue.class)
    @Uninterruptible(reason = "Synchronous single-threaded Jam scanner callback.")
    static void scan(JamScanContext context, Pointer visitor, int at) { get().scanObject(visitor, at); }

    @CEntryPoint(include = Enabled.class, publishAs = CEntryPoint.Publish.NotPublished)
    @CEntryPointOptions(prologue = ScannerPrologue.class, epilogue = CEntryPointOptions.NoEpilogue.class)
    @Uninterruptible(reason = "Read a registered indirection before any source object is reclaimed; never mark it.")
    static int resolveWeak(JamScanContext context, long descriptor, int key) { return JamWeakIndirections.resolve(descriptor, key); }

    @Uninterruptible(reason = "Claim the entire object before enumerating fields.")
    private void scanObject(Pointer visitor, int at) {
        Object object = heap.decode(at);
        UnsignedWord bytes = LayoutEncoding.getSizeFromObjectInGC(object);
        if (JamNative.claim(visitor, at, bytes.unsignedShiftRight(3)) == 0) return;
        objects.add(at & 0xffffffffL);
        Pointer previous = scanner;
        scanner = visitor;
        InteriorObjRefWalker.walkObjectInline(object, fieldVisitor);
        if (object instanceof Reference<?>) {
            Reference<?> reference = (Reference<?>) object;
            references.add(at & 0xffffffffL);
            Pointer slot = ReferenceInternals.getReferentFieldAddress(reference);
            CLongPointer slots = StackValue.get(Long.BYTES);
            slots.write(slot.subtract(heap.base()).unsignedShiftRight(2).rawValue());
            JamNative.fields(visitor, slots, Word.unsigned(1), 0);
            if (!clearSoft && reference instanceof SoftReference<?>) traceTarget(ReferenceInternals.getReferentPointer(reference), visitor);
        }
        scanner = previous;
    }

    @Uninterruptible(reason = "Drain external roots on the attached collector thread.")
    void traceTarget(Pointer target, Pointer visitor) {
        if (target.isNull() || heap.isInImageHeap(target)) return;
        VMError.guarantee(heap.isManaged(target), "Root outside Jam and the permanent image heap");
        CIntPointer root = StackValue.get(Integer.BYTES);
        root.write(encode(target));
        if (visitor.isNonNull()) JamNative.targets(visitor, root, Word.unsigned(1));
        else JamNative.trace(heap.nativeHeap(), root, Word.unsigned(1), SCAN.getFunctionPointer(), context, Word.unsigned(1));
    }

    @Uninterruptible(reason = Uninterruptible.CORE_GC_CODE, callerMustBe = true)
    private int encode(Pointer pointer) {
        return pointer.isNull() ? 0 : (int) pointer.subtract(heap.base()).unsignedShiftRight(3).rawValue();
    }

    @Uninterruptible(reason = Uninterruptible.CORE_GC_CODE, callerMustBe = true)
    boolean marked(Pointer pointer) {
        if (pointer.isNull()) return false;
        if (heap.isInImageHeap(pointer)) return true;
        VMError.guarantee(heap.isManaged(pointer), "Liveness query outside Jam and the permanent image heap");
        return JamNative.marked(heap.nativeHeap(), encode(pointer)) != 0;
    }

    @Uninterruptible(reason = Uninterruptible.CORE_GC_CODE, callerMustBe = true)
    private Pointer forward(Pointer pointer) {
        if (pointer.isNull() || heap.isInImageHeap(pointer)) return pointer;
        VMError.guarantee(heap.isManaged(pointer), "Forwarding query outside Jam and the permanent image heap");
        int at = JamNative.forward(heap.nativeHeap(), encode(pointer));
        return at == 0 ? Word.nullPointer() : heap.base().add(Word.unsigned(at & 0xffffffffL).shiftLeft(3));
    }

    @Uninterruptible(reason = "Capture locations, not strong Java handles.")
    private void addRoot(Pointer slot, boolean compressed) {
        recordRoot(slot, compressed);
        traceTarget(ReferenceAccess.singleton().readObjectAsUntrackedPointer(slot, compressed), Word.nullPointer());
    }

    @Uninterruptible(reason = "Register a source slot without strengthening a weak edge.")
    private void recordRoot(Pointer slot, boolean compressed) {
        roots.add(slot.rawValue()); roots.add(compressed ? 1 : 0); roots.add(0);
    }

    @Uninterruptible(reason = "Reference policy may change slots after marking; snapshot before any repair.")
    private void snapshotRoots() {
        for (long i = 0; i < roots.size(); i += 3) {
            Pointer slot = Word.pointer(roots.get(i));
            roots.set(i + 2, ReferenceAccess.singleton().readObjectAsUntrackedPointer(slot, roots.get(i + 1) != 0).rawValue());
        }
    }

    @Uninterruptible(reason = "Duplicate root locations use the same pre-repair values.")
    private void repairRoots() {
        for (long i = 0; i < roots.size(); i += 3) {
            Pointer before = Word.pointer(roots.get(i + 2));
            Pointer after = forward(before);
            if (before.notEqual(after)) DerivedReferenceSupport.writeReference(Word.pointer(roots.get(i)), after, roots.get(i + 1) != 0);
        }
    }

    @Uninterruptible(reason = "Preserve interior displacements independently of base-slot repair ordering.")
    private void addDerived(Pointer base, Pointer slot, boolean compressed) {
        Pointer baseValue = DerivedReferenceSupport.readReferenceAsPointer(base, compressed);
        Pointer value = DerivedReferenceSupport.readReferenceAsPointer(slot, compressed);
        derived.add(slot.rawValue()); derived.add(baseValue.rawValue()); derived.add(value.rawValue()); derived.add(compressed ? 1 : 0);
    }

    @Uninterruptible(reason = "Repair stack and continuation interior pointers before copying their storage.")
    private void repairDerived() {
        for (long i = 0; i < derived.size(); i += 4) {
            Pointer base = Word.pointer(derived.get(i + 1));
            Pointer before = Word.pointer(derived.get(i + 2));
            if (base.isNull() || before.isNull()) continue;
            Pointer afterBase = forward(base);
            Pointer after = afterBase.isNull() ? Word.nullPointer() : afterBase.add(before.subtract(base));
            if (before.notEqual(after)) DerivedReferenceSupport.writeReference(Word.pointer(derived.get(i)), after, derived.get(i + 3) != 0);
        }
    }

    @Uninterruptible(reason = "Java weak clearing precedes newly dead generalized finalizer roots.", calleeMustBe = false)
    private void processReferences(long start, long limit, boolean phantom) {
        for (long i = start; i < limit; i++) {
            Reference<?> reference = (Reference<?>) heap.decode((int) references.get(i));
            boolean late = reference instanceof PhantomReference<?> || ObjectHandlesImpl.isJNIWeakReference(reference);
            if (late != phantom) continue;
            Pointer target = ReferenceInternals.getReferentPointer(reference);
            if (target.isNull() || marked(target)) continue;
            ReferenceInternals.setReferent(reference, null);
            if (ReferenceInternals.hasQueue(reference) && ReferenceInternals.getNextDiscovered(reference) == null) referenceQueue.add(reference);
        }
    }

    @Uninterruptible(reason = "Only nonmoving old referent slots need explicit narrow repair.")
    private void repairReference(Reference<?> reference, int at) {
        if (minor && at >= 0) {
            Pointer slot = ReferenceInternals.getReferentFieldAddress(reference);
            Pointer before = ReferenceInternals.getReferentPointer(reference);
            Pointer after = forward(before);
            if (before.notEqual(after)) DerivedReferenceSupport.writeReference(slot, after, true);
        }
    }

    private final class RootVisitor implements UninterruptibleObjectReferenceVisitor {
        @Override @Uninterruptible(reason = "Enumerate external root slots.")
        public void visitObjectReferences(Pointer first, boolean compressed, int stride, Object holder, int count) {
            for (int i = 0; i < count; i++) addRoot(first.add(Word.unsigned(i).multiply(stride)), compressed);
        }
        @Override @Uninterruptible(reason = "Trace derived bases as ordinary roots.")
        public void visitDerivedReferenceBase(Pointer slot, boolean compressed, int stride, Object holder) { addRoot(slot, compressed); }
        @Override @Uninterruptible(reason = "Save derived offsets before root repair.")
        public void visitDerivedReference(Pointer base, Pointer slot, boolean compressed, Object holder) { addDerived(base, slot, compressed); }
    }

    private final class FieldVisitor implements UninterruptibleObjectReferenceVisitor {
        @Override @Uninterruptible(reason = "Batch narrow field declarations into Jam's pointer masks.")
        public void visitObjectReferences(Pointer first, boolean compressed, int stride, Object holder, int count) {
            CLongPointer slots = StackValue.get(128 * Long.BYTES);
            for (int start = 0; start < count; start += 128) {
                int n = Math.min(128, count - start);
                if (compressed) {
                    for (int j = 0; j < n; j++) slots.write(j, first.add(Word.unsigned(start + j).multiply(stride)).subtract(heap.base()).unsignedShiftRight(2).rawValue());
                    JamNative.fields(scanner, slots, Word.unsigned(n), 1);
                } else {
                    for (int j = 0; j < n; j++) traceTarget(ReferenceAccess.singleton().readObjectAsUntrackedPointer(first.add(Word.unsigned(start + j).multiply(stride)), false), scanner);
                }
            }
        }
        @Override @Uninterruptible(reason = "Trace the base, never an interior address.")
        public void visitDerivedReferenceBase(Pointer slot, boolean compressed, int stride, Object holder) { visitObjectReferences(slot, compressed, stride, holder, 1); }
        @Override @Uninterruptible(reason = "Save continuation-derived references for source repair.")
        public void visitDerivedReference(Pointer base, Pointer slot, boolean compressed, Object holder) { addDerived(base, slot, compressed); }
    }

    private final class RepairVisitor implements UninterruptibleObjectReferenceVisitor {
        @Override @Uninterruptible(reason = "Repair wide fields and nonmoving old fields; SIMD repairs collected narrow fields.")
        public void visitObjectReferences(Pointer first, boolean compressed, int stride, Object holder, int count) {
            if (compressed && (!minor || heap.isYoung(Word.objectToUntrackedPointer(holder)))) return;
            for (int i = 0; i < count; i++) {
                Pointer slot = first.add(Word.unsigned(i).multiply(stride));
                Pointer before = ReferenceAccess.singleton().readObjectAsUntrackedPointer(slot, compressed);
                Pointer after = forward(before);
                if (before.notEqual(after)) DerivedReferenceSupport.writeReference(slot, after, compressed);
            }
        }
        @Override @Uninterruptible(reason = "Repair an ordinary base slot.")
        public void visitDerivedReferenceBase(Pointer slot, boolean compressed, int stride, Object holder) { visitObjectReferences(slot, compressed, stride, holder, 1); }
        @Override @Uninterruptible(reason = "Derived values are repaired from their saved displacement.")
        public void visitDerivedReference(Pointer base, Pointer slot, boolean compressed, Object holder) { }
    }

    private final class ImageVisitor implements UninterruptibleObjectVisitor {
        @Override @Uninterruptible(reason = "Permanent image objects supply external roots.", callerMustBe = true)
        public void visitObject(Object object) {
            InteriorObjRefWalker.walkObjectInline(object, rootVisitor);
            if (object instanceof Reference<?>) {
                Reference<?> reference = (Reference<?>) object;
                references.add(heap.encode(object) & 0xffffffffL);
                recordRoot(ReferenceInternals.getReferentFieldAddress(reference), true);
                if (!clearSoft && reference instanceof SoftReference<?>) traceTarget(ReferenceInternals.getReferentPointer(reference), Word.nullPointer());
            }
        }
    }

    private final class OldVisitor implements UninterruptibleObjectVisitor {
        @Override @Uninterruptible(reason = "The heap enumerates each dirty old owner once.", callerMustBe = true)
        public void visitObject(Object object) {
            CIntPointer owner = StackValue.get(Integer.BYTES);
            owner.write(heap.encode(object));
            JamNative.traceOld(heap.nativeHeap(), owner, Word.unsigned(1), SCAN.getFunctionPointer(), context);
        }
    }

    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    UninterruptibleObjectReferenceVisitor rootVisitor() { return rootVisitor; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public RuntimeCodeInfoGCSupport getRuntimeCodeInfoGCSupport() { return codeRoots; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    public UnsignedWord getUsedMemoryAfterLastGC() { return Word.unsigned(usedAfterGC); }
    public long getMillisSinceLastWholeHeapExamined() { return System.currentTimeMillis() - lastMajorMillis; }
    long lastMajorTime() { return lastMajorMillis; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long minorCount() { return minorCount; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long majorCount() { return majorCount; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long minorMillis() { return minorNanos / 1_000_000; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    long majorMillis() { return majorNanos / 1_000_000; }
    @Uninterruptible(reason = Uninterruptible.CALLED_FROM_UNINTERRUPTIBLE_CODE, mayBeInlined = true)
    UnsignedWord collectedMemory() { return Word.unsigned(usedAfterGC); }
    public void doReferenceHandling() { if (ReferenceHandler.isExecutedManually()) ReferenceHandler.processPendingReferencesInRegularThread(); }
    @Uninterruptible(reason = "Reference queue locking.")
    public boolean hasReferencePendingList() { return referenceQueue.hasReferencePendingList(); }
    public void waitForReferencePendingList() throws InterruptedException { referenceQueue.waitForReferencePendingList(); }
    @Uninterruptible(reason = "Reference queue locking.")
    public void wakeUpReferencePendingListWaiters() { referenceQueue.wakeUpReferencePendingListWaiters(); }
    @Uninterruptible(reason = "Reference queue locking.")
    public Reference<?> getAndClearReferencePendingList() { return referenceQueue.getAndClearReferencePendingList(); }
    @Uninterruptible(reason = "Isolate tear down.")
    void tearDown() { objects.release(); references.release(); roots.release(); derived.release(); }
}
