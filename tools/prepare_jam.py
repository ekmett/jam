#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Apply the hosted-heap patch to a fresh copy; never edit the pinned jam checkout."""
import json
from pathlib import Path
import shutil
import subprocess
root = Path(__file__).resolve().parents[1]
source, destination = root / 'upstream/jam', root / 'upstream/jam-adapted'
pin = json.loads((root / 'config/source-pins.json').read_text())['jam']['commit']
head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True)
if head != pin or dirty:
    raise SystemExit('Original jam must be clean and match the recorded pin.')
if destination.exists():
    raise SystemExit('Preserving existing upstream/jam-adapted; choose a fresh workspace to reproduce.')
shutil.copytree(source, destination, ignore=shutil.ignore_patterns('.git'))
subprocess.run(['patch', '-p1', '-i', str(root / 'patches/jam-hosted-heap.patch')], cwd=destination, check=True)
print(destination)
