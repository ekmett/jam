# Experimental CRaC provider

The Linux CRaC port is tracked in [#54](https://github.com/ekmett/jam/issues/54).
It is not part of the released runtime. The candidate builds and passes a
simulation-engine check of Jam's checkpoint callbacks, weak references and
pending finalizers. Actual process checkpoint/restore and Graal compilation
across restore remain unqualified.

The port keeps the pinned LabsJDK 25 and JVMCI version. It takes CRaC commit
`25d6782a1c26965f21b62638213d9a9cedd75004`, immediately before CRaC moved to
JDK 27, relative to its JDK 25+26 base. The separately pinned upstream delta
supplies CRaC's source files. A local compatibility patch preserves LabsJDK's
typed invalidation reasons, JVMCI reporting and Jam's platform hooks.

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

This CRaC snapshot expects the modified CRIU release 1.4 described by its
upstream README. The engine identifies itself as `3.17.1-crac`. It requires
privileged execution; acquiring it does not grant those privileges. Do not
silently install it setuid. Before distributing a provider, qualify actual
restore with heap alias coherence, Java weak references, pending Jam finalizers,
post-restore allocation and collections, and the prepared THC main wrapper.
