// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import jam.vm.Weak;
import java.lang.ref.Reference;

/** Exercise the packaged boundary without the test-only collector JNI library. */
public final class WeakBridgeSmoke {
    private static int executions;
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

    public static void main(String[] args) {
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
        System.out.println("Weak bridge passed: JVM runnables, retirement, pumping and nested GC");
    }
}
