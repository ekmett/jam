#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Check release qualification and installation identity without a runtime build."""

import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/ci'))
from release_runtime import inventory, require_run, PLATFORMS


class ReleaseTests(unittest.TestCase):
    def test_only_qualified_main_runs(self):
        run = dict(status='completed', conclusion='success', event='push', head_branch='main', head_repository={'full_name': 'ekmett/jam'}, path='.github/workflows/vm.yml')
        jobs = [dict(name=f'{s} ({p})', conclusion='success') for p in PLATFORMS for s in
                ('Build GraalVM', 'HotSpot integration', 'Native and guest bridge', 'GraalVM runtime', 'SubstrateVM')]
        require_run(run, jobs, {'status': 'ahead'})
        for field, value in [('event', 'pull_request_target'), ('head_repository', {'full_name': 'other/fork'}), ('status', 'in_progress'), ('conclusion', 'failure'), ('path', '.github/workflows/ci.yml')]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                require_run({**run, field: value}, jobs, {'status': 'ahead'})
        with self.assertRaises(ValueError):
            require_run(run, jobs[:-1], {'status': 'ahead'})
        require_run({**run, 'event': 'pull_request', 'head_branch': 'merged-branch'}, jobs, {'status': 'ahead'})
        with self.assertRaises(ValueError):
            require_run(run, jobs, {'status': 'diverged'})

    def test_content_identity_and_unsafe_members(self):
        files = {'release': b'JAVA_VERSION="25"\r\n', 'lib/jam/jam-vm.jar': b'api',
                 'lib/jam/runtime-libraries.txt': b'libjam-vm.so\n', 'legal/jam-vm/NOTICE.md': b'notice'}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'runtime.tar.gz'
            def write(extra=None):
                with tarfile.open(path, 'w:gz') as archive:
                    for name, data in reversed(list(files.items())):
                        entry = tarfile.TarInfo('graalvm/' + name); entry.size = len(data)
                        archive.addfile(entry, io.BytesIO(data))
                    link = tarfile.TarInfo('graalvm/lib/api.jar'); link.type = tarfile.SYMTYPE; link.linkname = 'jam/jam-vm.jar'
                    archive.addfile(link)
                    if extra: archive.addfile(extra, io.BytesIO(b''))
            write()
            identity, _, release, _ = inventory(path, 'graalvm')
            lines = {name: hashlib.sha256(data).hexdigest() + '  ' + name + '\n' for name, data in files.items()}
            lines['lib/api.jar'] = 'link  jam/jam-vm.jar  lib/api.jar\n'
            expected = hashlib.sha256(''.join(lines[name] for name in sorted(lines)).encode()).hexdigest()
            self.assertEqual(identity['sha256'], expected)
            self.assertEqual((identity['files'], identity['symlinks']), (4, 1))
            self.assertEqual(release['JAVA_VERSION'], '25')
            for name in ('../escape', '/absolute', 'graalvm/release'):
                write(tarfile.TarInfo(name))
                with self.subTest(name=name), self.assertRaises(ValueError): inventory(path, 'graalvm')
            link = tarfile.TarInfo('graalvm/bad'); link.type = tarfile.SYMTYPE; link.linkname = '../../outside'
            write(link)
            with self.assertRaises(ValueError): inventory(path, 'graalvm')


if __name__ == '__main__':
    unittest.main()
