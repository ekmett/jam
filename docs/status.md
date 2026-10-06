# Supported configurations

Use the patched JDK 25 or GraalVM 25.3.4.1 on macOS 26 arm64. Select Jam with
`-XX:+UnlockExperimentalVMOptions -XX:+UseJamGC` and set equal initial and
maximum heap sizes. The [build guide](build.md) gives the toolchain and commands.

## Heap and compiler

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

Interpreter, C1, C2 and the patched Graal compiler use Jam's card barriers.
The GraalVM build includes patched libgraal. The VM rejects a compiler that
does not recognize Jam before it can install Java code. Other operating
systems and instruction sets still need validation.

## Guest API

Use [jam.vm.Weak](thc-integration.md) to register weak associations and install
JVM runnables. Any caller can pump the shared queue. There is no automatic
finalizer thread or wakeup notification; the caller supplies the pump schedule.

Tokens are JVM-local and never reused. Dead registrations currently retain
metadata, so native memory use grows with lifetime registrations. Allocation
failure while growing that metadata terminates instead of throwing Java
`OutOfMemoryError`. Keep this in mind for long-running, weak-heavy workloads.

Use the [packaged runtime](build.md#packaging) when moving an installation
out of its build checkout.

## Next steps

The thc owner can use the existing Java hooks to
lower weak primitives and wrap guest finalizers in runnables. GHC C finalizers
and weak-thread resurrection need additional runtime support.

Native Image requires a separate SubstrateVM adapter. Its allocation lowering,
stack maps, image heap, compressed encoding and pinning rules must agree with
jam's representation. Selecting a HotSpot flag does not supply that adapter.

Within the collector, the next work includes reclaiming weak metadata, indexed
weak processing, selective promotion, adaptive capacities and parallel VM
scanning. Jam's SIMD kernels and work scheduler remain the implementation base.
