# Build and run

jam-vm builds two things: a native jam backend and a patched JDK. The backend
uses C++26 modules. The JDK uses its normal C++14 toolchain and calls the
backend through [a C header](../adapter/jam_vm.h).

Start with macOS 26 arm64. Other platform paths still need validation; see
[supported configurations](status.md) for the current limits.

## Tools

Install Git, Python 3, `patch`, and the normal OpenJDK platform build tools.
On macOS that includes Xcode and its SDK. The tested versions are:

| Tool | Version or requirement |
| --- | --- |
| C++ compiler for jam | LLVM 23.1.2 |
| C++ library on Darwin | libc++ 22.1.8, both headers and runtime, from Homebrew `llvm@22` |
| CMake | 4.4.3 |
| Ninja | 1.13 |
| Autoconf | 2.72 |
| GNU M4 / Make | 1.4.20 / 4.4.1 |
| Boot JDK | JDK 24 or 25; the recorded build uses GraalVM Community 25.3.4.1 |
| HotSpot compiler | Apple Clang 21, compiling as C++14 |

The Darwin build pairs LLVM 23's compiler with libc++ 22's headers and runtime.
Keep the selected headers and runtime together.

The scripts default to tools under `.toolchains/`; they do not install those
tools. Override their locations for your installation:

```sh
export JAM_CMAKE=/absolute/path/to/cmake
export JAM_NINJA=/absolute/path/to/ninja
export JAM_CXX=/absolute/path/to/clang++
export JAM_AUTOCONF=/absolute/path/to/autoconf
export JAM_M4=/absolute/path/to/m4
export JAM_MAKE=/absolute/path/to/gmake
export JAM_BOOT_JDK=/absolute/path/to/jdk-25
```

On macOS, use a JDK bundle's `Contents/Home` directory. `JAM_LIBCXX_PREFIX`
defaults to `/opt/homebrew/opt/llvm@22`. `JAM_JOBS` defaults to eight for the JDK
and native backend, and three for GraalVM. The native
test runner expects `ctest` next to the selected `cmake` binary.

## Prepare the sources

```sh
git clone https://github.com/ekmett/jam-vm.git
cd jam-vm
python3 tools/fetch_sources.py --full
python3 tools/prepare_jdk.py
```

The [manifest](../config/source-pins.json) records the source revisions and
archive hashes. `upstream/jam` and `upstream/native` remain clean pinned
checkouts. Jam needs no local patch. Preparation extracts OpenJDK into
`upstream/jdk25` and applies the [HotSpot patch](../patches/hotspot-jam.patch).

The preparation script refuses to replace an existing JDK source directory.
Run it once in a fresh checkout. Repeated builds use the prepared sources.

## Build the backend and JVM

Set `JAM_BOOT_JDK` before the native build so it also builds the JNI test bridge.

```sh
bash tools/build_native.sh
bash tools/build_hotspot.sh
```

The first command builds `build-jam/` and runs the native tests. The second
configures and builds a fastdebug JDK with `jamgc`, `epsilongc` and `serialgc`.
Jam uses Serial's block-offset-table utility, so the Serial build feature is
required even when Jam is the selected collector.

On the tested host, the complete image is under
`upstream/jdk25/build/macosx-aarch64-server-fastdebug/images/jdk/`. Use that
image's `bin/java`:

```sh
export JAM_JAVA="$PWD/upstream/jdk25/build/macosx-aarch64-server-fastdebug/images/jdk/bin/java"
"$JAM_JAVA" -Xshare:off -Xms256m -Xmx256m \
  -XX:+UnlockExperimentalVMOptions -XX:+UseJamGC -Xlog:gc \
  -jar application.jar
```

Startup logging should identify `Using Jam`. Both heap limits must be equal.
The collector validates compressed oops, ordinary object headers, eight-byte
object alignment and its reserved address windows before allocating objects.
`-Xshare:off` makes the lack of archived Java heap support explicit.

`JamYoungSize` selects usable nursery bytes; zero chooses one quarter of the
heap. `JamPromoteEvery` selects the interval between whole-nursery promotion
attempts, defaulting to three minors. `JamWorkers` selects jam's worker count;
the recorded VM tests use four. HotSpot object scanning currently runs on the
VM thread while jam's copy work can run in parallel.

## GraalVM

For Truffle languages, build the GraalVM variant. It pairs LabsJDK with Graal
25.3.4.1, the release used by thc. Both the VM and compiler need the Jam patch;
putting stock libgraal beside a Jam-enabled JDK is not sufficient.

Starting from a fresh checkout, with the tools above configured:

```sh
python3 tools/fetch_sources.py --graal
python3 tools/prepare_jdk.py --graal
python3 tools/prepare_graal.py
export JAM_HOTSPOT_SOURCE="$PWD/upstream/labsjdk25"
bash tools/build_native.sh
bash tools/build_hotspot.sh --graal
bash tools/build_graal.sh
```

This builds `build/graalvm/`, including patched libgraal, the Jam backend and
the weak API. Use it as `JAVA_HOME` and select Jam as above. The compiler uses
Jam's card table for old-to-young stores. Compressed oops remain enabled.

The build uses the pinned `mx` checkout and keeps downloaded build dependencies
in `.toolchains/mx-cache/`. `JAM_GRAAL_OUTPUT` selects another output directory;
the packaging step refuses to overwrite an existing installation.

The distribution also includes the SubstrateVM adapter. Select it with
`native-image --gc=jam` when building a native executable. See
[Native Image](native-image.md) for heap sizing, deployment and the weak API.

## Development checks

```sh
bash tools/check_vm.sh
python3 tools/check_gc_registration.py
build-jam/jam-generational-test --require-simd
python3 tools/check_patches.py
```

`check_vm.sh` uses `JAM_JAVA` when set, with `javac` next to it; `JAM_JAVAC`
overrides that choice. Without overrides it uses the exploded macOS arm64
fastdebug build. These checks exercise the public API, Java reference behavior,
barriers and generation transitions. Generated logs stay local and are ignored
by Git. `check_patches.py` checks the unmodified Jam pin and reconstructs the
HotSpot sources from the patch.

## Packaging

The GraalVM build already packages its runtime. To package the plain JDK:

```sh
jdk="$PWD/upstream/jdk25/build/macosx-aarch64-server-fastdebug/images/jdk"
python3 tools/build_bridge.py --java-home "$jdk"
python3 tools/package_jdk.py --java-home "$jdk" --output build/jam-jdk
```

The output is a JDK home that can be moved out of the checkout. `lib/jam/`
contains the backend, JNI bridge, Java API and C++ runtime libraries. The
packager rewrites native library paths and signs the modified binaries for
local use. It preserves upstream licenses under `legal/`.

For the weak API, put `lib/jam/jam-vm.jar` on the application's class path and
`lib/jam` on `java.library.path`. See [integrating thc](thc-integration.md) for
registration and finalizer pumping.

HotSpot's collector registration is compiled into `libjvm`. A stock JVM cannot
discover Jam through JNI, JVMTI or `-agentpath`. Once the adapter is present,
an ABI-compatible backend change can rebuild just the library. Changes to VM
registration, barriers or the host contract require a HotSpot rebuild. Module
BMI files are build inputs; they are not runtime dependencies.
