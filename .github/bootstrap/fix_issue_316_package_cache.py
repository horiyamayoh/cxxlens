#!/usr/bin/env python3
"""Normalize the generated package-cache checker marker."""

from __future__ import annotations

import pathlib


path = pathlib.Path("tools/quality/check_ci_supply_chain.py")
text = path.read_text(encoding="utf-8")
old = '        "["apt-get", "download"",\n'
new = '        \'["apt-get", "download"\',\n'
if text.count(old) != 1:
    raise SystemExit(f"generated checker marker count differs: {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
