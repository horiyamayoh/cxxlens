#!/usr/bin/env python3
"""Check that BUILD_TESTING cannot change the shipped kernel surface.

The checker is deliberately read-only.  It consumes two already-configured and
built trees plus their already-populated install prefixes, writes no report, and
uses only its exit status as the gate result.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Iterable, Mapping, Sequence


class ProductionBoundaryError(RuntimeError):
    """The supplied artifacts do not establish the production boundary."""


@dataclasses.dataclass(frozen=True)
class CompileProfile:
    build: pathlib.Path
    build_testing: bool
    shared: bool
    sources: frozenset[str]
    definitions_by_source: tuple[tuple[str, tuple[str, ...]], ...]


@dataclasses.dataclass(frozen=True)
class SurfaceFile:
    relative_path: str
    text: str


_TRUE_CACHE_VALUES = frozenset({"1", "ON", "TRUE", "YES", "Y"})
_FALSE_CACHE_VALUES = frozenset({"0", "OFF", "FALSE", "NO", "N", ""})
_PRODUCT_TARGET_OUTPUT = re.compile(
    r"(?:^|/)CMakeFiles/cxxlens_kernel\.dir(?:/|$)"
)
_NM_LINE = re.compile(
    r"^(?P<name>.+) (?P<kind>[A-Za-z?]) (?P<value>[0-9A-Fa-f]+)"
    r"(?: (?P<size>[0-9A-Fa-f]+))?$"
)

# These patterns name test seams, not product safety concepts.  In particular,
# receipt, digest, provenance, identity, ordinary view, and ordinary factory
# identifiers are intentionally outside this deny set.
_FORBIDDEN_SEAMS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "test-support macro",
        re.compile(r"\b[A-Z][A-Z0-9_]*TEST_SUPPORT\b"),
    ),
    (
        "test-support identifier",
        re.compile(
            r"(?:^|[/_.:-])test[-_]support(?:$|[/_.:-])",
            re.IGNORECASE,
        ),
    ),
    (
        "for-testing identifier",
        re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*_for_testing\b"),
    ),
    (
        "test peer identifier",
        re.compile(
            r"\b[A-Za-z_][A-Za-z0-9_]*test_?peer[A-Za-z0-9_]*\b",
            re.IGNORECASE,
        ),
    ),
    (
        "test view identifier",
        re.compile(
            r"\b[A-Za-z_][A-Za-z0-9_]*test_?view[A-Za-z0-9_]*\b",
            re.IGNORECASE,
        ),
    ),
    (
        "test factory identifier",
        re.compile(
            r"\b[A-Za-z_][A-Za-z0-9_]*test_?factory[A-Za-z0-9_]*\b",
            re.IGNORECASE,
        ),
    ),
    (
        "Store fault dispatcher",
        re.compile(r"\bdispatch_sqlite_store_fault\b"),
    ),
    (
        "test kernel target",
        re.compile(r"\bcxxlens(?:::|[-_])test[-_]kernel\b", re.IGNORECASE),
    ),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ProductionBoundaryError(message)


def _read_cache(build: pathlib.Path) -> dict[str, str]:
    cache_path = build / "CMakeCache.txt"
    require(cache_path.is_file(), f"missing CMake cache: {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        if ":" not in declaration:
            continue
        name, _kind = declaration.split(":", 1)
        values[name] = value
    return values


def _cache_bool(cache: Mapping[str, str], name: str) -> bool:
    require(name in cache, f"CMake cache does not define {name}")
    value = cache[name].strip().upper()
    if value in _TRUE_CACHE_VALUES:
        return True
    if value in _FALSE_CACHE_VALUES:
        return False
    raise ProductionBoundaryError(f"CMake cache has a non-boolean {name}: {cache[name]!r}")


def _command_tokens(entry: Mapping[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    require(isinstance(command, str), "compile command has neither arguments nor command")
    return shlex.split(command, posix=True)


def _compile_definitions(tokens: Sequence[str]) -> tuple[str, ...]:
    definitions: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in {"-D", "/D"}:
            require(index + 1 < len(tokens), f"compile definition flag has no value: {token}")
            definitions.append(tokens[index + 1])
            index += 2
            continue
        if token.startswith("-D") and len(token) > 2:
            definitions.append(token[2:])
        elif token.startswith("/D") and len(token) > 2:
            definitions.append(token[2:])
        index += 1
    return tuple(sorted(set(definitions)))


def _source_key(file: pathlib.Path, source_root: pathlib.Path, build: pathlib.Path) -> str:
    resolved = file.resolve(strict=False)
    try:
        return resolved.relative_to(source_root).as_posix()
    except ValueError:
        try:
            return "@build/" + resolved.relative_to(build).as_posix()
        except ValueError:
            return "@external/" + resolved.name


def load_compile_profile(build_path: pathlib.Path, expected_testing: bool) -> CompileProfile:
    build = build_path.resolve(strict=True)
    cache = _read_cache(build)
    build_testing = _cache_bool(cache, "BUILD_TESTING")
    require(
        build_testing == expected_testing,
        f"{build} has BUILD_TESTING={build_testing}, expected {expected_testing}",
    )
    shared = _cache_bool(cache, "CXXLENS_BUILD_SHARED")
    source_root_text = cache.get("CMAKE_HOME_DIRECTORY")
    require(source_root_text is not None, "CMake cache lacks CMAKE_HOME_DIRECTORY")
    source_root = pathlib.Path(source_root_text).resolve(strict=False)

    database_path = build / "compile_commands.json"
    require(database_path.is_file(), f"missing compile database: {database_path}")
    document = json.loads(database_path.read_text(encoding="utf-8"))
    require(isinstance(document, list), f"compile database is not an array: {database_path}")

    definitions: dict[str, tuple[str, ...]] = {}
    for raw_entry in document:
        require(isinstance(raw_entry, dict), "compile database entry is not an object")
        output = raw_entry.get("output", "")
        if not isinstance(output, str) or not _PRODUCT_TARGET_OUTPUT.search(
            output.replace("\\", "/")
        ):
            continue
        file_text = raw_entry.get("file")
        require(isinstance(file_text, str), "product compile command has no source file")
        source = _source_key(pathlib.Path(file_text), source_root, build)
        source_definitions = _compile_definitions(_command_tokens(raw_entry))
        previous = definitions.setdefault(source, source_definitions)
        require(
            previous == source_definitions,
            f"product source has inconsistent compile definitions: {source}",
        )

    require(definitions, f"compile database has no cxxlens_kernel entries: {database_path}")
    for source, values in definitions.items():
        forbidden = [value for value in values if "TEST_SUPPORT" in value.upper()]
        require(
            not forbidden,
            f"product source {source} receives test-support definitions: {forbidden}",
        )

    return CompileProfile(
        build=build,
        build_testing=build_testing,
        shared=shared,
        sources=frozenset(definitions),
        definitions_by_source=tuple(sorted(definitions.items())),
    )


def compare_compile_profiles(enabled: CompileProfile, disabled: CompileProfile) -> None:
    require(enabled.build_testing, "enabled profile is not a BUILD_TESTING build")
    require(not disabled.build_testing, "disabled profile is not a production-only build")
    require(
        enabled.shared == disabled.shared,
        "BUILD_TESTING builds use different static/shared linkage",
    )
    require(
        enabled.sources == disabled.sources,
        "BUILD_TESTING changes the cxxlens_kernel source set",
    )
    require(
        enabled.definitions_by_source == disabled.definitions_by_source,
        "BUILD_TESTING changes cxxlens_kernel compile definitions",
    )


def find_forbidden_seam(text: str) -> tuple[str, str] | None:
    for description, pattern in _FORBIDDEN_SEAMS:
        match = pattern.search(text)
        if match is not None:
            return description, match.group(0)
    return None


def require_no_forbidden_seams(values: Iterable[str], owner: str) -> None:
    for value in values:
        found = find_forbidden_seam(value)
        if found is not None:
            description, token = found
            raise ProductionBoundaryError(
                f"{owner} exposes {description}: {token!r} in {value!r}"
            )


def _kernel_library_candidates(root: pathlib.Path, shared: bool) -> list[pathlib.Path]:
    def matches(path: pathlib.Path) -> bool:
        name = path.name
        if shared:
            return (
                name.startswith("libcxxlens_kernel.so")
                or name.startswith("libcxxlens_kernel.") and name.endswith(".dylib")
                or name == "cxxlens_kernel.dll"
            )
        return name in {"libcxxlens_kernel.a", "cxxlens_kernel.lib"}

    resolved: dict[pathlib.Path, pathlib.Path] = {}
    for candidate in root.rglob("*"):
        if not matches(candidate) or not (candidate.is_file() or candidate.is_symlink()):
            continue
        parts = set(candidate.relative_to(root).parts)
        if {"CMakeFiles", "_deps"}.intersection(parts):
            continue
        target = candidate.resolve(strict=True)
        previous = resolved.get(target)
        if previous is None or len(candidate.relative_to(root).parts) < len(
            previous.relative_to(root).parts
        ):
            resolved[target] = candidate
    return sorted(
        resolved.values(),
        key=lambda value: (len(value.relative_to(root).parts), str(value)),
    )


def find_kernel_library(root_path: pathlib.Path, shared: bool) -> pathlib.Path:
    root = root_path.resolve(strict=True)
    candidates = _kernel_library_candidates(root, shared)
    require(candidates, f"no cxxlens kernel library found under {root}")
    shallowest_depth = len(candidates[0].relative_to(root).parts)
    shallowest = [
        candidate
        for candidate in candidates
        if len(candidate.relative_to(root).parts) == shallowest_depth
    ]
    require(
        len(shallowest) == 1,
        f"ambiguous cxxlens kernel libraries under {root}: {shallowest}",
    )
    return shallowest[0]


def find_nm(explicit: str | None) -> str:
    if explicit is not None:
        resolved = shutil.which(explicit)
        if resolved is None and pathlib.Path(explicit).is_file():
            resolved = str(pathlib.Path(explicit).resolve())
        require(resolved is not None, f"nm tool is unavailable: {explicit}")
        return resolved
    for candidate in ("llvm-nm-22", "llvm-nm", "nm"):
        resolved = shutil.which(candidate)
        if resolved is not None:
            return resolved
    raise ProductionBoundaryError("no llvm-nm or nm executable is available")


def parse_nm_symbols(output: str) -> frozenset[str]:
    symbols: set[str] = set()
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or line.endswith(("]:", ":")):
            continue
        match = _NM_LINE.fullmatch(line)
        if match is not None:
            symbols.add(match.group("name"))
    return frozenset(symbols)


def read_symbols(nm: str, library: pathlib.Path, shared: bool) -> frozenset[str]:
    command = [nm, "-g", "--defined-only", "--format=posix", "-C"]
    if shared:
        command.append("-D")
    command.append(str(library))
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    require(
        completed.returncode == 0,
        f"nm failed for {library}: {completed.stderr.strip()}",
    )
    symbols = parse_nm_symbols(completed.stdout)
    require(symbols, f"nm returned no defined global symbols for {library}")
    require_no_forbidden_seams(symbols, str(library))
    return symbols


def _surface_files(prefix: pathlib.Path) -> tuple[SurfaceFile, ...]:
    header_root = prefix / "include"
    require(header_root.is_dir(), f"installed include directory is missing: {header_root}")
    installed_paths = sorted(path for path in prefix.rglob("*") if path.is_file())
    require_no_forbidden_seams(
        (path.relative_to(prefix).as_posix() for path in installed_paths),
        f"installed paths under {prefix}",
    )
    header_paths = sorted(
        path
        for path in header_root.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}
    )
    export_paths = sorted(path for path in prefix.rglob("*.cmake") if path.is_file())
    require(header_paths, f"installed prefix has no public headers: {prefix}")
    require(export_paths, f"installed prefix has no CMake exports: {prefix}")

    files: list[SurfaceFile] = []
    prefix_text = str(prefix)
    for path in (*header_paths, *export_paths):
        text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        normalized = text.replace(prefix_text, "<INSTALL_PREFIX>")
        relative_path = path.relative_to(prefix).as_posix()
        require_no_forbidden_seams(normalized.splitlines(), relative_path)
        files.append(SurfaceFile(relative_path, normalized))
    return tuple(files)


def compare_installed_surfaces(
    enabled_prefix: pathlib.Path, disabled_prefix: pathlib.Path
) -> None:
    enabled = _surface_files(enabled_prefix.resolve(strict=True))
    disabled = _surface_files(disabled_prefix.resolve(strict=True))
    require(
        tuple(item.relative_path for item in enabled)
        == tuple(item.relative_path for item in disabled),
        "BUILD_TESTING changes the installed header/export file set",
    )
    require(enabled == disabled, "BUILD_TESTING changes installed headers or CMake exports")


def check(
    enabled_build: pathlib.Path,
    disabled_build: pathlib.Path,
    enabled_prefix: pathlib.Path,
    disabled_prefix: pathlib.Path,
    nm_executable: str | None = None,
) -> None:
    enabled_profile = load_compile_profile(enabled_build, expected_testing=True)
    disabled_profile = load_compile_profile(disabled_build, expected_testing=False)
    compare_compile_profiles(enabled_profile, disabled_profile)

    nm = find_nm(nm_executable)
    enabled_build_library = find_kernel_library(enabled_profile.build, enabled_profile.shared)
    disabled_build_library = find_kernel_library(disabled_profile.build, disabled_profile.shared)
    enabled_build_symbols = read_symbols(nm, enabled_build_library, enabled_profile.shared)
    disabled_build_symbols = read_symbols(nm, disabled_build_library, disabled_profile.shared)
    require(
        enabled_build_symbols == disabled_build_symbols,
        "BUILD_TESTING changes the cxxlens_kernel global ABI",
    )

    enabled_install = enabled_prefix.resolve(strict=True)
    disabled_install = disabled_prefix.resolve(strict=True)
    compare_installed_surfaces(enabled_install, disabled_install)
    enabled_installed_library = find_kernel_library(enabled_install, enabled_profile.shared)
    disabled_installed_library = find_kernel_library(disabled_install, disabled_profile.shared)
    enabled_installed_symbols = read_symbols(nm, enabled_installed_library, enabled_profile.shared)
    disabled_installed_symbols = read_symbols(
        nm, disabled_installed_library, disabled_profile.shared
    )
    require(
        enabled_installed_symbols == disabled_installed_symbols,
        "BUILD_TESTING changes the installed cxxlens_kernel global ABI",
    )
    require(
        enabled_build_symbols == enabled_installed_symbols,
        "BUILD_TESTING=ON build and installed kernel ABI differ",
    )
    require(
        disabled_build_symbols == disabled_installed_symbols,
        "BUILD_TESTING=OFF build and installed kernel ABI differ",
    )


def main(arguments: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-testing-on", required=True, type=pathlib.Path)
    parser.add_argument("--build-testing-off", required=True, type=pathlib.Path)
    parser.add_argument("--install-testing-on", required=True, type=pathlib.Path)
    parser.add_argument("--install-testing-off", required=True, type=pathlib.Path)
    parser.add_argument("--nm", dest="nm_executable")
    parsed = parser.parse_args(arguments)
    try:
        check(
            parsed.build_testing_on,
            parsed.build_testing_off,
            parsed.install_testing_on,
            parsed.install_testing_off,
            parsed.nm_executable,
        )
    except (OSError, ValueError, json.JSONDecodeError, ProductionBoundaryError) as error:
        print(f"check_production_boundary: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
