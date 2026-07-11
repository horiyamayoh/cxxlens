#!/usr/bin/env python3
"""Generate the complete chapter 40 API inventory projection."""

from __future__ import annotations

import argparse
import pathlib

import yaml

from validate_api_contract import render_inventory, validate_document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalog", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    document = yaml.safe_load(args.catalog.read_text(encoding="utf-8"))
    validate_document(document)
    expected = render_inventory(document)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != expected:
            raise SystemExit("generated API inventory is stale")
    else:
        args.output.write_text(expected, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
