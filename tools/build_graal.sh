#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME=${JAM_GRAAL_BASE_JDK:-$root/upstream/labsjdk25/build/macosx-aarch64-server-fastdebug/images/graal-builder-jdk}
export MX_CACHE_DIR=${MX_CACHE_DIR:-$root/.toolchains/mx-cache}
mx="$root/upstream/mx-graal25/mx"
python3 "$root/tools/prepare_graal.py" --check
cd "$root/upstream/graal25/vm"
"$mx" --max-cpus "${JAM_JOBS:-3}" --env ce build --build-logs=silent
graal_home=$("$mx" --env ce graalvm-home)
# mx tracks the base module image, but a HotSpot-only rebuild can leave it
# unchanged. Refresh the final JDK and module archive when libjvm changed.
if ! cmp -s "$JAVA_HOME/lib/server/libjvm.dylib" "$graal_home/lib/server/libjvm.dylib"; then
  "$mx" --max-cpus "${JAM_JOBS:-3}" --env ce build \
    --only graalvm-jimage,java.base.jmod_modifier,GRAALVM_COMMUNITY_JAVA25 -f --build-logs=silent
fi
archive=$("$mx" --env ce paths "$("$mx" --env ce graalvm-dist-name)")
# mx performs distribution processing when it creates the archive.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/jam-graal-package.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
tar -xf "$archive" -C "$scratch"
image="$scratch/$(basename "$graal_home")"
if ! cmp -s "$JAVA_HOME/lib/server/libjvm.dylib" "$image/lib/server/libjvm.dylib"; then
  echo "GraalVM archive contains an outdated HotSpot library" >&2
  exit 1
fi
python3 "$root/tools/build_bridge.py" --java-home "$image"
python3 "$root/tools/package_jdk.py" --java-home "$image" \
  --output "${JAM_GRAAL_OUTPUT:-$root/build/graalvm}"
