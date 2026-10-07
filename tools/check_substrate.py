#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Build and relocate Jam executables, including a runtime-compiled Truffle consumer."""

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
from package_jdk import (check_elf_paths, check_loaded_libraries, load_commands,
                         loader_environment, runtime_libraries, SYSTEM)

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--java-home', type=Path, default=root / 'build/graalvm')
options = parser.parse_args()
home = options.java_home.resolve()
work = root / 'build-substrate-tests'
classes = work / 'classes'
scratch = work / 'tmp'
evidence = root / 'evidence'
for directory in (classes, scratch, evidence):
    directory.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, TMPDIR=str(scratch))
jar = home / 'lib/jam/jam-vm.jar'
image_options = [home / 'bin/native-image', '--gc=jam', '-ETMPDIR',
                 '-J-Djava.io.tmpdir=' + str(scratch),
                 '-J-Xmx' + os.environ.get('JAM_NATIVE_IMAGE_HEAP', '6g'),
                 '--parallelism=' + os.environ.get('JAM_JOBS', '3')]


def run(label, command, timeout=300, expected=None, trace_libraries=False, reject=False):
    runtime_environment = loader_environment(environment) if trace_libraries else environment
    result = subprocess.run(list(map(str, command)), cwd=root, env=runtime_environment,
                            text=True, capture_output=True, timeout=timeout)
    output = result.stdout + result.stderr
    (evidence / f'substrate-{label}.log').write_text(output + f'\nexit={result.returncode}\n')
    if (result.returncode == 0 if reject else result.returncode != 0) or expected is not None and expected not in output:
        raise SystemExit(f'{label} failed (exit {result.returncode}):\n{output[-8000:]}')
    return output


run('javac', [home / 'bin/javac', '--add-modules', 'org.graalvm.nativeimage',
               '-cp', jar, '-d', classes, *sorted((root / 'tests/substrate').glob('*.java')),
               root / 'tests/bridge/WeakBridgeSmoke.java'])
compiler = ['xcrun', 'clang'] if platform.system() == 'Darwin' else shlex.split(os.environ.get('CC', 'cc'))
archiver = ['xcrun', 'ar'] if platform.system() == 'Darwin' else shlex.split(os.environ.get('AR', 'ar'))
run('pin-compile', [*compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-c', root / 'tests/substrate/pin_writer.c', '-o', work / 'pin_writer.o'])
run('pin-archive', [*archiver, 'rcs', work / 'libjam_pin_test.a', work / 'pin_writer.o'])
executable = work / 'substrate-smoke'
run('image-build', [*image_options,
                    '--initialize-at-build-time=IsolateSmoke$EntryPoints',
                    '-Djam.pin.include=' + str(root / 'tests/substrate'),
                    '-Djam.pin.library=' + str(work),
                    '-cp', os.pathsep.join(map(str, (classes, jar))),
                    'SubstrateSmoke', executable], timeout=1200,
    expected='Garbage collector: Jam')

def relocate(executable):
    # The executable and its sibling runtime directory are the deployment unit.
    bundle = executable.with_name(executable.name + '.jam')
    for library in runtime_names:
        if not (bundle / library).is_file():
            raise SystemExit(f'Missing native-image runtime: {library}')
    relocated = work / 'relocated'
    relocated.mkdir(exist_ok=True)
    shutil.copy2(executable, relocated / executable.name)
    shutil.copytree(bundle, relocated / bundle.name, dirs_exist_ok=True)
    result = relocated / executable.name
    for binary in (result, *(relocated / bundle.name / name for name in runtime_names)):
        if platform.system() == 'Linux':
            check_elf_paths(binary, relocated)
            continue
        identity, dependencies, rpaths = load_commands(binary)
        for path in (identity, *dependencies, *rpaths):
            if path and path.startswith('/') and not path.startswith(SYSTEM):
                raise SystemExit(f'{binary.name} retains a build-time load path: {path}')
    return result


runtime_names = runtime_libraries(home / 'lib/jam')


def run_executable(label, executable, arguments, expected):
    output = run(label, [executable, *arguments], expected=expected, trace_libraries=True)
    bundle = executable.with_name(executable.name + '.jam')
    check_loaded_libraries(output, bundle, runtime_names)


executable = relocate(executable)

for mode, expected in (
        ('heap', 'Jam Native Image heap passed'),
        ('weak', 'Weak bridge passed:'),
        ('pin', 'Jam Native Image concurrent pin passed'),
        ('runtime', 'Jam Native Image runtime contracts passed'),
        ('continuations', 'Jam Native Image continuations passed'),
        ('isolates', 'Jam Native Image isolate lifecycle passed'),
        ('capacity', 'Jam Native Image pin capacity passed')):
    run_executable(mode, executable, ['-Xmx128m', '-Xmn32m', mode], expected)
    print(f'Native Image {mode}: passed')

truffle_classes = work / 'truffle-classes'
truffle_classes.mkdir(exist_ok=True)
dependencies = [root / 'upstream/graal25' / suite / 'mxbuild/dists' / (name + '.jar')
                for suite, names in (
                    ('truffle', ('truffle-api', 'truffle-runtime', 'truffle-compiler')),
                    ('sdk', ('polyglot', 'collections', 'jniutils', 'nativebridge', 'nativeimage', 'word')))
                for name in names]
for dependency in dependencies:
    if not dependency.is_file():
        raise SystemExit(f'Missing Truffle build dependency: {dependency}; build GraalVM first.')
classpath = os.pathsep.join(map(str, (truffle_classes, jar, *dependencies)))
run('truffle-javac', [home / 'bin/javac', '-cp', classpath, '-d', truffle_classes,
                      root / 'tests/substrate/truffle/TruffleSmoke.java'])
executable = work / 'truffle-smoke'
run('truffle-image-build', [*image_options, '--macro:truffle-svm',
                           # Truffle's partial evaluator requires initialized guest classes.
                           '--initialize-at-build-time=' + ','.join(
                               path.stem for path in sorted(truffle_classes.glob('TruffleSmoke*.class'))),
                           '-J-Dpolyglot.engine.userResourceCache=' + str(scratch / 'truffle-cache'),
                           '--add-exports=org.graalvm.truffle.runtime/com.oracle.truffle.runtime=ALL-UNNAMED',
                           '--enable-native-access=ALL-UNNAMED', '-cp', classpath,
                           'TruffleSmoke', executable], timeout=1800,
    expected='Garbage collector: Jam')
amd64 = platform.machine().lower() in ('x86_64', 'amd64')
run_executable('truffle', relocate(executable), ['-Xmx512m', '-Xmn64m',
                 '-Dpolyglot.engine.BackgroundCompilation=false',
                 '-Dpolyglot.engine.CompilationFailureAction=Throw',
                 *(['check-masking'] if amd64 else [])],
    'Jam Native Image compiled Truffle consumer passed')
print('Native Image compiled Truffle: passed')
if amd64:
    run('masking-build-rejection', [*image_options, '-R:+MemoryMaskingAndFencing',
                                   '-cp', classpath, 'TruffleSmoke', work / 'unsupported-masking'],
        expected='The option is not supported when using Jam', reject=True)
    run('masking-runtime-rejection', [work / 'relocated/truffle-smoke', '-XX:+MemoryMaskingAndFencing'],
        expected='MemoryMaskingAndFencing is not supported when using Jam', reject=True)
    print('Unsupported memory masking: rejected at build time and runtime')
print('Relocated Jam executables passed.')
