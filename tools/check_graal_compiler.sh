#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME=${JAM_GRAAL_BASE_JDK:-$root/upstream/labsjdk25/build/macosx-aarch64-server-fastdebug/images/graal-builder-jdk}
export MX_CACHE_DIR=${MX_CACHE_DIR:-$root/.toolchains/mx-cache}
mx="$root/upstream/mx-graal25/mx"
python3 "$root/tools/prepare_graal.py" --check
cd "$root/upstream/graal25/compiler"
"$mx" --max-cpus "${JAM_JOBS:-3}" build --build-logs=silent
"$mx" unittest -XX:+UnlockExperimentalVMOptions -XX:+UseJamGC -Xshare:off \
  -Xms128m -Xmx128m -XX:+VerifyBeforeGC -XX:+VerifyAfterGC \
  -XX:+TraceDerivedPointers -Xlog:gc=debug \
  GraalHotSpotVMConfigAccessTest WriteBarrierAdditionTest DeferredBarrierAdditionTest \
  DerivedOopTest PointerTrackingTest
