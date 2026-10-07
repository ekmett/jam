#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Apply the Jam compiler and SubstrateVM patch, preserving other edits."""

import argparse
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true', help='Verify prepared sources without changing them')
options = parser.parse_args()
source = root / 'upstream/graal25'
pin = json.loads((root / 'config/source-pins.json').read_text())['graal']['commit']
patch = root / 'patches/graal-jam.patch'


def git(*arguments):
    return subprocess.check_output(['git', '-C', str(source), *arguments])


if git('rev-parse', 'HEAD').decode().strip() != pin:
    raise SystemExit('Preserving a Graal checkout at a different revision.')
if git('ls-files', '--others', '--exclude-standard'):
    raise SystemExit('Preserving untracked files in the Graal checkout.')
current = git('diff', '--binary', 'HEAD')
expected = patch.read_bytes()
if current == expected:
    print(f'Graal at {pin} has exactly the Jam compiler and SubstrateVM patch.')
elif current or options.check:
    raise SystemExit('Graal sources do not match the Jam patch; existing changes preserved.')
else:
    subprocess.run(['git', '-C', str(source), 'apply', '--check', str(patch)], check=True)
    subprocess.run(['git', '-C', str(source), 'apply', '--index', str(patch)], check=True)
    if git('diff', '--binary', 'HEAD') != expected:
        raise SystemExit('Applied Graal patch differs from the published patch.')
    print(f'Prepared Graal at {pin}.')
