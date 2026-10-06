#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${JAM_BOOT_JDK:?Set JAM_BOOT_JDK to a JDK 24 or 25 installation}"
case "${1:-}" in
  '') source_dir="$root/upstream/jdk25"; target=images; version_flags=() ;;
  --graal)
    source_dir="$root/upstream/labsjdk25"
    target=graal-builder-image
    version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["labsjdk25"]["version"])' "$root/config/source-pins.json")
    version_flags=("--with-version-string=$version" --with-build-user=jam-vm --with-vendor-name='Jam VM') ;;
  *) echo 'Usage: build_hotspot.sh [--graal]' >&2; exit 1 ;;
esac
cd "$source_dir"
export AUTOCONF=${JAM_AUTOCONF:-$root/.toolchains/autoconf-install/bin/autoconf}
export M4=${JAM_M4:-$root/.toolchains/gnu/bin/m4}
make_bin=${JAM_MAKE:-$root/.toolchains/gnu/bin/make}
bash configure "--with-boot-jdk=$JAM_BOOT_JDK" --with-boot-jdk-jvmargs=-Xshare:off \
  "${version_flags[@]}" \
  --with-debug-level=fastdebug --with-jvm-variants=server --with-jvm-features=jamgc,epsilongc,serialgc \
  --disable-warnings-as-errors "--with-extra-cxxflags=-I$root/adapter" \
  "--with-extra-ldflags=-L$root/build-jam -ljam_vm -Wl,-rpath,$root/build-jam"
"$make_bin" "JOBS=${JAM_JOBS:-8}" "$target"
