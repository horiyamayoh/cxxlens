#!/usr/bin/env python3
"""Run a resource-aware cxxlens gate and emit reproducible timing evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import resource
import shutil
import subprocess
import sys
import time
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]


def output(command: list[str], root: pathlib.Path) -> str:
    return subprocess.run(
        command, cwd=root, check=True, capture_output=True, text=True
    ).stdout.strip()


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def worktree_digest(root: pathlib.Path) -> str:
    diff = output(["git", "diff", "--binary", "HEAD", "--"], root).encode("utf-8")
    entries = bytearray(diff)
    for relative in sorted(output(["git", "ls-files", "--others", "--exclude-standard"], root).splitlines()):
        if not relative:
            continue
        path = root / relative
        entries.extend(relative.encode("utf-8"))
        entries.extend(b"\0")
        entries.extend(path.read_bytes())
        entries.extend(b"\0")
    return digest_bytes(bytes(entries))


def available_memory_gib() -> float:
    try:
        fields = pathlib.Path("/proc/meminfo").read_text(encoding="utf-8").splitlines()
        available = next(line for line in fields if line.startswith("MemAvailable:"))
        return int(available.split()[1]) / 1024 / 1024
    except (OSError, StopIteration, ValueError):
        return 2.0


def parallel_levels(preset: str) -> tuple[int, int]:
    cpus = max(1, os.cpu_count() or 1)
    memory_jobs = max(1, int(available_memory_gib() / 1.5))
    build_jobs = min(cpus, memory_jobs)
    test_jobs = build_jobs
    if preset == "asan-ubsan":
        build_jobs = min(build_jobs, 4)
        test_jobs = min(test_jobs, 2)
    elif preset == "tsan":
        build_jobs = min(build_jobs, 2)
        test_jobs = 1
    return build_jobs, test_jobs


def run_phase(
    name: str,
    command: list[str],
    *,
    root: pathlib.Path,
    environment: dict[str, str],
) -> dict[str, Any]:
    started = time.monotonic()
    cpu_started = resource.getrusage(resource.RUSAGE_CHILDREN)
    completed = subprocess.run(command, cwd=root, env=environment, check=False)
    elapsed = time.monotonic() - started
    cpu_finished = resource.getrusage(resource.RUSAGE_CHILDREN)
    result = {
        "name": name,
        "command": command,
        "wall_seconds": round(elapsed, 6),
        "user_cpu_seconds": round(cpu_finished.ru_utime - cpu_started.ru_utime, 6),
        "system_cpu_seconds": round(cpu_finished.ru_stime - cpu_started.ru_stime, 6),
        "return_code": completed.returncode,
    }
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return result


def ninja_summary(path: pathlib.Path, start_offset: int) -> dict[str, Any]:
    if not path.is_file():
        return {"entries": 0, "total_edge_milliseconds": 0, "critical_path": []}
    rows: list[tuple[int, str]] = []
    data = path.read_bytes()
    if start_offset and len(data) >= start_offset:
        data = data[start_offset:]
    for line in data.decode("utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) >= 4:
            try:
                rows.append((max(0, int(fields[1]) - int(fields[0])), fields[3]))
            except ValueError:
                continue
    rows.sort(key=lambda row: (-row[0], row[1]))
    return {
        "entries": len(rows),
        "total_edge_milliseconds": sum(duration for duration, _ in rows),
        "critical_path": [
            {"output": name, "milliseconds": duration} for duration, name in rows[:10]
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("fast", "check", "full", "stress"))
    parser.add_argument("--preset", default="dev-clang")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--baseline", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    build_dir = root / "build" / args.preset
    ninja_log = build_dir / ".ninja_log"
    ninja_log_offset = ninja_log.stat().st_size if ninja_log.is_file() else 0
    revision = output(["git", "rev-parse", "HEAD"], root)
    tree = output(["git", "rev-parse", "HEAD^{tree}"], root)
    clean = output(["git", "status", "--porcelain=v1"], root) == ""
    if args.mode in {"full", "stress"} and not clean:
        print("quality gate failed: full/stress requires a clean worktree", file=sys.stderr)
        return 1
    build_jobs, test_jobs = parallel_levels(args.preset)
    environment = dict(os.environ)
    environment["CMAKE_BUILD_PARALLEL_LEVEL"] = str(build_jobs)
    environment["CTEST_PARALLEL_LEVEL"] = str(test_jobs)
    junit = args.report.with_suffix(".junit.xml").resolve()
    quality_junit = args.report.with_suffix(".quality.junit.xml").resolve()
    install_junit = args.report.with_suffix(".install.junit.xml").resolve()
    phases: list[dict[str, Any]] = []
    started = time.monotonic()
    try:
        if args.configure:
            phases.append(
                run_phase(
                    "configure",
                    ["cmake", "--preset", args.preset, *args.cmake_arg],
                    root=root,
                    environment=environment,
                )
            )
        target = "cxxlens-ng-test-binaries" if args.mode == "fast" else "all"
        phases.append(
            run_phase(
                "build",
                ["cmake", "--build", "--preset", args.preset, "--target", target],
                root=root,
                environment=environment,
            )
        )
        test_command = [
            "ctest",
            "--preset",
            args.preset,
            "--parallel",
            str(test_jobs),
            "--output-junit",
            str(junit),
        ]
        if args.mode == "fast":
            test_command.extend(["--label-exclude", "quality|install|slow"])
        else:
            test_command.extend(["--label-exclude", "quality|install"])
        phases.append(
            run_phase("ctest-runtime", test_command, root=root, environment=environment)
        )
        if args.mode in {"check", "full", "stress"}:
            phases.append(
                run_phase(
                    "ctest-quality-unit",
                    [
                        "ctest",
                        "--preset",
                        args.preset,
                        "--parallel",
                        str(test_jobs),
                        "--label-regex",
                        "quality",
                        "--output-junit",
                        str(quality_junit),
                    ],
                    root=root,
                    environment=environment,
                )
            )
            phases.append(
                run_phase(
                    "quality-production",
                    [
                        "cmake",
                        "--build",
                        "--preset",
                        args.preset,
                        "--target",
                        "cxxlens-quality",
                    ],
                    root=root,
                    environment=environment,
                )
            )
        if args.mode in {"full", "stress"}:
            phases.append(
                run_phase(
                    "ctest-install",
                    [
                        "ctest",
                        "--preset",
                        args.preset,
                        "--parallel",
                        str(test_jobs),
                        "--label-regex",
                        "install",
                        "--output-junit",
                        str(install_junit),
                    ],
                    root=root,
                    environment=environment,
                )
            )
        if args.mode == "stress":
            phases.append(
                run_phase(
                    "repeat",
                    [
                        "ctest",
                        "--preset",
                        args.preset,
                        "--parallel",
                        str(test_jobs),
                        "--repeat",
                        "until-fail:3",
                        "--label-exclude",
                        "quality|install",
                    ],
                    root=root,
                    environment=environment,
                )
            )
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"quality gate failed: {error}", file=sys.stderr)
        return 1

    compiler = output([os.environ.get("CXX", "c++"), "--version"], root).splitlines()[0]
    toolchain_digest = "sha256:" + hashlib.sha256(compiler.encode()).hexdigest()
    report = {
        "schema": "cxxlens.quality-run-report.v1",
        "mode": args.mode,
        "result": "passed",
        "source": {
            "revision": revision,
            "tree": tree,
            "clean": clean,
            "worktree_digest": worktree_digest(root),
        },
        "preset": args.preset,
        "parallel": {
            "cpu_count": os.cpu_count() or 1,
            "available_memory_gib": round(available_memory_gib(), 3),
            "build": build_jobs,
            "test": test_jobs,
        },
        "toolchain": {"identity": compiler, "digest": toolchain_digest},
        "configuration": {
            "preset": args.preset,
            "cmake_args": args.cmake_arg,
            "digest": digest_bytes(
                json.dumps(
                    {"preset": args.preset, "cmake_args": args.cmake_arg},
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8")
            ),
        },
        "checker": {
            "path": str(pathlib.Path(__file__).resolve()),
            "digest": digest_bytes(pathlib.Path(__file__).read_bytes()),
        },
        "cache": {"used": shutil.which("ccache") is not None, "correctness_evidence": False},
        "wall_seconds": round(time.monotonic() - started, 6),
        "phases": phases,
        "logical_checks": [
            {
                "id": f"gate.{args.mode}.{phase['name']}",
                "executions": 1,
                "configuration": args.preset,
            }
            for phase in phases
        ],
        "ctest_junit": {
            "runtime": str(junit),
            "quality": str(quality_junit) if args.mode != "fast" else None,
            "install": str(install_junit) if args.mode in {"full", "stress"} else None,
        },
        "ninja": ninja_summary(ninja_log, ninja_log_offset),
    }
    if args.baseline:
        try:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"quality gate failed: invalid baseline report: {error}", file=sys.stderr)
            return 1
        if baseline.get("schema") != report["schema"] or baseline.get("mode") != args.mode:
            print("quality gate failed: baseline workload mode differs", file=sys.stderr)
            return 1
        if baseline.get("configuration") != report["configuration"]:
            print("quality gate failed: baseline configuration differs", file=sys.stderr)
            return 1
        baseline_phases = {row["name"]: row for row in baseline.get("phases", [])}
        if set(baseline_phases) != {row["name"] for row in phases}:
            print("quality gate failed: baseline phase set differs", file=sys.stderr)
            return 1
        report["baseline_comparison"] = {
            "report": str(args.baseline.resolve()),
            "wall_seconds_delta": round(
                report["wall_seconds"] - baseline["wall_seconds"], 6
            ),
            "phases": [
                {
                    "name": row["name"],
                    "wall_seconds_delta": round(
                        row["wall_seconds"]
                        - baseline_phases[row["name"]]["wall_seconds"],
                        6,
                    ),
                }
                for row in phases
            ],
        }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"{args.mode} gate passed; report={args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
