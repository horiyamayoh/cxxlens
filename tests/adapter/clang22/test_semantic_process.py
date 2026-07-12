#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys


binary = pathlib.Path(sys.argv[1])
golden = pathlib.Path(sys.argv[2])

outputs = [
    subprocess.run([binary, "--emit"], check=True, text=True, capture_output=True).stdout
    for _ in range(3)
]
if outputs[0] != outputs[1] or outputs[0] != outputs[2]:
    raise SystemExit("semantic extractor output changed across process runs")
expected = golden.read_text(encoding="utf-8")
if outputs[0] != expected:
    raise SystemExit("semantic extractor output does not match golden")

required = [
    "symbol.qualified_name=left::same",
    "symbol.qualified_name=right::same",
    "type.kind=pointer",
    "type.kind=lvalue_reference",
    "type.kind=function",
    "call.kind=virtual_member",
    "call.kind=function_pointer",
    "call.unresolved_reason=dependent-call-target",
    "inheritance.direct=true",
    "override.direct=true",
]
for marker in required:
    if marker not in outputs[0]:
        raise SystemExit(f"semantic golden is missing {marker}")
