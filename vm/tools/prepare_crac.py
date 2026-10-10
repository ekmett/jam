#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Prepare an experimental CRaC port on Jam's pinned LabsJDK 25 in a fresh directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def checked(path, expected):
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Source hash mismatch: {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--labsjdk-archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='Fresh source directory; existing directories are preserved')
    parser.add_argument('--crac-patch', type=Path,
                        help='Previously downloaded, hash-checked upstream delta')
    args = parser.parse_args()
    pin = json.loads((ROOT / 'config/crac-pins.json').read_text())
    labs = json.loads((ROOT / 'config/source-pins.json').read_text())['labsjdk25']
    checked(args.labsjdk_archive, labs['archive_sha256'])
    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f'Preserving existing source directory: {output}')
    if args.crac_patch:
        checked(args.crac_patch, pin['patch_sha256'])
        delta = args.crac_patch.read_bytes()
    else:
        url = f"https://api.github.com/repos/{pin['repo']}/compare/{pin['base']}...{pin['commit']}"
        request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github.diff'})
        with urllib.request.urlopen(request, timeout=120) as response:
            delta = response.read()
        if hashlib.sha256(delta).hexdigest() != pin['patch_sha256']:
            raise SystemExit('CRaC upstream delta hash mismatch.')
    output.mkdir(parents=True)
    # Keep the exact inputs and port provenance alongside the prepared source.
    evidence = output / '.jam-crac'
    evidence.mkdir()
    (evidence / 'upstream.patch').write_bytes(delta)
    (evidence / 'pins.json').write_text(json.dumps({'labsjdk': labs, 'crac': pin}, indent=2) + '\n')
    subprocess.run(['tar', '-xzf', str(args.labsjdk_archive.resolve()),
                    '-C', str(output), '--strip-components=1'], check=True)
    patch = os.environ.get('JAM_PATCH', 'patch')

    def apply(path):
        subprocess.run([patch, '--batch', '--fuzz=0', '-p1', '-i', str(path.resolve())],
                       cwd=output, check=True)

    apply(ROOT / 'patches/hotspot-jam.patch')
    apply(ROOT / 'patches/labsjdk-compat.patch')
    for path, hashes in pin['compat_files'].items():
        checked(output / path, hashes['before_sha256'])
    sections = re.split(r'(?=^diff --git )', delta.decode(), flags=re.MULTILINE)
    selected = []
    for section in sections:
        if not section:
            continue
        path = section.splitlines()[0].split(' b/', 1)[1]
        if path in pin['compat_files'] or any(
                path.startswith(item) if item.endswith('/') else path == item
                for item in pin['excluded']):
            continue
        selected.append(section)
    runtime_patch = evidence / 'runtime.patch'
    runtime_patch.write_text(''.join(selected))
    apply(runtime_patch)
    apply(ROOT / 'patches/labsjdk-crac-compat.patch')
    for path, hashes in pin['compat_files'].items():
        checked(output / path, hashes['after_sha256'])
    print(f'Prepared experimental Jam CRaC sources: {output}')


if __name__ == '__main__':
    main()
