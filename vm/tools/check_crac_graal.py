#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Check released libgraal against a CRaC candidate using simulation callbacks only."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--native-tests', type=Path, required=True,
                        help='Directory containing the existing collector/weak test JNI libraries')
    parser.add_argument('--output', type=Path, required=True, help='Fresh evidence directory')
    args = parser.parse_args()
    home = args.java_home.resolve()
    output = args.output.resolve()
    jar = home / 'lib/jam/jam-vm.jar'
    for required in [home / 'bin/java', home / 'bin/javac',
                     home / 'lib/libjvmcicompiler.so', jar]:
        if not required.is_file():
            raise SystemExit(f'Missing candidate artifact: {required}')
    if not args.native_tests.is_dir():
        raise SystemExit(f'Missing test JNI directory: {args.native_tests}')
    if output.exists():
        raise SystemExit(f'Preserving existing evidence: {output}')
    output.mkdir(parents=True)
    classes = output / 'classes'
    classes.mkdir()
    exports = ['--add-modules=jdk.crac,jdk.internal.vm.ci']
    exports += [f'--add-exports=jdk.internal.vm.ci/jdk.vm.ci.{package}=ALL-UNNAMED'
                for package in ['hotspot', 'runtime', 'meta']]
    sources = [ROOT / 't/java' / (name + '.java') for name in
               ['CollectorIdentitySmoke', 'JamWeak', 'WeakSmoke', 'HeapSmoke']]
    sources += [ROOT / 't/crac' / (name + '.java') for name in ['CracJamSmoke', 'CracGraalSmoke']]
    subprocess.run([str(home / 'bin/javac'), *exports, '-cp', str(jar),
                    '-d', str(classes), *map(str, sources)], check=True)
    flags = ['--enable-native-access=ALL-UNNAMED', '-Xshare:off', '-Xms128m', '-Xmx128m',
             '-XX:+UnlockExperimentalVMOptions', '-XX:+UnlockDiagnosticVMOptions', '-XX:+UseJamGC',
             '-XX:+EnableJVMCI', '-XX:+UseJVMCICompiler', '-XX:+UseJVMCINativeLibrary',
             '-Xbatch', '-XX:-TieredCompilation', '-XX:CompileThreshold=1000', '-XX:JamWorkers=4',
             '-XX:+LogCompilation', '-XX:LogFile=' + str(output / 'compilation.xml'),
             '-Djam.crac.compilationLog=' + str(output / 'compilation.xml'),
             '-XX:CompileCommand=compileonly,CracGraalSmoke::before',
             '-XX:CompileCommand=compileonly,CracGraalSmoke::after',
             '-Djdk.graal.CompilationFailureAction=ExitVM', '-Djdk.graal.ShowConfiguration=info',
             '-Djava.library.path=' + os.pathsep.join([str(home / 'lib/jam'), str(args.native_tests.resolve())]),
             '-cp', os.pathsep.join([str(classes), str(jar)]),
             # Libgraal keeps its image backing file open for future isolates.
             # Preserve it for the engine; do not close it or allow unrelated files.
             '-XX:CRaCAllowedOpenFilePrefixes=' + str(home / 'lib/libjvmcicompiler.so'),
             '-XX:CRaCEngine=simengine', '-XX:CRaCCheckpointTo=' + str(output / 'simulation')]
    with (output / 'probe.log').open('w') as log:
        result = subprocess.run([str(home / 'bin/java'), *exports, *flags, 'CracGraalSmoke'],
                                cwd=output, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    text = (output / 'probe.log').read_text()
    print(text)
    expected = 'Jam CRaC Graal compiled execution passed before/after callback'
    if result.returncode or expected not in text:
        raise SystemExit(f'Graal simulation probe failed (exit {result.returncode}); evidence: {output}')
    for method in ['before', 'after']:
        path = output / ('compilation-before.xml' if method == 'before' else 'compilation.xml')
        # The pre-checkpoint snapshot is intentionally taken while the log is open.
        records = [ET.fromstring(record) for record in re.findall(r'<nmethod\b[^>]*?/>', path.read_text())]
        entries = [entry for entry in records
                   if entry.get('compiler', '').lower() == 'jvmci'
                   and entry.get('compile_kind') != 'osr'
                   and entry.get('method', '').startswith('CracGraalSmoke ' + method + ' ')]
        if not entries:
            raise SystemExit(f'No installed Graal code for {method}; evidence: {output}')
        print(f'{method}: installed JVMCI code verified')
    print('Simulation only: actual process restore remains unqualified.')


if __name__ == '__main__':
    main()
