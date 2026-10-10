// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import java.lang.ref.WeakReference;
import jdk.crac.Core;

/** The simengine run checks hooks only; only a CRIU run proves process restore. */
public final class CracJamSmoke {
    static Object keep = new byte[4096];
    static volatile Object rescued;
    static int callbacks;
    static final class Cleanup implements Runnable {
        final Object key;
        Cleanup(Object key) { this.key = key; }
        public void run() { callbacks++; rescued = key; System.gc(); }
    }
    static long pending() {
        Object key = new byte[8192];
        return JamWeak.create(key, key, new Cleanup(key));
    }
    public static void main(String[] args) throws Exception {
        CollectorIdentitySmoke.main(new String[]{"Jam", "JVMCI"});
        byte[] live = (byte[]) keep;
        live[0] = 42;
        WeakReference<Object> weak = new WeakReference<>(live);
        long token = pending();
        System.gc();
        if (JamWeak.deref(token) != null || callbacks != 0)
            throw new AssertionError("finalizer should be pending");
        System.out.println("before checkpoint; guest main has not run");
        Core.checkpointRestore();
        if (live[0] != 42 || weak.get() != keep)
            throw new AssertionError("live data changed across checkpoint");
        JamWeak.runFinalizers();
        JamWeak.runFinalizers();
        if (callbacks != 1 || rescued == null || JamWeak.deref(token) != null)
            throw new AssertionError("pending finalizer lost or ran more than once");
        JamWeak.minor(false);
        JamWeak.minor(true);
        System.gc();
        if (live[0] != 42) throw new AssertionError("post-restore collection lost data");
        WeakSmoke.main(new String[0]);
        System.out.println("Jam CRaC hooks and post-checkpoint weak/GC checks passed");
    }
}
