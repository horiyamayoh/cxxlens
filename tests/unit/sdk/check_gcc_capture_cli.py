#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_capture(cli: pathlib.Path, root: pathlib.Path, compiler: pathlib.Path):
    return subprocess.run(
        [
            str(cli),
            "capture",
            "--project-id",
            "project:cli-capture",
            "--project-root",
            str(root),
            "--compile-commands",
            str(root / "build" / "compile_commands.json"),
            "--compiler",
            str(compiler),
        ],
        cwd=root,
        capture_output=True,
        check=False,
        timeout=45,
    )


def main() -> int:
    require(len(sys.argv) == 4, "expected CLI, probe fixture, and consumer")
    cli = pathlib.Path(sys.argv[1]).resolve()
    compiler = pathlib.Path(sys.argv[2]).resolve()
    consumer = pathlib.Path(sys.argv[3]).resolve()

    with tempfile.TemporaryDirectory(prefix="cxxlens-gcc-capture-cli-") as raw_root:
        root = pathlib.Path(raw_root).resolve()
        (root / "src").mkdir()
        (root / "build").mkdir()
        (root / "src" / "main.cpp").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (root / "build" / "outer.rsp").write_text(
            "@nested.rsp --specs=custom.spec '-DNAME=a b'\n", encoding="utf-8"
        )
        (root / "build" / "nested.rsp").write_text(
            "-I../include\n", encoding="utf-8"
        )
        (root / "build" / "custom.spec").write_text("*link:\n", encoding="utf-8")

        database = [
            {
                "directory": str(root / "build"),
                "file": "../src/main.cpp",
                "arguments": [
                    str(compiler),
                    "@outer.rsp",
                    "-std=gnu++23",
                    "-c",
                    "../src/main.cpp",
                ],
            }
        ]
        database_path = root / "build" / "compile_commands.json"
        database_path.write_text(json.dumps(database), encoding="utf-8")

        first = run_capture(cli, root, compiler)
        second = run_capture(cli, root, compiler)
        require(first.returncode == 0, f"first capture failed: {first.stderr!r}")
        require(second.returncode == 0, f"second capture failed: {second.stderr!r}")
        require(first.stdout and first.stdout == second.stdout, "capture was empty or nondeterministic")
        require(first.stderr == b"" and second.stderr == b"", "successful capture wrote diagnostics")

        bundle_path = root / "capture.cxxlens"
        bundle_path.write_bytes(first.stdout)
        admitted = subprocess.run(
            [str(consumer), str(bundle_path)],
            capture_output=True,
            check=False,
            timeout=15,
        )
        require(admitted.returncode == 0, f"SDK rejected CLI bundle: {admitted.stderr!r}")

        database[0]["arguments"][0] = "g++"
        database_path.write_text(json.dumps(database), encoding="utf-8")
        unbound = run_capture(cli, root, compiler)
        require(unbound.returncode == 2 and unbound.stdout == b"", "unbound compiler was accepted")
        require(
            b"absolute-compiler-path-required" in unbound.stderr,
            "unbound compiler did not return the typed failure",
        )

        invalid = subprocess.run(
            [str(cli), "capture", "--project-id", "project:cli-capture"],
            capture_output=True,
            check=False,
            timeout=15,
        )
        require(invalid.returncode == 2 and invalid.stdout == b"", "incomplete CLI input was accepted")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"gcc capture CLI test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
