#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Fetch only the two native dependencies named by the source manifest."""

import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
pins = json.loads((root / "config/source-pins.json").read_text())
(root / "upstream").mkdir(exist_ok=True)
for name in ("jam", "native"):
    pin = pins[name]
    destination = root / "upstream" / name
    if not destination.exists():
        subprocess.run(["git", "init", "--quiet", str(destination)], check=True)
        subprocess.run(["git", "-C", str(destination), "remote", "add", "origin",
                        f"https://github.com/{pin['repo']}.git"], check=True)
        subprocess.run(["git", "-C", str(destination), "fetch", "--depth=1", "origin",
                        pin["commit"]], check=True)
        subprocess.run(["git", "-C", str(destination), "checkout", "--quiet", "--detach",
                        pin["commit"]], check=True)
    head = subprocess.check_output(["git", "-C", str(destination), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(destination), "status", "--porcelain"], text=True)
    if head != pin["commit"] or name != 'jam' and dirty:
        raise SystemExit(f"Preserving changed upstream/{name}; expected the clean manifest revision.")
    print(f"Verified {name} at {head}")
subprocess.run([sys.executable, str(root / 'tools/prepare_jam.py')], check=True)
