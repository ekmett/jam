// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
package com.oracle.svm.core.jam;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.List;
import org.graalvm.nativeimage.Platform;
import com.oracle.svm.core.SubstrateOptions;
import com.oracle.svm.core.feature.InternalFeature;
import com.oracle.svm.guest.staging.util.UserError;
import com.oracle.svm.hosted.FeatureImpl.BeforeImageWriteAccessImpl;
import com.oracle.svm.shared.feature.AutomaticallyRegisteredFeature;

/** Ship the native collector with its executable, independent of the build JDK's location. */
@AutomaticallyRegisteredFeature
final class JamRuntimeBundleFeature implements InternalFeature {
    private List<String> libraries;
    private String directory;

    @Override public boolean isInConfiguration(IsInConfigurationAccess access) { return SubstrateOptions.useJamGC(); }

    @Override public void beforeImageWrite(BeforeImageWriteAccess access) {
        boolean darwin = Platform.includedIn(Platform.DARWIN.class);
        boolean windows = Platform.includedIn(Platform.WINDOWS.class);
        UserError.guarantee(darwin || windows || Platform.includedIn(Platform.LINUX.class), "Jam Native Image runtime packaging requires macOS, Linux or Windows");
        BeforeImageWriteAccessImpl hosted = (BeforeImageWriteAccessImpl) access;
        directory = hosted.getOutputFilename() + ".jam";
        Path libraryDirectory = JamNative.Directives.libraryDirectory();
        Path manifest = libraryDirectory.resolve("runtime-libraries.txt");
        try {
            libraries = Files.isRegularFile(manifest) ? Files.readAllLines(manifest) :
                            darwin ? List.of("libjam-vm.dylib", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib") : List.of();
        } catch (IOException error) {
            throw UserError.abort("Unable to read packaged Jam runtime manifest %s: %s", manifest, error.getMessage());
        }
        UserError.guarantee(!libraries.isEmpty() && libraries.stream().distinct().count() == libraries.size() &&
                        libraries.contains(windows ? "jam-vm.dll" : darwin ? "libjam-vm.dylib" : "libjam-vm.so"), "Missing or invalid Jam runtime manifest: %s", manifest);
        for (String library : libraries) {
            UserError.guarantee(library.matches(windows ? "[A-Za-z0-9_+.-]+\\.dll" : "lib[A-Za-z0-9_+.-]+"), "Invalid Jam runtime library name: %s", library);
            UserError.guarantee(Files.isRegularFile(libraryDirectory.resolve(library)), "Missing packaged Jam runtime: %s", libraryDirectory.resolve(library));
        }
        if (!windows) {
            hosted.registerLinkerInvocationTransformer(linker -> {
                linker.addRPath((darwin ? "@loader_path/" : "$ORIGIN/") + directory);
                return linker;
            });
        }
    }

    @Override public void afterImageWrite(AfterImageWriteAccess access) {
        Path output = access.getImagePath().resolveSibling(directory);
        boolean windows = Platform.includedIn(Platform.WINDOWS.class);
        Path runtimeOutput = windows ? access.getImagePath().toAbsolutePath().getParent() : output;
        Path libraryDirectory = JamNative.Directives.libraryDirectory();
        Path legal = Path.of(System.getProperty("java.home"), "legal", "jam-vm");
        try {
            Files.createDirectories(output);
            for (String library : libraries) {
                Path source = libraryDirectory.resolve(library);
                Path destination = runtimeOutput.resolve(library);
                if (windows && Files.exists(destination)) {
                    UserError.guarantee(Files.isRegularFile(destination) && Files.mismatch(source, destination) == -1,
                                    "A different runtime already exists at %s; use a separate output directory", destination);
                } else {
                    Files.copy(source, destination, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.COPY_ATTRIBUTES);
                }
            }
            Files.write(output.resolve("runtime-libraries.txt"), libraries);
            Files.createDirectories(output.resolve("legal"));
            try (var files = Files.list(legal)) {
                for (Path file : files.toList()) {
                    if (Files.isRegularFile(file)) Files.copy(file, output.resolve("legal").resolve(file.getFileName()), StandardCopyOption.REPLACE_EXISTING);
                }
            }
        } catch (IOException error) {
            throw UserError.abort("Unable to bundle Jam runtime beside %s: %s", access.getImagePath(), error.getMessage());
        }
    }
}
