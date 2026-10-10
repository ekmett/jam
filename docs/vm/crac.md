# Experimental CRaC provider

The Linux CRaC port is tracked in [#54](https://github.com/ekmett/jam/issues/54).
It is not part of the released runtime. The candidate builds and passes a
simulation-engine check of Jam's checkpoint callbacks, weak references and
pending finalizers, with the released libgraal compiling and executing methods
on both sides of the callback. Actual process checkpoint/restore and Graal
compilation across real restore remain unqualified.

The port keeps the pinned LabsJDK 25 and JVMCI version. It takes CRaC commit
`25d6782a1c26965f21b62638213d9a9cedd75004`, immediately before CRaC moved to
JDK 27, relative to its JDK 25+26 base. The separately pinned upstream delta
supplies CRaC's source files. A local compatibility patch preserves LabsJDK's
typed invalidation reasons, JVMCI reporting and Jam's platform hooks. CRaC's
additional x86 host-feature checks remain internal: the JVMCI export keeps the
65 feature names and bit indices understood by the pinned LabsJDK compiler.

Prepare a fresh source tree with the same LabsJDK archive used by the normal
GraalVM build:

```sh
python3 vm/tools/prepare_crac.py \
  --labsjdk-archive vm/upstream/labsjdk25.tar.gz \
  --output /path/to/fresh/jam-crac
```

The script verifies the archive and upstream delta hashes and refuses to replace
an existing output directory. `--crac-patch` accepts a previously downloaded
copy of that same delta. It does not change the ordinary runtime source pins or
prepare the standard `upstream/labsjdk25` directory.

Configure that tree with the normal LabsJDK/Jam build options and native bridge
library, then build `images`. Keep the existing JVMCI version string. The
candidate's `jdk.crac` module exports `jdk.crac.Core.checkpointRestore()`.

`vm/t/crac/CracJamSmoke.java` uses the existing collector-identity and weak smoke
checks. Compile it with `--add-modules jdk.crac`, the guest bridge JAR and the
`CollectorIdentitySmoke`, `JamWeak`, `WeakSmoke` and `HeapSmoke` test sources.
Run with equal `-Xms` and `-Xmx`, Jam selected, JVMCI enabled, the test JNI and
bridge libraries on `java.library.path`, and the JVMCI hotspot package exported
to unnamed modules. `-XX:CRaCEngine=simengine` exercises callbacks without taking
a process image; it cannot establish restore correctness.

For a focused compiler compatibility check, copy the candidate JDK into an
isolated directory and add the released Jam provider's unchanged
`lib/libjvmcicompiler.so` and `lib/jam/`. This is a LabsJDK plus libgraal test
image, not a complete GraalVM or Native Image distribution. Keep the original
candidate and released provider intact.

```sh
python3 vm/tools/check_crac_graal.py \
  --java-home /path/to/isolated/crac-graal-jdk \
  --native-tests vm/build-java-tests/native \
  --output /path/to/fresh/crac-graal-evidence
```

The probe forces a reference-store method through Graal before the simulation
callback, then checks the method again and compiles a second one afterward. It
requires installed level-4 code and JVMCI entries in the compilation log, and
checks live references after another collection. It also runs the checkpoint,
weak-reference and pending-finalizer checks above. Simulation evidence does not
qualify CRIU restore or THC's compiled application.

Libgraal caches an open descriptor for its image backing file to initialize
future isolates. The probe supplies `-XX:CRaCAllowedOpenFilePrefixes=` with the
candidate's full `lib/libjvmcicompiler.so` path. This asks the engine to preserve
that resource; it does not close the descriptor or allow arbitrary open files.
Actual restore must still prove that the descriptor and backing image remain
usable. The compiler log is also reopened by CRaC, so the probe preserves a
pre-callback fragment alongside the final log.

This CRaC snapshot expects the modified CRIU release 1.4 described by its
upstream README. The engine identifies itself as `3.17.1-crac`. It requires
privileged execution; acquiring it does not grant those privileges. Do not
silently install it setuid. Before distributing a provider, qualify actual
restore with heap alias coherence, Java weak references, pending Jam finalizers,
post-restore allocation and collections, and the prepared THC main wrapper.
