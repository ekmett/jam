#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Export the HotSpot adaptation against the pinned source archive."""
import difflib
from pathlib import Path
import tarfile
root = Path(__file__).resolve().parents[1]
def difference(before, after, name):
    return ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                       fromfile='a/' + name, tofile='b/' + name))
# Only tracked source edits, against files read straight from the pinned archive.
with tarfile.open(root / 'upstream/jdk25.tar.gz') as archive:
    members = {m.name.split('/', 1)[1]: m for m in archive.getmembers() if '/' in m.name and m.isfile()}
    paths = [p for directory in ['epsilon', 'jam']
             for p in (root / 'upstream/jdk25/src/hotspot/share/gc' / directory).glob('*') if p.is_file()]
    paths += [root / 'upstream/jdk25' / p for p in [
        'make/autoconf/jvm-features.m4',
        'make/hotspot/lib/JvmFeatures.gmk',
        'src/hotspot/share/utilities/macros.hpp',
        'src/hotspot/share/gc/shared/gc_globals.hpp',
        'src/hotspot/share/gc/shared/collectedHeap.hpp',
        'src/hotspot/share/gc/shared/gcConfig.cpp',
        'src/hotspot/share/gc/shared/barrierSetConfig.hpp',
        'src/hotspot/share/gc/shared/barrierSetConfig.inline.hpp',
        'src/hotspot/share/gc/shared/vmStructs_gc.hpp',
        'src/hotspot/share/gc/shared/gcName.hpp',
        'src/hotspot/share/gc/shared/gcConfiguration.cpp',
        'src/hotspot/share/jvmci/vmStructs_jvmci.cpp',
        'src/hotspot/share/jvmci/jvmciCompilerToVMInit.cpp',
        'src/hotspot/share/jvmci/jvmci_globals.cpp',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/memory/Universe.java',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/gc/shared/CollectedHeapName.java',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/tools/HeapSummary.java',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/HSDB.java',
        'test/lib/jdk/test/whitebox/gc/GC.java',
        'src/hotspot/cpu/aarch64/gc/shared/cardTableBarrierSetAssembler_aarch64.cpp',
        'src/hotspot/cpu/riscv/gc/shared/cardTableBarrierSetAssembler_riscv.cpp',
        'src/hotspot/cpu/arm/gc/shared/cardTableBarrierSetAssembler_arm.cpp',
        'src/hotspot/share/gc/shared/cardTable.cpp',
        'src/hotspot/share/gc/shared/gcVMOperations.cpp',
        'src/hotspot/share/gc/shared/referenceProcessor.cpp',
        'src/hotspot/share/gc/shared/referenceProcessor.hpp',
        'src/hotspot/share/runtime/vmOperation.hpp',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/gc/epsilon/EpsilonHeap.java',
        'src/jdk.hotspot.agent/share/classes/sun/jvm/hotspot/gc/jam/JamHeap.java',
        'src/hotspot/share/include/jvm.h']]
    patch = ''
    for p in sorted(paths):
        name = str(p.relative_to(root / 'upstream/jdk25'))
        before = archive.extractfile(members[name]).read().decode() if name in members else ''
        patch += difference(before, p.read_text(), name)
(root / 'patches/hotspot-jam.patch').write_text(patch)
print('Exported the HotSpot source patch.')
