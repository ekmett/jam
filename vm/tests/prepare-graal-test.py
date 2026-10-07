#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Exercise source-tree verification with a differently formatted local patch."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='jam-prepare-test-') as temporary:
    root = Path(temporary)
    for name in ('tools', 'config', 'patches', 'upstream/graal25'):
        (root / name).mkdir(parents=True)
    script = root / 'tools/prepare_graal.py'
    shutil.copy2(Path(__file__).resolve().parents[1] / 'tools/prepare_graal.py', script)
    source = root / 'upstream/graal25'

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(source), *arguments])

    git('init', '--quiet')
    git('config', 'user.name', 'Jam test')
    git('config', 'user.email', 'test@example.invalid')
    (source / 'input').write_text('before\n')
    git('add', 'input')
    git('commit', '--quiet', '-m', 'fixture')
    revision = git('rev-parse', 'HEAD').decode().strip()
    (root / 'config/source-pins.json').write_text(json.dumps({'graal': {'commit': revision}}))
    (source / 'input').write_text('after\n')
    # Full hashes differ from git diff's default serialization.
    (root / 'patches/graal-jam.patch').write_bytes(git('diff', '--full-index'))
    git('restore', 'input')
    subprocess.run([sys.executable, str(script)], check=True)
    subprocess.run([sys.executable, str(script), '--check'], check=True)
    assert (source / 'input').read_text() == 'after\n'
    (source / 'input').write_text('local edit\n')
    result = subprocess.run([sys.executable, str(script)], capture_output=True)
    assert result.returncode != 0
    assert (source / 'input').read_text() == 'local edit\n'
    print('Patch applied, repeated check passed, unrelated local edit preserved.')
