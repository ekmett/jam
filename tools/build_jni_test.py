#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

"""Build the same JNI weak-global fixture for HotSpot and Native Image."""

import os
from pathlib import Path
import platform
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(java_home, output):
    output.mkdir(parents=True, exist_ok=True)
    system = platform.system()
    windows = system == 'Windows'
    include = {'Darwin': 'darwin', 'Linux': 'linux', 'Windows': 'win32'}[system]
    name = {'Darwin': 'libjam_weak_test.dylib', 'Linux': 'libjam_weak_test.so',
            'Windows': 'jam_weak_test.dll'}[system]
    library = output / name
    source = ROOT / 'tests/weak_jni.c'
    compiler = shlex.split(os.environ.get('CC', 'clang-cl' if windows else 'cc'))
    if windows:
        flags = ['/nologo', '/std:c11', '/O2', '/MD', '/W4', '/WX', '/LD',
                 '/I' + str(java_home / 'include'), '/I' + str(java_home / 'include' / include),
                 str(source), '/Fe' + str(library), '/link',
                 '/IMPLIB:' + str(output / 'jam_weak_test.lib')]
    else:
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fvisibility=hidden',
                 *(['-dynamiclib'] if system == 'Darwin' else ['-shared', '-fPIC']),
                 '-I' + str(java_home / 'include'), '-I' + str(java_home / 'include' / include),
                 str(source), '-o', str(library)]
    subprocess.run([*compiler, *flags], cwd=output, check=True)
    return library
