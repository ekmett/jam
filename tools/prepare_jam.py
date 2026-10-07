#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Apply the hosted-mapping extension to pinned Jam, preserving unrelated edits."""

import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def prepare(check=False):
    source = ROOT / 'upstream/jam'
    pin = json.loads((ROOT / 'config/source-pins.json').read_text())['jam']['commit']
    patch = ROOT / 'patches/jam-host-windows.patch'

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(source), *arguments])

    if git('rev-parse', 'HEAD').decode().strip() != pin:
        raise SystemExit('Preserving a Jam checkout at a different revision.')
    if git('ls-files', '--others', '--exclude-standard'):
        raise SystemExit('Preserving untracked files in the Jam checkout.')
    current = git('diff', '--binary', 'HEAD')
    expected = patch.read_bytes()
    if current != expected:
        if current or check:
            raise SystemExit('Jam sources differ from the hosted-mapping patch; existing changes preserved.')
        subprocess.run(['git', '-C', str(source), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(source), 'apply', '--index', str(patch)], check=True)
        if git('diff', '--binary', 'HEAD') != expected:
            raise SystemExit('Applied Jam patch differs from the published patch.')
    print(f'Jam at {pin} has exactly the hosted-mapping patch.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Check prepared sources without changing them')
    prepare(parser.parse_args().check)
