#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
vm=${JAM_JAVA:-$root/upstream/jdk25/build/macosx-aarch64-server-fastdebug/jdk/bin/java}
javac=${JAM_JAVAC:-$(dirname "$vm")/javac}
prefix=${JAM_EVIDENCE_PREFIX:-jam}
mkdir -p build-java-tests evidence
python3 tools/build_bridge.py --java-home "$(cd "$(dirname "$javac")/.." && pwd)"
"$javac" -cp build/bridge/jam-vm.jar -d build-java-tests tests/java/*.java
flags=(-Xshare:off -Xms32m -Xmx32m -XX:+UnlockExperimentalVMOptions -XX:+UseJamGC
       -XX:JamWorkers=4 -XX:+VerifyBeforeGC -XX:+VerifyAfterGC -Xlog:gc
       --enable-native-access=ALL-UNNAMED "-Djava.library.path=$root/build/bridge/lib:$root/build-jam"
       -cp "build-java-tests:build/bridge/jam-vm.jar")
for test in HeapSmoke WeakSmoke BoundarySmoke; do
  "$vm" "${flags[@]}" "$test" > "evidence/$prefix-$test.log" 2>&1
  tail -2 "evidence/$prefix-$test.log"
done
for mode in interpreter c1 c2; do
  case "$mode" in
    interpreter) compiler=(-Xint) ;;
    c1) compiler=(-Xbatch -XX:TieredStopAtLevel=1 -XX:+PrintCompilation) ;;
    c2) compiler=(-Xbatch -XX:-TieredCompilation -XX:CompileThreshold=1000 -XX:+PrintCompilation) ;;
  esac
  "$vm" "${flags[@]}" "${compiler[@]}" \
    -XX:+UnlockDiagnosticVMOptions "-XX:LogFile=$root/evidence/$prefix-compiler-$mode.log" \
    -XX:CompileCommand=dontinline,GenerationSmoke::store \
    -XX:CompileCommand=dontinline,GenerationSmoke::arrayStore \
    -XX:CompileCommand=dontinline,GenerationSmoke::unsafeStore \
    -XX:CompileCommand=dontinline,GenerationSmoke::largeStore \
    -XX:CompileCommand=dontinline,GenerationSmoke::copy \
    GenerationSmoke > "evidence/$prefix-GenerationSmoke-$mode.log" 2>&1
  tail -2 "evidence/$prefix-GenerationSmoke-$mode.log"
  "$vm" "${flags[@]}" "${compiler[@]}" -Xms128m -Xmx128m -XX:JamYoungSize=8m \
    '-XX:CompileCommand=dontinline,CompiledBarrierSmoke::*' CompiledBarrierSmoke \
    > "evidence/$prefix-CompiledBarrierSmoke-$mode.log" 2>&1
  tail -2 "evidence/$prefix-CompiledBarrierSmoke-$mode.log"
done
"$vm" "${flags[@]}" -Xms64m -Xmx64m -XX:JamYoungSize=32m \
  -XX:JamPromoteEvery=1000 GenerationCapacitySmoke \
  > "evidence/$prefix-GenerationCapacitySmoke.log" 2>&1
tail -2 "evidence/$prefix-GenerationCapacitySmoke.log"
