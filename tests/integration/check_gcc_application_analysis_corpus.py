#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def execute(arguments: list[str], *, cwd: pathlib.Path, timeout: int = 60):
    return subprocess.run(
        arguments,
        cwd=cwd,
        capture_output=True,
        check=False,
        timeout=timeout,
    )


def main() -> int:
    require(
        len(sys.argv) == 6,
        "expected CLI, compiler, materializer, worker, and corpus directory",
    )
    cli = pathlib.Path(sys.argv[1]).resolve()
    compiler = pathlib.Path(sys.argv[2]).resolve()
    materializer = pathlib.Path(sys.argv[3]).resolve()
    worker = pathlib.Path(sys.argv[4]).resolve()
    source = pathlib.Path(sys.argv[5]).resolve()
    for path in (cli, compiler, materializer, worker, source / "CMakeLists.txt"):
        require(path.exists(), f"required corpus input is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="cxxlens-gcc-application-corpus-") as raw_root:
        root = pathlib.Path(raw_root).resolve()
        project = root / "project"
        shutil.copytree(source, project)
        build = project / "build"
        configured = execute(
            [
                "cmake",
                "-S",
                str(project),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DCMAKE_CXX_COMPILER={compiler}",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            ],
            cwd=project,
        )
        require(configured.returncode == 0, f"corpus configure failed: {configured.stderr!r}")
        built = execute(["cmake", "--build", str(build)], cwd=project)
        require(built.returncode == 0, f"corpus build failed: {built.stderr!r}")
        executed = execute([str(build / "gcc_application_corpus")], cwd=project)
        require(executed.returncode == 0, "GCC-built corpus behavior changed")

        # CMake/Ninja appends a private depfile option after the command exported in the
        # compilation database. Execute the exported, shell-free argv as well so each
        # explicitly declared depfile remains available to the capture adapter.
        database = json.loads(
            (build / "compile_commands.json").read_text(encoding="utf-8")
        )
        require(len(database) == 3, "real-project compilation database census changed")
        for index, entry in enumerate(database):
            arguments = entry.get("arguments") or shlex.split(entry["command"])
            require(
                pathlib.Path(arguments[0]).resolve() == compiler,
                f"compile unit {index} was not bound to exact GCC",
            )
            compiled = execute(arguments, cwd=pathlib.Path(entry["directory"]))
            require(
                compiled.returncode == 0,
                f"exported compile unit {index} failed: {compiled.stderr!r}",
            )

        command = [
            str(cli),
            "capture",
            "--project-id",
            "project:gcc-application-corpus",
            "--project-root",
            str(project),
            "--compile-commands",
            str(build / "compile_commands.json"),
            "--compiler",
            str(compiler),
        ]
        first = execute(command, cwd=project)
        second = execute(command, cwd=project)
        require(first.returncode == 0, f"first corpus capture failed: {first.stderr!r}")
        require(second.returncode == 0, f"second corpus capture failed: {second.stderr!r}")
        require(
            first.stdout and first.stdout == second.stdout,
            "real-project corpus capture was empty or nondeterministic",
        )
        require(
            first.stderr == b"" and second.stderr == b"",
            "successful real-project capture wrote diagnostics",
        )
        bundle = root / "gcc-application-corpus.cxxlens"
        bundle.write_bytes(first.stdout)
        analyzed = execute(
            [str(materializer), str(bundle), str(worker), "3"],
            cwd=project,
            timeout=90,
        )
        require(
            analyzed.returncode == 0,
            "real-project capture/replay/materialize/query failed "
            f"with exit {analyzed.returncode}: {analyzed.stderr!r}",
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"GCC application-analysis corpus failed: {error}", file=sys.stderr)
        raise SystemExit(1)
