# SPDX-FileCopyrightText: 2026 Edward Kmett
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

PYTHON ?= python3
PANDOC ?= pandoc

.PHONY: docs docs-check
docs:
	PANDOC="$(PANDOC)" $(PYTHON) tools/build_docs.py
	$(PYTHON) tools/check_docs.py

docs-check:
	$(PYTHON) tools/check_docs.py
