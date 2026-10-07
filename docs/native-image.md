# Native Image

The [Jam-enabled GraalVM](build.md#graalvm) can also build native executables
whose Java objects live in jam's heap. Select the collector when building the
image:

```sh
export JAVA_HOME=/absolute/path/to/jam-graalvm
"$JAVA_HOME/bin/native-image" --gc=jam -jar application.jar application
./application -Xmx128m -Xmn32m
```

`-Xmx` sets the combined capacity of the two generations. `-Xmn` sets the
nursery capacity; the remainder belongs to old. The defaults are 128 MiB total
and a nursery one quarter that size. These capacities remain fixed for the
lifetime of an isolate.

Keep `application` and its generated `application.jam/` directory together.
That directory contains the collector and its C++ runtime. You can move the
pair out of the build checkout; the executable does not need a JVM installation
at runtime. The current target is macOS arm64.

## Weak associations

Put the same `jam.vm.Weak` API used on HotSpot on the image class path:

```sh
"$JAVA_HOME/bin/native-image" --gc=jam \
  -cp "$JAVA_HOME/lib/jam/jam-vm.jar:application.jar" \
  your.application.Main application
```

Now, register an association and pump finalizers as usual:

```java
long token = Weak.create(key, value, finalizer);
Object result = Weak.deref(token);
int completed = Weak.pump();
```

The image builder connects these methods directly to the collector. There is
no JNI library to load in the executable. Ordinary Java reference queues and
generalized weak associations share the collection's reachability decisions;
a value or finalizer's return edge to its key does not keep the association
alive. See [weak-pointer semantics](weak-pointers.md).

Registrations belong to the current isolate. Any thread in that isolate can
pump them, including callbacks supplied by different Truffle contexts. A
token cannot be passed to another isolate. The installed runnable still owns
entering its guest context and executing the finalizer.

## Native pointers and isolates

Use Native Image's `PinnedObject` when native code must hold an object address.
Jam promotes the object before exposing its address. While a pin is open, old
objects stay put and nursery collection continues. Close pins when the native
operation finishes: a long-lived pin prevents old-space reclamation and can
cause `OutOfMemoryError` even when some old objects are dead.

The normal isolate entry, attachment and teardown APIs select the corresponding
Jam heap. A thread enters a `jam::heap_scope` before executing Java or VM work
and leaves it before returning to native code. Nested native callbacks can
therefore enter another isolate and return to the original one. No heap handle
needs to be passed through guest code.

## Truffle

Use this GraalVM as the image builder for the Truffle application. Keep its
language and Truffle dependencies on the application's normal image class path
and add `--gc=jam` to the build. The runtime compiler uses Jam's allocation and
card barriers; collection repairs stack, continuation and installed-code
references as well as the image heap's writable roots.

The thc owner still needs to lower weak primitives through `jam.vm.Weak` and
schedule a finalizer pump. Selecting the collector supplies the heap semantics;
it does not add language primitives to thc.

## Limits

The adapter uses compressed references with a three-bit shift, eight-byte
object alignment and ordinary object headers. The permanent image heap begins
1 GiB above the compressed base, so old capacity plus its protected prefix must
fit below that boundary. The nursery occupies jam's second 16 GiB address
domain. These are virtual address ranges, not eager physical allocations.

Collection stops Java mutators. Pinning can defer a requested major collection.
Layered images and dynamic class loading are not supported. The shared weak
registry's [metadata limits](status.md#guest-api) also apply to native images.
Native Image does not run legacy `Object.finalize` methods. Use the runnable
finalizers registered through `Weak` for guest finalization.
