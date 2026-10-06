#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Run the public weak API using only a packaged JDK and its bundled libraries."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--java-home', required=True, type=Path)
options = parser.parse_args()
home = options.java_home.resolve()
library = home / 'lib/jam'
jar = library / 'jam-vm.jar'
with tempfile.TemporaryDirectory(prefix='jam-package-check-') as temporary:
    classes = Path(temporary) / 'classes'
    subprocess.run([str(home / 'bin/javac'), '-cp', str(jar), '-d', str(classes),
                    str(root / 'tests/bridge/WeakBridgeSmoke.java')], check=True)
    result = subprocess.run([
        str(home / 'bin/java'), '-Xshare:off', '-Xms32m', '-Xmx32m',
        '-XX:+UnlockExperimentalVMOptions', '-XX:+UseJamGC',
        '-XX:+VerifyBeforeGC', '-XX:+VerifyAfterGC', '--enable-native-access=ALL-UNNAMED',
        '-Djava.library.path=' + str(library), '-cp', os.pathsep.join((str(classes), str(jar))),
        'WeakBridgeSmoke'], cwd=temporary, env={**os.environ, 'DYLD_PRINT_LIBRARIES': '1'},
        text=True, capture_output=True, timeout=120)
output = result.stdout + result.stderr
native_image = home / 'bin/native-image'
version = None
if native_image.is_file():
    version = subprocess.run([str(native_image), '--version'], text=True,
                             capture_output=True, timeout=120)
    output += '\n' + version.stdout + version.stderr
(root / 'evidence').mkdir(exist_ok=True)
(root / 'evidence/package-check.log').write_text(output)
if version is not None and version.returncode:
    raise SystemExit(f'Packaged native-image launcher failed:\n{version.stdout}{version.stderr}')
if result.returncode or 'Weak bridge passed:' not in output:
    raise SystemExit(f'Packaged JDK failed (exit {result.returncode}):\n{output[-6000:]}')
for name in ('libjam_vm.dylib', 'libjam_bridge.dylib', 'libc++.1.dylib',
             'libc++abi.1.dylib', 'libunwind.1.dylib'):
    if str(library / name) not in output:
        raise SystemExit(f'Loader did not report the packaged {name}.')
print('Packaged JDK passed: public weak API and bundled native libraries.')
