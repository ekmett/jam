// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import jam.vm.Weak;
import java.lang.ref.Reference;
import java.lang.ref.WeakReference;

/** Exercise the packaged boundary without the test-only collector JNI library. */
public final class WeakBridgeSmoke {
    private static int executions;
    private static Object resurrected;
    private record Chain(Object root, long first, long second) { }
    private record Batch(long first, long second, WeakReference<Object> weak) { }
    private record GuestClosure(Object captured) implements Runnable {
        @Override public void run() {
            System.gc();
            check(captured != null, "captured guest object survives execution");
            executions++;
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static long deadAssociation() {
        Object key = new Object();
        return Weak.create(key, new GuestClosure(key), new GuestClosure(key));
    }

    private static long install(Runnable finalizer) {
        return Weak.create(new Object(), new Object(), finalizer);
    }

    private static Chain chain() {
        Object first = new Object();
        Object second = new Object();
        long tail = Weak.create(second, new byte[4096], null);
        return new Chain(first, Weak.create(first, second, null), tail);
    }

    private static Batch batch(int[] count) {
        Object key = new Object();
        long first = Weak.create(key, new Object(), () -> {
            System.gc();
            resurrected = key;
            count[0]++;
        });
        long second = Weak.create(key, new Object(), () -> count[0]++);
        return new Batch(first, second, new WeakReference<>(key));
    }

    private static void generalized() {
        Chain chain = chain();
        for (int i = 0; i < 4; i++) {
            System.gc();
            check(Weak.deref(chain.first()) != null && Weak.deref(chain.second()) instanceof byte[],
                  "reverse registration order reaches a fixed point");
        }
        Reference.reachabilityFence(chain.root());
        int[] count = new int[1];
        Batch batch = batch(count);
        System.gc();
        check(Weak.deref(batch.first()) == null && Weak.deref(batch.second()) == null,
              "all dead associations freeze before tracing new finalizers");
        check(batch.weak().get() == null, "Java weak clears before generalized finalizer resurrection");
        check(Weak.pump() == 2 && count[0] == 2 && resurrected != null, "shared-key finalizers survive nested GC");
        System.gc();
        check(Weak.deref(batch.first()) == null && Weak.deref(batch.second()) == null && Weak.pump() == 0,
              "resurrection cannot rearm either association");
        resurrected = null;
        long permanent = Weak.create(WeakBridgeSmoke.class, new byte[4096], null);
        System.gc();
        check(Weak.deref(permanent) instanceof byte[], "permanent image key retains a moving value");
        Weak.finalizeNow(permanent);
        Weak.complete(permanent);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 0) {
            try {
                Weak.checkAvailable();
                throw new AssertionError("unsupported runtime admitted");
            } catch (UnsatisfiedLinkError | UnsupportedOperationException expected) {
                System.out.println("Weak bridge unavailable as expected: " + expected.getMessage());
                return;
            }
        }
        Weak.checkAvailable();
        Object key = new Object();
        Object value = new Object();
        GuestClosure finalizer = new GuestClosure(key);
        long live = Weak.create(key, value, finalizer);
        System.gc();
        check(Weak.deref(live) == value, "live key retains value");
        check(Weak.finalizeNow(live) == finalizer, "opaque closure returned");
        try {
            System.gc();
            check(Weak.deref(live) == null, "explicit retirement is irreversible");
            check(Weak.finalizeNow(live) == null, "running closure cannot be claimed twice");
        } finally {
            Weak.complete(live);
        }
        Weak.complete(live);
        Reference.reachabilityFence(key);

        long dead = deadAssociation();
        System.gc();
        check(Weak.deref(dead) == null, "value and finalizer cannot activate their own key");
        long[] token = { -1 };
        Object claimed = Weak.take(token);
        check(claimed instanceof GuestClosure && token[0] == dead, "queued guest closure claimed");
        try {
            System.gc();
            check(((GuestClosure) claimed).captured() != null, "claimed closure survives nested collection");
            check(Weak.finalizeNow(dead) == null, "explicit and queued claims share state");
        } finally {
            Weak.complete(token[0]);
        }
        check(Weak.take(token) == null && token[0] == 0, "empty poll resets token");
        check(Weak.deref(0) == null && Weak.finalizeNow(-1) == null, "unknown tokens do not alias");
        Weak.complete(0);
        try {
            Weak.create(null, value, null);
            throw new AssertionError("null key accepted");
        } catch (NullPointerException expected) {}
        try {
            Weak.take(new long[0]);
            throw new AssertionError("empty output accepted");
        } catch (IllegalArgumentException expected) {}
        long pumped = deadAssociation();
        System.gc();
        check(Weak.pump() == 1 && executions == 1, "any host can pump a JVM runnable");
        check(Weak.finalizeNow(pumped) == null && Weak.pump() == 0, "pump completes its claim once");
        int[] independentOwners = new int[2];
        install(() -> independentOwners[0]++);
        install(() -> independentOwners[1]++);
        System.gc();
        check(Weak.pump() == 2 && independentOwners[0] == 1 && independentOwners[1] == 1,
              "one pump runs callbacks from independent owners");
        RuntimeException failure = new RuntimeException("guest failure");
        long throwing = install(() -> { throw failure; });
        System.gc();
        try {
            Weak.pump();
            throw new AssertionError("guest failure swallowed");
        } catch (RuntimeException expected) {
            check(expected == failure, "guest failure propagated");
        }
        check(Weak.finalizeNow(throwing) == null && Weak.pump() == 0,
              "throwing finalizer is completed without retry");
        generalized();
        System.out.println("Weak bridge passed: JVM runnables, retirement, pumping and nested GC");
        if (Boolean.getBoolean("jam.runtime.audit")) {
            System.clearProperty("jam.runtime.audit");
            System.out.println("jam-runtime-audit-ready");
            System.out.flush();
            if (System.in.read() != '\n') throw new AssertionError("runtime audit did not resume");
        }
    }
}
