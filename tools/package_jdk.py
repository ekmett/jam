#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Bundle Jam and its guest API into a relocatable macOS JDK image."""

import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM = ('/usr/lib/', '/System/Library/')


def macho(path):
    with path.open('rb') as source:
        header = source.read(8)
    if header[:4] in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xce'):
        return True
    if header[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        return 0 < int.from_bytes(header[4:], 'big') < 20
    return False


def load_commands(path):
    result = subprocess.check_output(['otool', '-l', str(path)], text=True)
    identity, dependencies, rpaths = None, set(), set()
    for block in result.split('Load command ')[1:]:
        command = re.search(r'^\s*cmd (LC_\w+)$', block, re.MULTILINE)
        value = re.search(r'^\s*(?:name|path) (.*?) \(offset \d+\)$', block, re.MULTILINE)
        if not command or not value:
            continue
        kind, name = command[1], value[1]
        if kind == 'LC_ID_DYLIB':
            identity = name
        elif kind == 'LC_RPATH':
            rpaths.add(name)
        elif kind in ('LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
                      'LC_LAZY_LOAD_DYLIB', 'LC_LOAD_UPWARD_DYLIB'):
            dependencies.add(name)
    return identity, dependencies, rpaths


def package(java_home, output, runtime):
    if platform.system() != 'Darwin':
        raise SystemExit('JDK packaging is currently supported on macOS.')
    java_home, output, runtime = java_home.resolve(), output.resolve(), runtime.resolve()
    if output.exists():
        raise SystemExit(f'Preserving existing output: {output}')
    if not (java_home / 'bin/java').is_file():
        raise SystemExit('Pass the JDK home containing bin/java.')
    if output.is_relative_to(java_home):
        raise SystemExit('The output must be outside the source JDK.')
    libraries = {
        'libjam_vm.dylib': ROOT / 'build-jam/libjam_vm.dylib',
        'libjam_bridge.dylib': ROOT / 'build/bridge/lib/libjam_bridge.dylib',
        'libc++.1.dylib': runtime / 'lib/c++/libc++.1.dylib',
        'libc++abi.1.dylib': runtime / 'lib/c++/libc++abi.1.dylib',
        'libunwind.1.dylib': runtime / 'lib/unwind/libunwind.1.dylib',
    }
    licenses = {
        'LICENSE.md': ROOT / 'LICENSE.md',
        'NOTICE.md': ROOT / 'NOTICE.md',
        'jam-LICENSE.md': ROOT / 'upstream/jam/LICENSE.md',
        'native-LICENSE.md': ROOT / 'upstream/native/LICENSE.md',
        'LLVM-LICENSE.txt': runtime / 'LICENSE.TXT',
    }
    jars = [ROOT / 'build/bridge' / name for name in ('jam-vm.jar', 'jam-vm-sources.jar')]
    header = ROOT / 'adapter/jam_vm.h'
    for source in [*libraries.values(), *licenses.values(), *jars, header]:
        if not source.is_file():
            raise SystemExit(f'Missing package input: {source}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='jam-package-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'jdk'
        shutil.copytree(java_home, stage, symlinks=True)
        library_dir = stage / 'lib/jam'
        library_dir.mkdir(parents=True)
        legal_dir = stage / 'legal/jam-vm'
        legal_dir.mkdir(parents=True)
        for name, source in libraries.items():
            shutil.copy2(source, library_dir / name)
        for source in jars:
            shutil.copy2(source, library_dir / source.name)
        (library_dir / 'include').mkdir()
        shutil.copy2(header, library_dir / 'include/jam_vm.h')
        for name, source in licenses.items():
            shutil.copy2(source, legal_dir / name)

        binaries = []
        for path in stage.rglob('*'):
            if path.is_symlink():
                target = Path(os.readlink(path))
                if target.is_absolute():
                    if not target.is_relative_to(java_home):
                        raise SystemExit(f'External symlink in JDK: {path.relative_to(stage)} -> {target}')
                    path.unlink()
                    path.symlink_to(os.path.relpath(stage / target.relative_to(java_home), path.parent))
                if not path.resolve().is_relative_to(stage) or not path.exists():
                    raise SystemExit(f'Broken or external symlink in JDK: {path.relative_to(stage)}')
            elif path.is_file() and macho(path):
                binaries.append(path)

        for path in binaries:
            identity, dependencies, rpaths = load_commands(path)
            changes = []
            if identity and (path.parent == library_dir or identity.startswith('/')):
                changes += ['-id', '@rpath/' + path.name]
            for dependency in sorted(dependencies):
                if dependency.startswith(SYSTEM):
                    continue
                if Path(dependency).name in libraries:
                    target = library_dir / Path(dependency).name
                elif path.parent == library_dir:
                    raise SystemExit(f'Unbundled Jam runtime dependency in {path.name}: {dependency}')
                elif dependency.startswith('/') and Path(dependency).is_relative_to(java_home):
                    target = stage / Path(dependency).relative_to(java_home)
                elif dependency.startswith('/'):
                    raise SystemExit(f'Unbundled dependency in {path.relative_to(stage)}: {dependency}')
                else:
                    continue
                replacement = '@loader_path/' + os.path.relpath(target, path.parent)
                if dependency != replacement:
                    changes += ['-change', dependency, replacement]
            for rpath in sorted(rpaths):
                if path.parent == library_dir:
                    # These libraries are flattened into lib/jam and every
                    # dependency above now has a direct loader-relative path.
                    changes += ['-delete_rpath', rpath]
                    continue
                if rpath.startswith('@loader_path/'):
                    target = path.parent / rpath.removeprefix('@loader_path/')
                    if not target.exists() and all(dep.startswith(SYSTEM) for dep in dependencies):
                        # Statically linked distribution tools can retain an
                        # unused build-layout search path (for example lld).
                        changes += ['-delete_rpath', rpath]
                        continue
                if not rpath.startswith('/') or rpath.startswith(SYSTEM):
                    continue
                resolved = Path(rpath).resolve()
                if resolved.is_relative_to(java_home):
                    target = stage / resolved.relative_to(java_home)
                    replacement = '@loader_path/' + os.path.relpath(target, path.parent)
                    changes += ['-rpath', rpath, replacement]
                elif any(resolved.is_relative_to(base) for base in (ROOT, runtime)):
                    changes += ['-delete_rpath', rpath]
                else:
                    raise SystemExit(f'External runtime path in {path.relative_to(stage)}: {rpath}')
            if changes:
                path.chmod(path.stat().st_mode | 0o200)
                subprocess.run(['install_name_tool', *changes, str(path)], check=True)
                signed = subprocess.run(['codesign', '--force', '--sign', '-',
                                         '--preserve-metadata=identifier,entitlements,flags,runtime', str(path)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                if signed.returncode:
                    raise SystemExit(f'Could not sign {path.relative_to(stage)}: {signed.stderr}')

        # Check every native image after rewriting, including native-image and libgraal.
        for path in binaries:
            identity, dependencies, rpaths = load_commands(path)
            for value in [identity, *dependencies, *rpaths]:
                if value and value.startswith('/') and not value.startswith(SYSTEM):
                    raise SystemExit(f'External load path remains in {path.relative_to(stage)}: {value}')
            for value in dependencies | rpaths:
                if value.startswith('@loader_path/'):
                    target = (path.parent / value.removeprefix('@loader_path/')).resolve()
                    if not target.is_relative_to(stage) or not target.exists():
                        raise SystemExit(f'Broken or external loader path in {path.relative_to(stage)}: {value}')
        stage.rename(output)
    print(f'Packaged Jam JDK: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-prefix', type=Path,
                        default=os.environ.get('JAM_LIBCXX_PREFIX', '/opt/homebrew/opt/llvm@22'))
    options = parser.parse_args()
    package(options.java_home, options.output, options.runtime_prefix)
