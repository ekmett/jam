#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Reapply each exported patch to pristine inputs and compare every result."""
from pathlib import Path
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
with tarfile.open(root / 'upstream/jdk25.tar.gz') as archive:
    members = {m.name.split('/', 1)[1]: m for m in archive.getmembers()
               if '/' in m.name and m.isfile()}
    for patch_name, source, adapted in [
        ('jam-hosted-heap.patch', root / 'upstream/jam', root / 'upstream/jam-adapted'),
        ('hotspot-jam.patch', None, root / 'upstream/jdk25'),
    ]:
        patch = root / 'patches' / patch_name
        paths = [line[6:] for line in patch.read_text().splitlines() if line.startswith('--- a/')]
        with tempfile.TemporaryDirectory(prefix='jam-patch-') as temporary:
            stage = Path(temporary)
            for name in paths:
                relative = Path(name)
                if relative.is_absolute() or '..' in relative.parts:
                    raise SystemExit('Invalid patch path')
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                if source is not None:
                    target.write_bytes((source / relative).read_bytes())
                elif name in members:
                    target.write_bytes(archive.extractfile(members[name]).read())
            subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=stage, check=True)
            for name in paths:
                if (stage / name).read_bytes() != (adapted / name).read_bytes():
                    raise SystemExit(f'Patch round trip differs: {name}')
        print(f'{patch_name}: {len(paths)} files reproduced exactly')
    epsilon_prefix = 'src/hotspot/share/gc/epsilon/'
    expected = {name for name in members if name.startswith(epsilon_prefix)}
    actual = {str(p.relative_to(root / 'upstream/jdk25'))
              for p in (root / 'upstream/jdk25' / epsilon_prefix).rglob('*') if p.is_file()}
    if actual != expected:
        raise SystemExit('Epsilon source file set differs from the pinned original')
    expected.add('src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/gc/epsilon/EpsilonHeap.java')
    for name in expected:
        if (root / 'upstream/jdk25' / name).read_bytes() != archive.extractfile(members[name]).read():
            raise SystemExit(f'Original Epsilon source changed: {name}')
    print('Epsilon collector and SA heap sources are byte-for-byte upstream.')
for name in ['compact.ccm', 'work.ccm', 'simd.ccm']:
    if (root / 'upstream/jam' / name).read_bytes() != (root / 'upstream/jam-adapted' / name).read_bytes():
        raise SystemExit(f'Upstream implementation changed: {name}')
print('SIMD compactor, work scheduler and SIMD module are byte-for-byte upstream.')
