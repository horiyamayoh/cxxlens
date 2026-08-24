#!/usr/bin/env python3
"""Check that BUILD_TESTING cannot change the shipped product surface.

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
_CMAKE_TARGET_OUTPUT = re.compile(
    r"(?:^|/)CMakeFiles/(?P<target>[^/]+)\.dir(?:/|$)"
)
_TEST_ONLY_TARGET = re.compile(
    r"(?:^|[-_.])(?:test|tests|testing|unit|acceptance|quality|qualification|"
    r"safety_support|benchmark|fuzz)(?:$|[-_.])",
    re.IGNORECASE,
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
        re.compile(
            r"(?:^|[^A-Za-z0-9])(?:[A-Za-z_][A-Za-z0-9_]*_)?for_testing\b"
        ),
    ),
    (
        "test peer identifier",
        re.compile(
            r"(?:^|[_:./-])test_?peer(?:$|[_:.(<])",
            re.IGNORECASE,
        ),
    ),
    (
        "test view identifier",
        re.compile(
            r"(?:^|[_:./-])test_?view(?:$|[_:.(<])",
            re.IGNORECASE,
        ),
    ),
    (
        "test factory identifier",
        re.compile(
            r"(?:^|[_:./-])test_?factory(?:$|[_:.(<])",
            re.IGNORECASE,
        ),
    ),
    (
        "Store fault dispatcher",
        re.compile(r"dispatch_sqlite_store_fault\b"),
    ),
    (
        "Store fault implementation",
        re.compile(r"sqlite_store_fault_injection(?:_noop)?(?:_internal)?\b"),
    ),
    (
        "test kernel target",
        re.compile(r"cxxlens(?:::|[-_])test[-_]kernel\b", re.IGNORECASE),
    ),
    (
        "fixture/mutation test seam",
        re.compile(
            r"\b(?:[A-Za-z0-9_]*(?:test|fault)_(?:fixture|mutation)[A-Za-z0-9_]*|"
            r"[A-Za-z0-9_]*(?:fixture|mutation)_(?:test|only|hook|seam)[A-Za-z0-9_]*)\b",
            re.IGNORECASE,
        ),
    ),
    (
        "retired request/task protocol",
        re.compile(
            r"(?:materialization_request_v2_1|provider_task_v3|"
            r"materializer_legacy_request_support|provider_protocol_v1(?:_[0-9]+)?|"
            r"request[-_.]?2[-_.]?1|task[-_.]?v3)",
            re.IGNORECASE,
        ),
    ),
    (
        "legacy binding authority",
        re.compile(r"LEGACY_BINDINGS\b"),
    ),
    (
        "lint bypass",
        re.compile(r"SKIP_LINTING\b"),
    ),
    (
        "implementation byte authority",
        re.compile(
            r"(?:(?:implementation|schema)_(?:source_)?sha(?:256)?|"
            r"source_sha(?:256)?_(?:binding|authority)|implementation_bytes?|"
            r"byte_(?:authority|binding|drift)|frozen_(?:source|bytes?))",
            re.IGNORECASE,
        ),
    ),
    (
        "testing header",
        re.compile(r"(?:^|[/\\])testing\.(?:h|hh|hpp|hxx)$", re.IGNORECASE),
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


def _cmake_target(output: str) -> str | None:
    match = _CMAKE_TARGET_OUTPUT.search(output.replace("\\", "/"))
    return match.group("target") if match is not None else None


def _is_test_only_target(target: str) -> bool:
    return _TEST_ONLY_TARGET.search(target) is not None


def _is_product_target(target: str | None) -> bool:
    return target is not None and target.startswith("cxxlens") and not _is_test_only_target(target)


def _validate_product_compile_commands(document: Sequence[object]) -> None:
    for raw_entry in document:
        require(isinstance(raw_entry, dict), "compile database entry is not an object")
        output = raw_entry.get("output", "")
        if not isinstance(output, str):
            continue
        target = _cmake_target(output)
        if not _is_product_target(target):
            continue
        tokens = _command_tokens(raw_entry)
        file_text = raw_entry.get("file")
        require(isinstance(file_text, str), "product compile command has no source file")
        require_no_forbidden_seams(
            (target or "", file_text, *tokens),
            f"production compile command for {target}",
        )
        normalized_source = file_text.replace("\\", "/")
        require(
            re.search(r"(?:^|/)tests?(?:/|$)", normalized_source, re.IGNORECASE) is None,
            f"production target {target} compiles test source: {file_text}",
        )


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
    _validate_product_compile_commands(document)

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


def _ninja_build_blocks(text: str) -> Iterable[tuple[str, tuple[str, ...]]]:
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        if not lines[index].startswith("build "):
            index += 1
            continue
        block = [lines[index]]
        index += 1
        while index < len(lines) and (lines[index].startswith((" ", "\t"))):
            block.append(lines[index])
            index += 1
        yield block[0], tuple(block)


def _ninja_output_targets(header: str) -> tuple[str, ...]:
    output_text = header.removeprefix("build ").split(":", 1)[0]
    targets: list[str] = []
    for output in output_text.split():
        normalized = output.replace("\\", "/")
        target = _cmake_target(normalized)
        if target is None:
            order_prefix = "cmake_object_order_depends_target_"
            name = pathlib.PurePosixPath(normalized).name
            target = name.removeprefix(order_prefix) if name.startswith(order_prefix) else name
        targets.append(target)
    return tuple(targets)


def _is_product_graph_output(output: str) -> bool:
    if _is_product_target(output):
        return True
    name = pathlib.PurePosixPath(output).name
    if _is_test_only_target(name):
        return False
    return (
        re.fullmatch(r"libcxxlens[^/]*\.(?:a|dylib|so(?:\.[0-9.]+)?)", name) is not None
        or name in {"cxxlens_kernel.lib", "cxxlens_kernel.dll"}
        or name.startswith("cxxlens-")
    )


def check_product_target_graph(build: pathlib.Path) -> None:
    graph = build / "build.ninja"
    require(graph.is_file(), f"missing generated target graph: {graph}")
    product_blocks = 0
    for header, block in _ninja_build_blocks(graph.read_text(encoding="utf-8")):
        outputs = _ninja_output_targets(header)
        if not any(_is_product_graph_output(output) for output in outputs):
            continue
        product_blocks += 1
        require_no_forbidden_seams(block, f"production target graph in {graph}")
    require(product_blocks > 0, f"generated graph has no production targets: {graph}")
    install_script = build / "cmake_install.cmake"
    require(install_script.is_file(), f"missing generated install graph: {install_script}")
    require_no_forbidden_seams(
        install_script.read_text(encoding="utf-8").splitlines(),
        f"production install graph in {install_script}",
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
    for candidate in sorted(root.rglob("*")):
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


def _is_native_binary(path: pathlib.Path) -> bool:
    with path.open("rb") as stream:
        magic = stream.read(4)
    return magic.startswith((b"\x7fELF", b"MZ")) or magic in {
        b"\xca\xfe\xba\xbe",
        b"\xce\xfa\xed\xfe",
        b"\xcf\xfa\xed\xfe",
        b"\xfe\xed\xfa\xce",
        b"\xfe\xed\xfa\xcf",
    }


def installed_executable_names(prefix: pathlib.Path) -> tuple[str, ...]:
    binary_directory = prefix / "bin"
    require(binary_directory.is_dir(), f"installed binary directory is missing: {binary_directory}")
    names = tuple(
        sorted(
            path.name
            for path in binary_directory.iterdir()
            if (path.is_file() or path.is_symlink())
            and (path.suffix.lower() == ".exe" or path.stat().st_mode & 0o111)
            and _is_native_binary(path)
        )
    )
    require(names, f"installed prefix has no product executables: {prefix}")
    require_no_forbidden_seams(names, f"installed executables under {prefix}")
    return names


def find_product_executable(root_path: pathlib.Path, name: str) -> pathlib.Path:
    root = root_path.resolve(strict=True)
    candidates: list[pathlib.Path] = []
    for candidate in root.rglob(name):
        if not (candidate.is_file() or candidate.is_symlink()):
            continue
        parts = set(candidate.relative_to(root).parts)
        if {"CMakeFiles", "_deps"}.intersection(parts):
            continue
        candidates.append(candidate)
    require(candidates, f"product executable {name} is missing under {root}")
    candidates.sort(key=lambda value: (len(value.relative_to(root).parts), str(value)))
    shallowest_depth = len(candidates[0].relative_to(root).parts)
    shallowest = [
        candidate
        for candidate in candidates
        if len(candidate.relative_to(root).parts) == shallowest_depth
    ]
    require(
        len(shallowest) == 1,
        f"ambiguous product executable {name} under {root}: {shallowest}",
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


def require_no_forbidden_product_symbols(symbols: Iterable[str], owner: str) -> None:
    # Worker executables contain LLVM/Clang implementation symbols. Their
    # vocabulary is outside this repository's production/test boundary; only
    # cxxlens-owned symbols are classified here.
    require_no_forbidden_seams(
        (symbol for symbol in symbols if "cxxlens" in symbol.lower()), owner
    )


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
    require_no_forbidden_product_symbols(symbols, str(library))
    return symbols


def scan_defined_symbols(nm: str, artifact: pathlib.Path) -> None:
    # Local and hidden definitions matter: a test seam compiled into the
    # product is a boundary violation even when it is not part of the ABI.
    command = [nm, "--defined-only", "--format=posix", "-C", str(artifact)]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    require(
        completed.returncode == 0,
        f"nm failed for {artifact}: {completed.stderr.strip()}",
    )
    symbols = parse_nm_symbols(completed.stdout)
    require(symbols, f"nm returned no defined symbols for {artifact}")
    require_no_forbidden_product_symbols(symbols, str(artifact))


def product_library_artifacts(root: pathlib.Path) -> tuple[pathlib.Path, ...]:
    resolved: dict[pathlib.Path, pathlib.Path] = {}
    for candidate in sorted(root.rglob("*")):
        if not (candidate.is_file() or candidate.is_symlink()):
            continue
        relative = candidate.relative_to(root)
        if {"CMakeFiles", "_deps"}.intersection(relative.parts):
            continue
        name = candidate.name
        if _is_test_only_target(name):
            continue
        if not (
            re.fullmatch(r"libcxxlens[^/]*\.(?:a|dylib|so(?:\.[0-9.]+)?)", name)
            or re.fullmatch(r"cxxlens[^/]*\.(?:lib|dll)", name)
        ):
            continue
        target = candidate.resolve(strict=True)
        previous = resolved.get(target)
        if previous is None or len(relative.parts) < len(previous.relative_to(root).parts):
            resolved[target] = candidate
    artifacts = tuple(sorted(resolved.values(), key=lambda value: str(value)))
    require(artifacts, f"no production libraries found under {root}")
    return artifacts


def scan_product_artifacts(
    nm: str, root: pathlib.Path, executable_names: Sequence[str]
) -> None:
    for artifact in product_library_artifacts(root):
        scan_defined_symbols(nm, artifact)
    for name in executable_names:
        scan_defined_symbols(nm, find_product_executable(root, name))


def _surface_files(prefix: pathlib.Path) -> tuple[SurfaceFile, ...]:
    header_root = prefix / "include"
    require(header_root.is_dir(), f"installed include directory is missing: {header_root}")
    installed_paths = sorted(path for path in prefix.rglob("*") if path.is_file())
    require_no_forbidden_seams(
        (path.relative_to(prefix).as_posix() for path in installed_paths),
        f"installed paths under {prefix}",
    )
    for path in installed_paths:
        relative = path.relative_to(prefix)
        components = {part.lower() for part in relative.parts}
        require(
            not {"test", "tests", "testing"}.intersection(components)
            and "cxxlens-quality" not in relative.as_posix().lower(),
            f"installed test/quality path is not a product artifact: {relative}",
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
    check_product_target_graph(enabled_profile.build)
    check_product_target_graph(disabled_profile.build)

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
    executable_names = installed_executable_names(enabled_install)
    require(
        executable_names == installed_executable_names(disabled_install),
        "BUILD_TESTING changes the installed product executable set",
    )
    for root in (
        enabled_profile.build,
        disabled_profile.build,
        enabled_install,
        disabled_install,
    ):
        scan_product_artifacts(nm, root, executable_names)


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
