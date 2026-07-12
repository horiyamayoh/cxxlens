#!/usr/bin/env python3
"""Extract Doxygen C++ examples from public headers and syntax-check them."""

from __future__ import annotations

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile


BLOCK = re.compile(r"@code\{\.cpp\}(.*?)@endcode", re.DOTALL)


def clean_comment(block: str) -> str:
    lines = []
    for line in block.splitlines():
        lines.append(re.sub(r"^\s*\* ?", "", line))
    return "\n".join(lines).strip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--include", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("headers", type=pathlib.Path)
    args = parser.parse_args()

    examples: list[tuple[pathlib.Path, str]] = []
    for header in sorted(args.headers.rglob("*.hpp")):
        text = header.read_text(encoding="utf-8")
        examples.extend((header, clean_comment(match)) for match in BLOCK.findall(text))
    if not examples:
        print("no Doxygen C++ examples found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="cxxlens-doc-examples-") as temporary:
        work: list[tuple[pathlib.Path, pathlib.Path, str]] = []
        for index, (header, example) in enumerate(examples):
            source = pathlib.Path(temporary) / f"example_{index}.cpp"
            source.write_text(example, encoding="utf-8")
            work.append((source, header, example))

        def check(
            item: tuple[pathlib.Path, pathlib.Path, str],
        ) -> subprocess.CompletedProcess[str]:
            source, _, _ = item
            command = [
                args.compiler,
                "-std=c++23",
                "-fsyntax-only",
                f"-I{args.include}",
                str(source),
            ]
            return subprocess.run(command, check=False, text=True, capture_output=True)

        if args.jobs < 1:
            parser.error("--jobs must be positive")
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
            results = executor.map(check, work)
            for (_, header, example), result in zip(work, results, strict=True):
                if result.returncode != 0:
                    print(
                        f"example from {header} failed:\n{example}\n{result.stderr}",
                        file=sys.stderr,
                    )
                    return result.returncode
    print(f"syntax-checked {len(examples)} Doxygen examples")
    return 0


if __name__ == "__main__":
    sys.exit(main())
