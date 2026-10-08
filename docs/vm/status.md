# Supported configurations

Use the patched JDK 25 or GraalVM 25.3.4.1 on macOS 26 arm64, Linux x86_64 or
Windows x86_64. The Linux build has been exercised on Ubuntu 22.04 with glibc 2.35;
the Windows build on Windows 11 with Visual Studio 2022. The distributed CI
archives have their own [platform requirements](distribution.md), including
glibc 2.38 for the initial Linux package. Select Jam with
`-XX:+UnlockExperimentalVMOptions -XX:+UseJamGC` and set equal initial and
maximum heap sizes. The [build guide](build.md) gives the toolchain and commands.
Native executables use `native-image --gc=jam` from the patched GraalVM; see
[Native Image](native-image.md) for its separate sizing and pinning rules.

## HotSpot heap and compiler

Jam collects both generations. New objects normally enter young; minor
collections retain young survivors or promote the whole live nursery when it
fits. Major collections compact both generations independently. Java soft,
weak, final and phantom references use the VM's reference processor alongside
the [generalized weak policy](weak-pointers.md).

The current heap uses fixed capacities and stop-the-world collection. It needs
ordinary object headers, eight-byte alignment, normal pages and nonzero-base,
shift-three compressed oops. Each generation, including its guard and copy
reserve, must fit within a 16 GiB domain. Class unloading and CDS heap loading
are disabled.

Interpreter, C1, C2 and the patched Graal compiler record exact old source
slots in Jam's remembered set. Minor collection reads those slots at the safepoint
and follows their current young targets.
The GraalVM build includes patched libgraal. The VM rejects a compiler that
does not recognize Jam before it can install Java code. Other operating
systems and instruction sets still need validation.

## Guest API

Use [jam.vm.Weak](thc-integration.md) to register weak associations and install
JVM runnables. Any caller can pump the shared queue. There is no automatic
finalizer thread or wakeup notification; the caller supplies the pump schedule.

Tokens are local to a JVM or Native Image isolate and never reused. Retired
registrations are excluded from collection scans, but their token records still
retain metadata, so native memory use grows with lifetime registrations. Allocation
failure while growing that metadata terminates instead of throwing Java
`OutOfMemoryError`. Keep this in mind for long-running, weak-heavy workloads.

Use the [packaged runtime](build.md#packaging) when moving an installation
out of its build checkout.

## Next steps

The thc owner can use the existing Java hooks to
lower weak primitives and wrap guest finalizers in runnables. GHC C finalizers
and weak-thread resurrection need additional runtime support.

Within the collector, the next work includes reclaiming weak metadata, indexed
weak processing, selective promotion, adaptive capacities and parallel VM
scanning. Jam's SIMD kernels and work scheduler remain the implementation base.
