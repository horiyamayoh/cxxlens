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


def run_wrapper(
    cli: pathlib.Path,
    root: pathlib.Path,
    compiler: pathlib.Path,
    capture_directory: pathlib.Path,
    *extra_arguments: str,
):
    return subprocess.run(
        [
            str(cli),
            "capture",
            "--project-id",
            "project:cli-capture",
            "--project-root",
            str(root),
            "--capture-directory",
            str(capture_directory),
            "--compiler",
            str(compiler),
            "--",
            str(compiler),
            "-I../include",
            "-std=gnu++23",
            "-c",
            "../src/main.cpp",
            "-o",
            "main.o",
            *extra_arguments,
        ],
        cwd=root / "build",
        capture_output=True,
        check=False,
        timeout=45,
    )


def main() -> int:
    require(
        len(sys.argv) in (4, 5),
        "expected CLI, compiler, consumer, and optional --require-production-build",
    )
    require(
        len(sys.argv) == 4 or sys.argv[4] == "--require-production-build",
        "unknown GCC capture CLI test option",
    )
    cli = pathlib.Path(sys.argv[1]).resolve()
    compiler = pathlib.Path(sys.argv[2]).resolve()
    consumer = pathlib.Path(sys.argv[3]).resolve()
    require_production_build = len(sys.argv) == 5

    with tempfile.TemporaryDirectory(prefix="cxxlens-gcc-capture-cli-") as raw_root:
        root = pathlib.Path(raw_root).resolve()
        (root / "src").mkdir()
        (root / "build").mkdir()
        (root / "include").mkdir()
        (root / "include" / "fixture.hpp").write_text(
            "#pragma once\ninline constexpr int fixture_value = 0;\n", encoding="utf-8"
        )
        (root / "src" / "main.cpp").write_text(
            '#include "fixture.hpp"\nint main() { return fixture_value; }\n',
            encoding="utf-8",
        )
        (root / "build" / "outer.rsp").write_text(
            "@nested.rsp --specs=custom.spec '-DNAME=a b'\n", encoding="utf-8"
        )
        (root / "build" / "nested.rsp").write_text(
            "-I../include\n", encoding="utf-8"
        )
        (root / "build" / "custom.spec").write_text(
            "%rename link old_link\n\n*link:\n%(old_link)\n", encoding="utf-8"
        )

        database = [
            {
                "directory": str(root / "build"),
                "file": "../src/main.cpp",
                "arguments": [
                    str(compiler),
                    "@outer.rsp",
                    "-std=gnu++23",
                    "-MMD",
                    "-MF",
                    "main.d",
                    "-c",
                    "../src/main.cpp",
                ],
            }
        ]
        database_path = root / "build" / "compile_commands.json"
        database_path.write_text(json.dumps(database), encoding="utf-8")

        if require_production_build:
            production = subprocess.run(
                [*database[0]["arguments"], "-o", "main.o"],
                cwd=root / "build",
                capture_output=True,
                check=False,
                timeout=45,
            )
            require(
                production.returncode == 0 and (root / "build" / "main.o").is_file(),
                f"production GCC build failed: {production.stderr!r}",
            )

        first = run_capture(cli, root, compiler)
        second = run_capture(cli, root, compiler)
        require(first.returncode == 0, f"first capture failed: {first.stderr!r}")
        require(second.returncode == 0, f"second capture failed: {second.stderr!r}")
        require(first.stdout and first.stdout == second.stdout, "capture was empty or nondeterministic")
        require(first.stderr == b"" and second.stderr == b"", "successful capture wrote diagnostics")

        bundle_path = root / "capture.cxxlens"
        bundle_path.write_bytes(first.stdout)
        admitted = subprocess.run(
            [
                str(consumer),
                str(bundle_path),
                *(["--expect-dependency-output"] if require_production_build else []),
            ],
            capture_output=True,
            check=False,
            timeout=15,
        )
        require(admitted.returncode == 0, f"SDK rejected CLI bundle: {admitted.stderr!r}")

        wrapper_directory = root / "wrapper-captures"
        wrapper_directory.mkdir()
        wrapped_first = run_wrapper(cli, root, compiler, wrapper_directory)
        wrapped_second = run_wrapper(cli, root, compiler, wrapper_directory)
        require(wrapped_first.returncode == 0, f"first wrapper failed: {wrapped_first.stderr!r}")
        require(wrapped_second.returncode == 0, f"second wrapper failed: {wrapped_second.stderr!r}")
        require(
            wrapped_first.stdout == b""
            and wrapped_first.stderr == b""
            and wrapped_second.stdout == b""
            and wrapped_second.stderr == b"",
            "wrapper polluted streams",
        )
        bundles = list(wrapper_directory.glob("capture-*.cxxlens"))
        require(len(bundles) == 1, "wrapper did not publish one deterministic bundle")
        require(
            not list(wrapper_directory.glob(".cxxlens-capture-*")),
            "wrapper left a private workspace reachable",
        )
        require((root / "build" / "main.o").is_file(), "wrapper did not preserve compiler output")
        wrapped_admitted = subprocess.run(
            [str(consumer), str(bundles[0]), "--expect-wrapper"],
            capture_output=True,
            check=False,
            timeout=15,
        )
        require(
            wrapped_admitted.returncode == 0,
            f"SDK rejected wrapper bundle: {wrapped_admitted.stderr!r}",
        )

        failure_directory = root / "failed-captures"
        failure_directory.mkdir()
        if not require_production_build:
            failed = run_wrapper(cli, root, compiler, failure_directory, "-DFAIL_COMPILE")
            require(failed.returncode == 23, "wrapper did not preserve compiler failure")
            require(not list(failure_directory.iterdir()), "failed compiler published a bundle")

        rejected_response = subprocess.run(
            [
                str(cli),
                "capture",
                "--project-id",
                "project:cli-capture",
                "--project-root",
                str(root),
                "--capture-directory",
                str(failure_directory),
                "--compiler",
                str(compiler),
                "--",
                str(compiler),
                "@outer.rsp",
                "-c",
                "../src/main.cpp",
            ],
            cwd=root / "build",
            capture_output=True,
            check=False,
            timeout=15,
        )
        require(
            rejected_response.returncode == 2
            and b"response-not-expanded" in rejected_response.stderr,
            "wrapper accepted an ambient response file",
        )

        missing_capture_directory = run_wrapper(
            cli, root, compiler, root / "missing-capture-directory"
        )
        require(
            missing_capture_directory.returncode == 2
            and b"capture.directory" in missing_capture_directory.stderr,
            "missing capture directory was not rejected before compilation",
        )

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
