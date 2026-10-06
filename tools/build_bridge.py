#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

"""Build the guest API and JNI shim without building jam or HotSpot."""

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(java_home):
    jdk = Path(java_home).resolve()
    system = platform.system()
    if system not in ("Darwin", "Linux"):
        raise SystemExit("The bridge currently supports Darwin and Linux builds")
    include = "darwin" if system == "Darwin" else "linux"
    library = "libjam_bridge.dylib" if system == "Darwin" else "libjam_bridge.so"
    output = ROOT / "build/bridge"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bridge-", dir=output.parent) as temporary:
        stage = Path(temporary)
        for directory in ("classes", "include", "lib", "api"):
            (stage / directory).mkdir()
        sources = sorted((ROOT / "bridge/java").rglob("*.java"))
        subprocess.run([str(jdk / "bin/javac"), "--release", "25", "-Xlint:all", "-Werror",
                        "-h", str(stage / "include"), "-d", str(stage / "classes"), *map(str, sources)], check=True)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-dynamiclib"] if system == "Darwin" else ["-shared", "-fPIC"]
        subprocess.run([*compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fvisibility=hidden", *flags, "-I" + str(jdk / "include"),
                        "-I" + str(jdk / "include" / include), "-I" + str(stage / "include"),
                        str(ROOT / "bridge/jam_bridge.c"), "-o", str(stage / "lib" / library),
                        *([] if system == "Darwin" else ["-ldl"])], check=True)
        manifest = stage / "MANIFEST.MF"
        manifest.write_text("Manifest-Version: 1.0\nAutomatic-Module-Name: jam.vm\n\n")
        subprocess.run([str(jdk / "bin/jar"), "--create", "--file", str(stage / "jam-vm.jar"),
                        "--manifest", str(manifest), "-C", str(stage / "classes"), "."], check=True)
        subprocess.run([str(jdk / "bin/jar"), "--create", "--file", str(stage / "jam-vm-sources.jar"),
                        "-C", str(ROOT / "bridge/java"), "."], check=True)
        subprocess.run([str(jdk / "bin/javadoc"), "-quiet", "-Xdoclint:all", "-Werror",
                        "-d", str(stage / "api"), *map(str, sources)], check=True)
        # Replace generated outputs only, after every tool has succeeded.
        for name in ("jam-vm.jar", "jam-vm-sources.jar", "lib", "include", "api"):
            destination = output / name
            if destination.is_dir():
                shutil.rmtree(destination)
            elif destination.exists():
                destination.unlink()
            shutil.move(str(stage / name), destination)
        shutil.copyfile(ROOT / "LICENSE.md", output / "LICENSE.md")
    print(f"Built guest API, JNI library, headers and Javadoc: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", default=os.environ.get("JAM_BOOT_JDK", os.environ.get("JAVA_HOME")))
    options = parser.parse_args()
    if not options.java_home:
        parser.error("set JAM_BOOT_JDK/JAVA_HOME or pass --java-home (JDK 25)")
    build(options.java_home)
