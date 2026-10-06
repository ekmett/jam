<p class="thc-eyebrow">A jam garbage collector for the JVM</p>

# jam-vm

This project adapts [jam](https://github.com/ekmett/jam) to collect JVM objects,
with both Java reference processing and GHC-style generalized weak pointers.
The intended consumer is [thc](https://github.com/ekmett/thc): Haskell running
through Truffle/Graal, with its closures in the host heap.

## Using Jam

First, [build the patched JDK](../build.md). Then select Jam with that JVM:

```sh
java -Xshare:off -Xms256m -Xmx256m \
  -XX:+UnlockExperimentalVMOptions -XX:+UseJamGC -Xlog:gc \
  -jar application.jar
```

Keep `-Xms` and `-Xmx` equal. Ordinary Java code continues to use its existing
reference classes. Code that needs generalized weak associations can use
`jam.vm.Weak` from the [Java/JNI API](../thc-integration.md).

## Weak associations

A weak association connects a key to a value and an optional finalizer.
The value may refer back to its key without keeping the association alive:

```text
weak association:  key ⇒ value
                    ▲      │
                    └──────┘
```

If something else keeps the key alive, the value stays available. Otherwise
the association dies, even though its value could have led us back to the key.
The [weak-pointer guide](../weak-pointers.md) explains the fixed point behind
that rule and what happens when finalizers resurrect objects.

After adding the API to your runtime, install an association:

```java
import jam.vm.Weak;

Weak.checkAvailable();
long token = Weak.create(key, value, () -> releaseResource());
Object result = Weak.deref(token);
```

Finalizers run when a caller pumps the shared queue:

```java
int completed = Weak.pump();
```

A finalizer is a JVM `Runnable`. It may enter whatever guest context it needs;
jam-vm does not need to know which context supplied it. Any caller can pump.
See [integrating thc](../thc-integration.md) for build instructions, explicit
finalization and the claim/completion protocol.

## Finding your way around

The [build guide](../build.md) covers the source pins and native libraries.
[Supported configurations](../status.md) lists current limits. Start there if
you want to run the collector or embed the weak API.

For work on the collector itself, [heap architecture](../architecture.md)
derives the compressed-oop encoding from jam's cell offsets. The
[HotSpot integration](../hotspot-integration.md) describes the C++26/C++14
boundary, roots, barriers and collection phases.

## Contact Information

Contributions and bug reports are welcome!

Please feel free to contact me through [GitHub](https://github.com/ekmett/jam-vm).

-Edward Kmett
