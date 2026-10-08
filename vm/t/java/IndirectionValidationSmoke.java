// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import jam.vm.Weak;

/** HotSpot rejects malformed descriptors at registration, before any GC reads. */
public final class IndirectionValidationSmoke {
    static class Carrier {
        volatile int state;
        Object value;
        int plain;
        volatile long wide;
        static Object shared;
        String narrow;
    }
    static final class Child extends Carrier { }
    private static void invalid(Runnable action) {
        try { action.run(); }
        catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("invalid descriptor accepted");
    }
    public static void main(String[] args) {
        Weak.checkAvailable();
        invalid(() -> Weak.registerIndirection(Carrier.class, "missing", 2, "value"));
        invalid(() -> Weak.registerIndirection(Carrier.class, "plain", 2, "value"));
        invalid(() -> Weak.registerIndirection(Carrier.class, "wide", 2, "value"));
        invalid(() -> Weak.registerIndirection(Carrier.class, "state", 2, "shared"));
        invalid(() -> Weak.registerIndirection(Carrier.class, "state", 2, "narrow"));
        invalid(() -> Weak.registerIndirection(Child.class, "state", 2, "value"));
        Weak.registerIndirection(Carrier.class, "state", 2, "value");
        Weak.registerIndirection(Carrier.class, "state", 2, "value");
        invalid(() -> Weak.registerIndirection(Carrier.class, "state", 3, "value"));
        System.out.println("Weak indirection descriptor validation passed");
    }
}
