#!/usr/bin/env python3

import json
import os
import pathlib
import subprocess
import sys
import tempfile


SHORT_PROCESS_TIMEOUT = 60 if os.getenv("ASAN_OPTIONS") or os.getenv("TSAN_OPTIONS") else 15


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


def run_response_wrapper(
    cli: pathlib.Path,
    root: pathlib.Path,
    compiler: pathlib.Path,
    capture_directory: pathlib.Path,
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
            "@wrapper-outer.rsp",
        ],
        cwd=root / "build",
        capture_output=True,
        check=False,
        timeout=45,
    )


def run_materializer(
    materializer: pathlib.Path,
    bundle: pathlib.Path,
    worker: pathlib.Path,
    expected_units: int,
) -> None:
    analyzed = subprocess.run(
        [str(materializer), str(bundle), str(worker), str(expected_units)],
        capture_output=True,
        check=False,
        timeout=60,
    )
    require(
        analyzed.returncode == 0,
        "GCC capture/replay/materialize/query failed "
        f"with exit {analyzed.returncode}: {analyzed.stderr!r}",
    )


def main() -> int:
    require(
        len(sys.argv) in (4, 5, 9),
        "expected CLI, compiler, consumer, optional production build, and optional materializer",
    )
    require(
        len(sys.argv) == 4
        or sys.argv[4] == "--require-production-build",
        "unknown GCC capture CLI test option",
    )
    require(
        len(sys.argv) != 9
        or (sys.argv[5] == "--materializer" and sys.argv[7] == "--worker"),
        "expected --materializer EXECUTABLE --worker EXECUTABLE",
    )
    cli = pathlib.Path(sys.argv[1]).resolve()
    compiler = pathlib.Path(sys.argv[2]).resolve()
    consumer = pathlib.Path(sys.argv[3]).resolve()
    require_production_build = len(sys.argv) >= 5
    materializer = pathlib.Path(sys.argv[6]).resolve() if len(sys.argv) == 9 else None
    worker = pathlib.Path(sys.argv[8]).resolve() if len(sys.argv) == 9 else None

    with tempfile.TemporaryDirectory(prefix="cxxlens-gcc-capture-cli-") as raw_root:
        root = pathlib.Path(raw_root).resolve()
        (root / "src").mkdir()
        (root / "build").mkdir()
        (root / "include").mkdir()
        (root / "include" / "fixture.hpp").write_text(
            "#pragma once\ninline constexpr int fixture_value = 0;\nint model_value();\n",
            encoding="utf-8",
        )
        (root / "src" / "main.cpp").write_text(
            '#include "fixture.hpp"\nint main() { return fixture_value + model_value(); }\n',
            encoding="utf-8",
        )
        (root / "src" / "model.cpp").write_text(
            '#include "fixture.hpp"\nint model_value() { return 0; }\n',
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
        (root / "build" / "included.spec").write_text(
            "%include custom.spec\n", encoding="utf-8"
        )
        (root / "build" / "wrapper-outer.rsp").write_text(
            "@wrapper-nested.rsp -std=gnu++23 -c ../src/main.cpp -o response.o\n",
            encoding="utf-8",
        )
        (root / "build" / "wrapper-nested.rsp").write_text(
            "-I../include\n", encoding="utf-8"
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
                    "-o",
                    "main.o",
                ],
            },
            {
                "directory": str(root / "build"),
                "file": "../src/model.cpp",
                "arguments": [
                    str(compiler),
                    "-I../include",
                    "-std=gnu++23",
                    "-MMD",
                    "-MF",
                    "model.d",
                    "-c",
                    "../src/model.cpp",
                    "-o",
                    "model.o",
                ],
            },
        ]
        database_path = root / "build" / "compile_commands.json"
        database_path.write_text(json.dumps(database), encoding="utf-8")

        if require_production_build:
            for compile_unit in database:
                production = subprocess.run(
                    compile_unit["arguments"],
                    cwd=root / "build",
                    capture_output=True,
                    check=False,
                    timeout=45,
                )
                require(
                    production.returncode == 0,
                    f"production GCC build failed: {production.stderr!r}",
                )
            require(
                (root / "build" / "main.o").is_file()
                and (root / "build" / "model.o").is_file(),
                "production GCC build did not emit both corpus objects",
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
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(admitted.returncode == 0, f"SDK rejected CLI bundle: {admitted.stderr!r}")
        if materializer is not None and worker is not None:
            run_materializer(materializer, bundle_path, worker, 2)

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
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(
            wrapped_admitted.returncode == 0,
            f"SDK rejected wrapper bundle: {wrapped_admitted.stderr!r}",
        )
        if materializer is not None and worker is not None:
            run_materializer(materializer, bundles[0], worker, 1)

        response_directory = root / "response-captures"
        response_directory.mkdir()
        response_first = run_response_wrapper(cli, root, compiler, response_directory)
        response_second = run_response_wrapper(cli, root, compiler, response_directory)
        require(response_first.returncode == 0, f"response wrapper failed: {response_first.stderr!r}")
        require(response_second.returncode == 0, f"repeated response wrapper failed: {response_second.stderr!r}")
        require(
            response_first.stdout == b""
            and response_first.stderr == b""
            and response_second.stdout == b""
            and response_second.stderr == b"",
            "response wrapper polluted compiler streams",
        )
        response_bundles = list(response_directory.glob("capture-*.cxxlens"))
        require(len(response_bundles) == 1, "response capture was empty or nondeterministic")
        require((root / "build" / "response.o").is_file(), "expanded response argv was not executed")
        response_admitted = subprocess.run(
            [str(consumer), str(response_bundles[0]), "--expect-wrapper"],
            capture_output=True,
            check=False,
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(response_admitted.returncode == 0, "SDK rejected response wrapper bundle")
        if materializer is not None and worker is not None:
            run_materializer(materializer, response_bundles[0], worker, 1)

        failure_directory = root / "failed-captures"
        failure_directory.mkdir()
        if not require_production_build:
            failed = run_wrapper(cli, root, compiler, failure_directory, "-DFAIL_COMPILE")
            require(failed.returncode == 23, "wrapper did not preserve compiler failure")
            require(not list(failure_directory.iterdir()), "failed compiler published a bundle")

        specs_directory = root / "specs-captures"
        specs_directory.mkdir()
        staged_specs = subprocess.run(
            [
                str(cli),
                "capture",
                "--project-id",
                "project:cli-capture",
                "--project-root",
                str(root),
                "--capture-directory",
                str(specs_directory),
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
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        repeated_specs = subprocess.run(
            staged_specs.args,
            cwd=root / "build",
            capture_output=True,
            check=False,
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(
            staged_specs.returncode == 0
            and staged_specs.stdout == b""
            and staged_specs.stderr == b""
            and repeated_specs.returncode == 0
            and repeated_specs.stdout == b""
            and repeated_specs.stderr == b"",
            f"wrapper did not execute a staged GCC specs file: {staged_specs.stderr!r}",
        )
        specs_bundles = list(specs_directory.glob("capture-*.cxxlens"))
        require(len(specs_bundles) == 1, "staged GCC specs did not publish one bundle")
        specs_admitted = subprocess.run(
            [str(consumer), str(specs_bundles[0]), "--expect-wrapper"],
            capture_output=True,
            check=False,
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(specs_admitted.returncode == 0, "SDK rejected staged GCC specs bundle")
        require(
            not list(specs_directory.glob(".cxxlens-capture-*")),
            "wrapper left a staged GCC specs copy reachable",
        )

        rejected_include = subprocess.run(
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
                "--specs=included.spec",
                "-c",
                "../src/main.cpp",
            ],
            cwd=root / "build",
            capture_output=True,
            check=False,
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(
            rejected_include.returncode == 2
            and b"include-staging-required" in rejected_include.stderr,
            "wrapper executed an unbound GCC specs include",
        )

        missing_response = subprocess.run(
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
                "@missing.rsp",
            ],
            cwd=root / "build",
            capture_output=True,
            check=False,
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(
            missing_response.returncode == 2
            and b"unreadable-before-execution" in missing_response.stderr,
            "wrapper did not reject a missing response file before compiler execution",
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
            timeout=SHORT_PROCESS_TIMEOUT,
        )
        require(invalid.returncode == 2 and invalid.stdout == b"", "incomplete CLI input was accepted")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"gcc capture CLI test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
