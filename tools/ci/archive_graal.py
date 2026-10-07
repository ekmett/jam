#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Archive the packaged runtime and the exact dependencies of its CI consumers."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from package_jdk import load_commands, SYSTEM

JARS = {
    'truffle': ('truffle-api', 'truffle-runtime', 'truffle-compiler'),
    'sdk': ('polyglot', 'collections', 'jniutils', 'nativebridge', 'nativeimage', 'word'),
}


def portable_jni(source, destination):
    """Drop unused build search paths from the C-only test library's copy."""
    shutil.copy2(source, destination)
    destination.chmod(destination.stat().st_mode | 0o200)
    _, dependencies, rpaths = load_commands(destination)
    if platform.system() == 'Darwin':
        if any(not path.startswith(SYSTEM) for path in dependencies):
            raise SystemExit('The JNI test library has a non-system dependency.')
        if rpaths:
            subprocess.run(['install_name_tool', *[argument for path in sorted(rpaths)
                            for argument in ('-delete_rpath', path)], str(destination)], check=True)
            subprocess.run(['codesign', '--force', '--sign', '-',
                            '--preserve-metadata=identifier,entitlements,flags,runtime',
                            str(destination)], check=True)
    else:
        if dependencies - {'libc.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1'}:
            raise SystemExit('The JNI test library has a non-system dependency.')
        if rpaths:
            subprocess.run(['patchelf', '--remove-rpath', str(destination)], check=True)


def metadata(member):
    member.uid = member.gid = 0
    member.uname = member.gname = ''
    return member


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, default=ROOT / 'build/graalvm')
    parser.add_argument('--output', required=True, type=Path)
    options = parser.parse_args()
    home = options.java_home.resolve()
    output = options.output.resolve()
    if platform.system() not in ('Darwin', 'Linux'):
        raise SystemExit('The CI runtime archive supports macOS and Linux.')
    if output.exists() or output.is_relative_to(home):
        raise SystemExit('Choose a new archive path outside the packaged runtime.')
    for relative in ('bin/java', 'bin/native-image', 'lib/jam/jam-vm.jar', 'legal/jam-vm/NOTICE.md'):
        if not (home / relative).is_file():
            raise SystemExit(f'Missing packaged runtime input: {relative}')
    for path in home.rglob('*'):
        if path.is_symlink() and (os.path.isabs(os.readlink(path)) or
                                  not path.resolve().is_relative_to(home) or not path.exists()):
            raise SystemExit(f'Broken or external runtime symlink: {path.relative_to(home)}')
    jars = [Path('upstream/graal25') / suite / 'mxbuild/dists' / (name + '.jar')
            for suite, names in JARS.items() for name in names]
    jni = Path('build-jam') / ('libjam_jni.dylib' if platform.system() == 'Darwin' else 'libjam_jni.so')
    for relative in (*jars, jni):
        if not (ROOT / relative).is_file():
            raise SystemExit(f'Missing CI consumer input: {relative}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='jam-ci-archive-', dir=output.parent) as temporary:
        copy = Path(temporary) / jni.name
        portable_jni(ROOT / jni, copy)
        archive_path = Path(temporary) / 'runtime.tar.gz'
        with tarfile.open(archive_path, 'w:gz', compresslevel=1) as archive:
            # Preserve the distribution's symlinks, executable modes and signed
            # binary bytes. Keep mx's absolute jar symlinks out of the archive.
            archive.add(home, arcname='graalvm', filter=metadata)
            for relative in jars:
                archive.add((ROOT / relative).resolve(), arcname=str(relative), filter=metadata)
            archive.add(copy, arcname=str(jni), filter=metadata)
        archive_path.rename(output)
    print(f'Archived packaged GraalVM, {len(jars)} jars and the JNI test library: {output}')


if __name__ == '__main__':
    main()
