// SPDX-FileCopyrightText: 2026 Edward Kmett
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import java.nio.file.Files;
import java.nio.file.Path;
import jdk.vm.ci.hotspot.HotSpotJVMCIRuntime;
import jdk.vm.ci.hotspot.HotSpotResolvedJavaMethod;

/** Forces JVMCI code on both sides of CracJamSmoke's checkpoint callback. */
public final class CracGraalSmoke {
    static final class Cell {
        final int value;
        Cell next;
        Cell(int value) { this.value = value; }
    }
    static volatile long sink;

    static int before(Cell target, Cell value, int n) {
        target.next = value;
        return target.next.value + n;
    }

    static int after(Cell target, Cell value, int n) {
        target.next = value;
        return target.next.value - n;
    }

    static HotSpotResolvedJavaMethod method(String name) throws Exception {
        return (HotSpotResolvedJavaMethod) HotSpotJVMCIRuntime.runtime()
            .getHostJVMCIBackend().getMetaAccess().lookupJavaMethod(
                CracGraalSmoke.class.getDeclaredMethod(name, Cell.class, Cell.class, int.class));
    }

    static void warm(boolean restored, Cell target, Cell value) {
        long sum = 0;
        long expected = 0;
        for (int i = 0; i < 20000; i++) {
            sum += restored ? after(target, value, i) : before(target, value, i);
            expected += restored ? value.value - i : value.value + i;
        }
        sink = sum;
        if (sum != expected || target.next != value)
            throw new AssertionError("compiled result or reference store changed");
    }

    public static void main(String[] args) throws Exception {
        Cell target = new Cell(0);
        Cell value = new Cell(42);
        warm(false, target, value);
        if (!method("before").hasCompiledCodeAtLevel(4))
            throw new AssertionError("before method has no level-4 compiled code");
        System.out.println("Graal before: level-4 code installed");
        // CRaC reopens LogCompilation on return. Preserve the already-written
        // installation records before they are replaced; this is an XML fragment.
        Path log = Path.of(System.getProperty("jam.crac.compilationLog"));
        Files.copy(log, log.resolveSibling("compilation-before.xml"));
        // Includes live data, pending finalizers, the checkpoint, and later GCs.
        CracJamSmoke.main(args);
        System.out.println("Graal before method still compiled after callback: "
            + method("before").hasCompiledCodeAtLevel(4));
        warm(false, target, value);
        warm(true, target, value);
        if (!method("before").hasCompiledCodeAtLevel(4)
                || !method("after").hasCompiledCodeAtLevel(4))
            throw new AssertionError("post-callback compilation did not install code");
        System.gc();
        if (target.next != value || before(target, value, 3) != 45
                || after(target, value, 3) != 39)
            throw new AssertionError("compiled references lost after collection");
        System.out.println("Jam CRaC Graal compiled execution passed before/after callback");
    }
}
