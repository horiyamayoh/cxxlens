#!/usr/bin/env python3
"""Apply and export the exact downloaded-package cache follow-up for issue #316."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    raise SystemExit(message)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        fail(f"{label} replacement count differs: {count}")
    return text.replace(old, new, 1)


def canonical_digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def update_setup_action() -> None:
    path = ROOT / ".github/actions/setup-ci/action.yml"
    text = path.read_text(encoding="utf-8")
    marker = (
        "    - uses: actions/setup-python@"
        "5fda3b95a4ea91299a34e894583c3862153e4b97\n"
    )
    cache = """    - id: package-cache
      uses: actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830
      if: ${{ inputs.profile != 'none' || inputs.documentation == 'true' }}
      with:
        path: ~/.cache/cxxlens/packages
        key: cxxlens-ci-packages-v1-${{ runner.os }}-${{ runner.arch }}-${{ inputs.profile }}-${{ inputs.documentation }}-${{ hashFiles('tools/ci/llvm22-noble.lock.json') }}

    - name: Bind exact downloaded-package cache
      if: ${{ inputs.profile != 'none' || inputs.documentation == 'true' }}
      shell: bash
      run: |
        echo "CXXLENS_PACKAGE_CACHE=${HOME}/.cache/cxxlens/packages" >> "${GITHUB_ENV}"
        echo "CXXLENS_PACKAGE_CACHE_KEY=cxxlens-ci-packages-v1-${{ runner.os }}-${{ runner.arch }}-${{ inputs.profile }}-${{ inputs.documentation }}-${{ hashFiles('tools/ci/llvm22-noble.lock.json') }}" >> "${GITHUB_ENV}"
        echo "CXXLENS_PACKAGE_CACHE_HIT=${{ steps.package-cache.outputs.cache-hit }}" >> "${GITHUB_ENV}"
        echo "CXXLENS_PACKAGE_CACHE_RECEIPT=${RUNNER_TEMP}/cxxlens-package-cache-receipt.json" >> "${GITHUB_ENV}"

"""
    path.write_text(
        replace_once(text, marker, cache + marker, "setup action cache"),
        encoding="utf-8",
    )


def update_lock() -> dict[str, object]:
    path = ROOT / "tools/ci/llvm22-noble.lock.json"
    lock = json.loads(path.read_text(encoding="utf-8"))
    lock["document_version"] = "1.5.0"
    lock["actions"]["actions/cache"] = (
        "0057852bfaa89a56745cba8c7296529d2fc39830"
    )
    lock["package_cache"] = {
        "directory": "~/.cache/cxxlens/packages",
        "environment": "CXXLENS_PACKAGE_CACHE",
        "hit_environment": "CXXLENS_PACKAGE_CACHE_HIT",
        "key_environment": "CXXLENS_PACKAGE_CACHE_KEY",
        "receipt_environment": "CXXLENS_PACKAGE_CACHE_RECEIPT",
        "key_version": "v1",
        "key_template": (
            "cxxlens-ci-packages-v1-${runner.os}-${runner.arch}-"
            "${profile}-${documentation}-${lock_digest}"
        ),
        "scope": "exact-downloaded-debs-only",
        "correctness_role": "transport-optimization-only",
        "restore_keys": False,
    }
    action = ROOT / ".github/actions/setup-ci/action.yml"
    lock["local_actions"][".github/actions/setup-ci/action.yml"] = (
        hashlib.sha256(action.read_bytes()).hexdigest()
    )
    path.write_text(
        json.dumps(lock, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return lock


def update_bootstrap() -> None:
    path = ROOT / "tools/ci/bootstrap_supply_chain.py"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "import json\nimport pathlib\nimport platform\n",
        "import json\nimport os\nimport pathlib\nimport platform\nimport shutil\n",
        "bootstrap imports",
    )
    text = replace_once(
        text,
        'SOURCE_LIST = pathlib.Path("/etc/apt/sources.list.d/cxxlens-llvm.list")\n',
        'SOURCE_LIST = pathlib.Path("/etc/apt/sources.list.d/cxxlens-llvm.list")\n'
        'PACKAGE_CACHE_ENV = "CXXLENS_PACKAGE_CACHE"\n'
        'PACKAGE_CACHE_HIT_ENV = "CXXLENS_PACKAGE_CACHE_HIT"\n'
        'PACKAGE_CACHE_KEY_ENV = "CXXLENS_PACKAGE_CACHE_KEY"\n'
        'PACKAGE_CACHE_RECEIPT_ENV = "CXXLENS_PACKAGE_CACHE_RECEIPT"\n',
        "bootstrap constants",
    )
    old_contract = '''    local_workflows = lock.get("local_workflows")
    if not all(
        isinstance(value, dict)
        for value in (llvm, documentation, python, runner, actions, local_workflows)
    ) or not local_workflows:
        raise SupplyChainError("supply-chain lock sections are missing")
'''
    new_contract = '''    local_workflows = lock.get("local_workflows")
    package_cache = lock.get("package_cache")
    if not all(
        isinstance(value, dict)
        for value in (
            llvm,
            documentation,
            python,
            runner,
            actions,
            local_workflows,
            package_cache,
        )
    ) or not local_workflows:
        raise SupplyChainError("supply-chain lock sections are missing")
    expected_package_cache = {
        "directory": "~/.cache/cxxlens/packages",
        "environment": PACKAGE_CACHE_ENV,
        "hit_environment": PACKAGE_CACHE_HIT_ENV,
        "key_environment": PACKAGE_CACHE_KEY_ENV,
        "receipt_environment": PACKAGE_CACHE_RECEIPT_ENV,
        "key_version": "v1",
        "key_template": (
            "cxxlens-ci-packages-v1-${runner.os}-${runner.arch}-"
            "${profile}-${documentation}-${lock_digest}"
        ),
        "scope": "exact-downloaded-debs-only",
        "correctness_role": "transport-optimization-only",
        "restore_keys": False,
    }
    if package_cache != expected_package_cache:
        raise SupplyChainError("downloaded-package cache contract differs")
'''
    text = replace_once(
        text,
        old_contract,
        new_contract,
        "bootstrap package-cache contract",
    )
    marker = "\ndef install_documentation(lock: dict[str, Any]) -> None:\n"
    helpers = r'''
def package_cache_authority_digest(lock: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(
            lock["package_cache"], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    ).hexdigest()


def package_cache_directory(lock: dict[str, Any]) -> pathlib.Path:
    config = lock["package_cache"]
    raw = os.environ.get(config["environment"], config["directory"])
    directory = pathlib.Path(raw).expanduser()
    if not directory.is_absolute():
        raise SupplyChainError("downloaded-package cache path must be absolute")
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def package_cache_hit_claimed(lock: dict[str, Any]) -> bool:
    return os.environ.get(lock["package_cache"]["hit_environment"], "").lower() == "true"


def cached_package_path(
    directory: pathlib.Path, namespace: str, package: str, digest: str
) -> pathlib.Path:
    if (
        not namespace
        or "/" in namespace
        or not package
        or "/" in package
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        raise SupplyChainError("downloaded-package cache identity is invalid")
    return directory / namespace / f"{package}-{digest}.deb"


def verify_deb_archive(
    archive: pathlib.Path,
    *,
    package: str,
    version: str,
    architecture: str,
    digest: str,
) -> None:
    verify_bytes(archive.read_bytes(), digest, f"cached package {package}")
    fields = {
        field: run(["dpkg-deb", "--field", str(archive), field], capture=True)
        for field in ("Package", "Version", "Architecture")
    }
    expected = {
        "Package": package,
        "Version": version,
        "Architecture": architecture,
    }
    if fields != expected:
        raise SupplyChainError(
            f"cached package metadata mismatch: expected {expected}, received {fields}"
        )


def resolve_cached_archive(
    archive: pathlib.Path,
    *,
    package: str,
    version: str,
    architecture: str,
    digest: str,
    cache_hit: bool,
) -> pathlib.Path | None:
    if not archive.is_file():
        if cache_hit:
            raise SupplyChainError(
                f"downloaded-package cache hit omitted locked package: {package}"
            )
        return None
    verify_deb_archive(
        archive,
        package=package,
        version=version,
        architecture=architecture,
        digest=digest,
    )
    return archive


def publish_cached_archive(source: pathlib.Path, target: pathlib.Path) -> pathlib.Path:
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + f".tmp-{os.getpid()}")
    try:
        shutil.copyfile(source, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def write_package_cache_receipt(
    lock: dict[str, Any], profile: str, records: list[dict[str, str]]
) -> None:
    config = lock["package_cache"]
    raw_path = os.environ.get(config["receipt_environment"])
    if not raw_path:
        return
    path = pathlib.Path(raw_path)
    if not path.is_absolute():
        raise SupplyChainError("package-cache receipt path must be absolute")
    authority_digest = package_cache_authority_digest(lock)
    key = os.environ.get(config["key_environment"], "unavailable")
    cache_hit = "hit" if package_cache_hit_claimed(lock) else "miss"
    if path.is_file():
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise SupplyChainError(f"could not read package-cache receipt: {error}") from error
        if (
            document.get("schema") != "cxxlens.ci-package-cache-receipt.v1"
            or document.get("authority_digest") != authority_digest
            or document.get("key") != key
            or document.get("cache_hit") != cache_hit
            or not isinstance(document.get("profiles"), dict)
        ):
            raise SupplyChainError("package-cache receipt binding differs")
    else:
        document = {
            "schema": "cxxlens.ci-package-cache-receipt.v1",
            "authority_digest": authority_digest,
            "key": key,
            "cache_hit": cache_hit,
            "profiles": {},
        }
    canonical_records = sorted(records, key=lambda row: row["package"])
    if profile in document["profiles"] and document["profiles"][profile] != canonical_records:
        raise SupplyChainError(f"package-cache receipt profile differs: {profile}")
    document["profiles"][profile] = canonical_records
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_text(
            json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
'''
    if marker not in text:
        fail("bootstrap helper insertion point is missing")
    text = text.replace(marker, helpers + marker, 1)

    documentation_start = text.index(
        "def install_documentation(lock: dict[str, Any]) -> None:\n"
    )
    install_start = text.index(
        "\ndef install(root: pathlib.Path, profile_name: str) -> None:\n",
        documentation_start,
    )
    documentation_function = r'''def install_documentation(lock: dict[str, Any]) -> None:
    documentation = lock["documentation"]
    cache = package_cache_directory(lock)
    cache_hit = package_cache_hit_claimed(lock)
    archive = cached_package_path(
        cache, "documentation", documentation["package"], documentation["sha256"]
    )
    resolved = resolve_cached_archive(
        archive,
        package=documentation["package"],
        version=documentation["version"],
        architecture=documentation["architecture"],
        digest=documentation["sha256"],
        cache_hit=cache_hit,
    )
    source = "verified-cache"
    if resolved is None:
        content = download(documentation["url"])
        verify_bytes(content, documentation["sha256"], "Doxygen package")
        with tempfile.TemporaryDirectory(prefix="cxxlens-documentation-package-") as temporary:
            candidate = pathlib.Path(temporary) / "doxygen.deb"
            candidate.write_bytes(content)
            verify_deb_archive(
                candidate,
                package=documentation["package"],
                version=documentation["version"],
                architecture=documentation["architecture"],
                digest=documentation["sha256"],
            )
            resolved = publish_cached_archive(candidate, archive)
        source = "verified-download"
    verify_deb_archive(
        resolved,
        package=documentation["package"],
        version=documentation["version"],
        architecture=documentation["architecture"],
        digest=documentation["sha256"],
    )
    if installed_package_version(documentation["package"]) != documentation["version"]:
        run(
            [
                "sudo",
                "apt-get",
                "install",
                "--yes",
                "--no-install-recommends",
                "--no-upgrade",
                str(resolved),
            ]
        )
    actual = installed_package_version(documentation["package"])
    if actual != documentation["version"]:
        raise SupplyChainError(f"installed Doxygen version mismatch: {actual}")
    version = run(["doxygen", "--version"], capture=True)
    if version != documentation["expected_release"]:
        raise SupplyChainError(f"Doxygen release mismatch: {version}")
    write_package_cache_receipt(
        lock,
        "documentation",
        [
            {
                "package": documentation["package"],
                "version": documentation["version"],
                "architecture": documentation["architecture"],
                "package_digest": "sha256:" + documentation["sha256"],
                "source": source,
            }
        ],
    )
'''
    text = text[:documentation_start] + documentation_function + text[install_start:]

    install_function_start = text.index(
        "def install(root: pathlib.Path, profile_name: str) -> None:\n"
    )
    old_body_start = text.index(
        '    signing_key = llvm["signing_key"]\n', install_function_start
    )
    old_body_end = text.index(
        '    for name in llvm["profiles"][profile_name]:\n', old_body_start
    )
    package_install = r'''    members = list(llvm["profiles"][profile_name])
    cache = package_cache_directory(lock)
    cache_hit = package_cache_hit_claimed(lock)

    signing_key = llvm["signing_key"]
    key_content = download(signing_key["url"])
    verify_bytes(key_content, signing_key["sha256"], "LLVM signing key")
    with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-key-") as temporary:
        verify_key(key_content, signing_key["primary_fingerprint"], pathlib.Path(temporary))

    archives: dict[str, pathlib.Path] = {}
    sources: dict[str, str] = {}
    missing: list[str] = []
    for name in members:
        target = cached_package_path(cache, "llvm", name, llvm["package_sha256"][name])
        resolved = resolve_cached_archive(
            target,
            package=name,
            version=llvm["packages"][name],
            architecture=llvm["architecture"],
            digest=llvm["package_sha256"][name],
            cache_hit=cache_hit,
        )
        if resolved is None:
            missing.append(name)
        else:
            archives[name] = resolved
            sources[name] = "verified-cache"

    if missing:
        with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-bootstrap-") as temporary:
            directory = pathlib.Path(temporary)
            keyring = verify_key(
                key_content, signing_key["primary_fingerprint"], directory
            )
            source = directory / "cxxlens-llvm.list"
            source.write_text(
                "deb [arch={architecture} signed-by={keyring}] {repository} "
                "{suite} {component}\n".format(
                    architecture=llvm["architecture"],
                    keyring=KEYRING,
                    repository=llvm["repository"],
                    suite=llvm["suite"],
                    component=llvm["component"],
                ),
                encoding="utf-8",
            )
            run(["sudo", "install", "-D", "-m", "0644", str(keyring), str(KEYRING)])
            run(["sudo", "install", "-D", "-m", "0644", str(source), str(SOURCE_LIST)])
        run(
            [
                "sudo",
                "apt-get",
                "-o",
                f"Dir::Etc::sourcelist={SOURCE_LIST}",
                "-o",
                "Dir::Etc::sourceparts=-",
                "-o",
                "APT::Get::List-Cleanup=0",
                "update",
            ]
        )
        package_requests = [f"{name}={llvm['packages'][name]}" for name in missing]
        with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-packages-") as temporary:
            package_directory = pathlib.Path(temporary)
            run(["apt-get", "download", *package_requests], cwd=package_directory)
            downloaded: dict[str, pathlib.Path] = {}
            for candidate in sorted(package_directory.glob("*.deb")):
                name = run(["dpkg-deb", "--field", str(candidate), "Package"], capture=True)
                if name not in missing or name in downloaded:
                    raise SupplyChainError(
                        f"downloaded LLVM package set is duplicated or unexpected: {candidate.name}"
                    )
                verify_deb_archive(
                    candidate,
                    package=name,
                    version=llvm["packages"][name],
                    architecture=llvm["architecture"],
                    digest=llvm["package_sha256"][name],
                )
                target = cached_package_path(
                    cache, "llvm", name, llvm["package_sha256"][name]
                )
                downloaded[name] = publish_cached_archive(candidate, target)
                sources[name] = "verified-download"
            if set(downloaded) != set(missing):
                raise SupplyChainError("downloaded LLVM package set differs from cache miss set")
            archives.update(downloaded)

    if set(archives) != set(members) or set(sources) != set(members):
        raise SupplyChainError("resolved LLVM package set differs from profile")
    for name, archive in archives.items():
        verify_deb_archive(
            archive,
            package=name,
            version=llvm["packages"][name],
            architecture=llvm["architecture"],
            digest=llvm["package_sha256"][name],
        )
    run(
        [
            "sudo",
            "apt-get",
            "install",
            "--yes",
            "--no-install-recommends",
            "--no-upgrade",
            *[str(archives[name]) for name in members],
        ]
    )
    write_package_cache_receipt(
        lock,
        profile_name,
        [
            {
                "package": name,
                "version": llvm["packages"][name],
                "architecture": llvm["architecture"],
                "package_digest": "sha256:" + llvm["package_sha256"][name],
                "source": sources[name],
            }
            for name in members
        ],
    )
'''
    text = text[:old_body_start] + package_install + text[old_body_end:]
    path.write_text(text, encoding="utf-8")


def update_collector() -> None:
    path = ROOT / "tools/quality/collect_toolchain_provenance.py"
    text = path.read_text(encoding="utf-8")
    marker = "\ndef provenance_digest(document: dict[str, Any]) -> str:\n"
    helper = r'''
def package_cache_authority_digest(lock: dict[str, Any]) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(
            lock["package_cache"], sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    ).hexdigest()


def package_cache_provenance(lock: dict[str, Any]) -> dict[str, Any]:
    config = lock.get("package_cache")
    if not isinstance(config, dict):
        raise ValueError("package-cache authority is missing")
    authority_digest = package_cache_authority_digest(lock)
    raw_path = os.environ.get(config["receipt_environment"])
    if not raw_path:
        return {
            "status": "not-requested",
            "authority_digest": authority_digest,
            "key": "unavailable",
            "cache_hit": "not-requested",
            "profiles": {},
        }
    path = pathlib.Path(raw_path)
    if not path.is_absolute() or not path.is_file():
        raise ValueError("package-cache provenance receipt is unavailable")
    document = json.loads(path.read_text(encoding="utf-8"))
    expected_key = os.environ.get(config["key_environment"], "unavailable")
    expected_hit = (
        "hit"
        if os.environ.get(config["hit_environment"], "").lower() == "true"
        else "miss"
    )
    if (
        document.get("schema") != "cxxlens.ci-package-cache-receipt.v1"
        or document.get("authority_digest") != authority_digest
        or document.get("key") != expected_key
        or document.get("cache_hit") != expected_hit
        or not isinstance(document.get("profiles"), dict)
    ):
        raise ValueError("package-cache provenance receipt binding differs")
    allowed_sources = {"verified-cache", "verified-download"}
    for profile, rows in document["profiles"].items():
        if not isinstance(profile, str) or not isinstance(rows, list) or not rows:
            raise ValueError("package-cache provenance profile is invalid")
        for row in rows:
            if (
                not isinstance(row, dict)
                or row.get("source") not in allowed_sources
                or not isinstance(row.get("package_digest"), str)
                or not row["package_digest"].startswith("sha256:")
            ):
                raise ValueError("package-cache provenance package record is invalid")
    return {
        "status": "verified",
        "authority_digest": authority_digest,
        "key": expected_key,
        "cache_hit": expected_hit,
        "profiles": document["profiles"],
        "receipt_digest": file_digest(path),
    }
'''
    if marker not in text:
        fail("collector insertion point is missing")
    text = text.replace(marker, helper + marker, 1)
    text = replace_once(
        text,
        '        "supply_chain": supply_chain_binding,\n',
        '        "supply_chain": supply_chain_binding,\n'
        '        "package_cache": package_cache_provenance(lock),\n',
        "collector package-cache output",
    )
    path.write_text(text, encoding="utf-8")


def update_checker(lock: dict[str, object]) -> None:
    path = ROOT / "tools/quality/check_ci_supply_chain.py"
    text = path.read_text(encoding="utf-8")
    old = '''    setup_text = (root / SETUP_ACTION).read_text(encoding="utf-8")
    for marker in (
        "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
        "cache: pip",
        "bootstrap_supply_chain.py install",
        "tools/quality/requirements.lock",
    ):
'''
    new = '''    setup_text = (root / SETUP_ACTION).read_text(encoding="utf-8")
    expected_cache = {
        "directory": "~/.cache/cxxlens/packages",
        "environment": "CXXLENS_PACKAGE_CACHE",
        "hit_environment": "CXXLENS_PACKAGE_CACHE_HIT",
        "key_environment": "CXXLENS_PACKAGE_CACHE_KEY",
        "receipt_environment": "CXXLENS_PACKAGE_CACHE_RECEIPT",
        "key_version": "v1",
        "key_template": (
            "cxxlens-ci-packages-v1-${runner.os}-${runner.arch}-"
            "${profile}-${documentation}-${lock_digest}"
        ),
        "scope": "exact-downloaded-debs-only",
        "correctness_role": "transport-optimization-only",
        "restore_keys": False,
    }
    if lock.get("package_cache") != expected_cache:
        raise CiSupplyChainError("downloaded-package cache contract differs")
    for marker in (
        "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
        "actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830",
        "cache: pip",
        "CXXLENS_PACKAGE_CACHE",
        "CXXLENS_PACKAGE_CACHE_KEY",
        "CXXLENS_PACKAGE_CACHE_HIT",
        "CXXLENS_PACKAGE_CACHE_RECEIPT",
        "cxxlens-ci-packages-v1-${{ runner.os }}-${{ runner.arch }}-",
        "${{ inputs.profile }}-${{ inputs.documentation }}-",
        "hashFiles('tools/ci/llvm22-noble.lock.json')",
        "bootstrap_supply_chain.py install",
        "tools/quality/requirements.lock",
    ):
'''
    text = replace_once(text, old, new, "checker setup markers")
    old_tail = '''    if workflow_text.count("collect_toolchain_provenance.py") < 8:
        raise CiSupplyChainError("toolchain provenance is not collected by all evidence jobs")
    collector = (root / "tools/quality/collect_toolchain_provenance.py").read_text(
'''
    new_tail = '''    if "restore-keys:" in setup_text:
        raise CiSupplyChainError("downloaded-package cache must not use fallback restore keys")
    bootstrap_text = (root / "tools/ci/bootstrap_supply_chain.py").read_text(
        encoding="utf-8"
    )
    for marker in (
        "CXXLENS_PACKAGE_CACHE",
        "package_cache_directory",
        "resolve_cached_archive",
        "verify_deb_archive",
        "write_package_cache_receipt",
        "verified-cache",
        "verified-download",
        "[\"apt-get\", \"download\"",
    ):
        if marker not in bootstrap_text:
            raise CiSupplyChainError(
                f"bootstrap lacks exact package-cache binding: {marker}"
            )
    if workflow_text.count("collect_toolchain_provenance.py") < 8:
        raise CiSupplyChainError("toolchain provenance is not collected by all evidence jobs")
    collector = (root / "tools/quality/collect_toolchain_provenance.py").read_text(
'''
    text = replace_once(text, old_tail, new_tail, "checker bootstrap markers")
    old_markers = '''    for marker in (
        "llvm22-noble.lock.json",
        "requirements.lock",
        "ImageVersion",
        "python_distributions",
        'command_identity("doxygen")',
    ):
'''
    new_markers = '''    for marker in (
        "llvm22-noble.lock.json",
        "requirements.lock",
        "ImageVersion",
        "python_distributions",
        'command_identity("doxygen")',
        "package_cache_provenance",
        "package_cache_authority_digest",
        '"package_cache": package_cache_provenance(lock)',
    ):
'''
    text = replace_once(text, old_markers, new_markers, "checker provenance markers")
    path.write_text(text, encoding="utf-8")


def update_tests() -> None:
    path = ROOT / "tests/quality/test_ng_ci_supply_chain.py"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "import json\nimport pathlib\n",
        "import json\nimport os\nimport pathlib\n",
        "test imports",
    )
    text = replace_once(
        text,
        "    install_documentation,\n    load_lock,\n    verify_bytes,\n",
        "    install_documentation,\n    load_lock,\n"
        "    package_cache_authority_digest,\n"
        "    package_cache_directory,\n"
        "    resolve_cached_archive,\n"
        "    verify_bytes,\n"
        "    verify_deb_archive,\n",
        "test bootstrap imports",
    )
    import_marker = "import check_ng_production_scope_closure as production_scope  # noqa: E402\n"
    if import_marker in text:
        fail("unexpected production-scope import in CI supply-chain tests")
    collector_import = '''from collect_toolchain_provenance import (  # noqa: E402
    package_cache_provenance,
)
'''
    insertion = "\n\nclass NgCiSupplyChainTest(unittest.TestCase):\n"
    text = replace_once(
        text,
        insertion,
        "\n" + collector_import + insertion,
        "test collector import",
    )
    methods = r'''
    def test_downloaded_package_cache_is_exact_and_has_no_fallback_key(self) -> None:
        action = (ROOT / ".github/actions/setup-ci/action.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830",
            action,
        )
        self.assertIn("CXXLENS_PACKAGE_CACHE_RECEIPT", action)
        self.assertIn("${{ inputs.profile }}-${{ inputs.documentation }}-", action)
        self.assertIn("hashFiles('tools/ci/llvm22-noble.lock.json')", action)
        self.assertNotIn("restore-keys:", action)
        self.assertEqual(
            self.lock["package_cache"]["correctness_role"],
            "transport-optimization-only",
        )

    def test_relative_downloaded_package_cache_path_is_rejected(self) -> None:
        with mock.patch.dict(
            os.environ, {"CXXLENS_PACKAGE_CACHE": "relative/cache"}, clear=False
        ):
            with self.assertRaisesRegex(SupplyChainError, "must be absolute"):
                package_cache_directory(self.lock)

    def test_verified_cached_package_is_reused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "clang.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_deb_archive") as verify:
                resolved = resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=True,
                )
            self.assertEqual(resolved, archive)
            verify.assert_called_once()

    def test_cache_miss_is_explicit_and_claimed_incomplete_hit_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "missing.deb"
            self.assertIsNone(
                resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=False,
                )
            )
            with self.assertRaisesRegex(SupplyChainError, "omitted locked package"):
                resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=True,
                )

    def test_corrupt_cached_package_is_rejected_before_metadata_probe(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"substituted")
            with mock.patch("bootstrap_supply_chain.run") as run:
                with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version=self.lock["llvm"]["packages"]["clang-22"],
                        architecture=self.lock["llvm"]["architecture"],
                        digest=self.lock["llvm"]["package_sha256"]["clang-22"],
                    )
                run.assert_not_called()

    def test_cached_package_wrong_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_bytes"), mock.patch(
                "bootstrap_supply_chain.run",
                side_effect=["clang-22", "wrong-version", "amd64"],
            ):
                with self.assertRaisesRegex(SupplyChainError, "metadata mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version="expected-version",
                        architecture="amd64",
                        digest="0" * 64,
                    )

    def test_cached_package_wrong_architecture_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_bytes"), mock.patch(
                "bootstrap_supply_chain.run",
                side_effect=["clang-22", "expected-version", "arm64"],
            ):
                with self.assertRaisesRegex(SupplyChainError, "metadata mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version="expected-version",
                        architecture="amd64",
                        digest="0" * 64,
                    )

    def test_package_cache_provenance_binds_verified_source_and_authority(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"
            authority_digest = package_cache_authority_digest(self.lock)
            receipt.write_text(
                json.dumps(
                    {
                        "schema": "cxxlens.ci-package-cache-receipt.v1",
                        "authority_digest": authority_digest,
                        "key": "fixture-key",
                        "cache_hit": "hit",
                        "profiles": {
                            "developer": [
                                {
                                    "package": "clang-22",
                                    "version": self.lock["llvm"]["packages"]["clang-22"],
                                    "architecture": "amd64",
                                    "package_digest": "sha256:"
                                    + self.lock["llvm"]["package_sha256"]["clang-22"],
                                    "source": "verified-cache",
                                }
                            ]
                        },
                    },
                    sort_keys=True,
                ),
                encoding="utf-8",
            )
            environment = {
                "CXXLENS_PACKAGE_CACHE_RECEIPT": str(receipt),
                "CXXLENS_PACKAGE_CACHE_KEY": "fixture-key",
                "CXXLENS_PACKAGE_CACHE_HIT": "true",
            }
            with mock.patch.dict(os.environ, environment, clear=False):
                evidence = package_cache_provenance(self.lock)
            self.assertEqual(evidence["status"], "verified")
            self.assertEqual(evidence["authority_digest"], authority_digest)
            self.assertEqual(
                evidence["profiles"]["developer"][0]["source"],
                "verified-cache",
            )

    def test_stale_package_cache_provenance_authority_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"
            receipt.write_text(
                json.dumps(
                    {
                        "schema": "cxxlens.ci-package-cache-receipt.v1",
                        "authority_digest": "sha256:" + "0" * 64,
                        "key": "fixture-key",
                        "cache_hit": "miss",
                        "profiles": {"developer": [{"source": "verified-download"}]},
                    }
                ),
                encoding="utf-8",
            )
            environment = {
                "CXXLENS_PACKAGE_CACHE_RECEIPT": str(receipt),
                "CXXLENS_PACKAGE_CACHE_KEY": "fixture-key",
                "CXXLENS_PACKAGE_CACHE_HIT": "false",
            }
            with mock.patch.dict(os.environ, environment, clear=False):
                with self.assertRaisesRegex(ValueError, "binding differs"):
                    package_cache_provenance(self.lock)

'''
    text = replace_once(
        text,
        '\n\nif __name__ == "__main__":\n',
        "\n" + methods + '\nif __name__ == "__main__":\n',
        "test methods",
    )
    path.write_text(text, encoding="utf-8")


def update_contributing() -> None:
    path = ROOT / "CONTRIBUTING.md"
    text = path.read_text(encoding="utf-8")
    old = (
        "`setup-python` の pip cache は反復高速化に使いますが、cache は `full` / `stress` の\n"
        "correctness evidence ではありません。\n"
    )
    new = (
        "`setup-python` の pip wheel cache と、lock digest・runner OS/arch・profile に完全一致する LLVM/Doxygen `.deb` cache を\n"
        "反復高速化に使います。cache hit でも package SHA-256、name、exact epoch-qualified version、architecture と LLVM\n"
        "signing-key fingerprint を再検証し、fallback restore key は使用しません。verified cache/download の別は provenance\n"
        "receipt に残します。cache は `full` / `stress` の correctness evidence ではありません。\n"
    )
    path.write_text(
        replace_once(text, old, new, "contributing cache policy"),
        encoding="utf-8",
    )


def apply() -> None:
    update_setup_action()
    lock = update_lock()
    update_bootstrap()
    update_collector()
    update_checker(lock)
    update_tests()
    update_contributing()
    print(
        json.dumps(
            {
                "package_cache_authority_digest": "sha256:"
                + canonical_digest(lock["package_cache"]),
                "setup_action_digest": hashlib.sha256(
                    (ROOT / ".github/actions/setup-ci/action.yml").read_bytes()
                ).hexdigest(),
            },
            sort_keys=True,
        )
    )


def git_output(*arguments: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(ROOT), *arguments], text=True
    ).strip()


def export(output: pathlib.Path) -> None:
    revision = git_output("rev-parse", "HEAD")
    parent = git_output("rev-parse", "HEAD^")
    expected_tree = git_output("rev-parse", "HEAD^{tree}")
    changed = subprocess.check_output(
        [
            "git",
            "-C",
            str(ROOT),
            "diff",
            "--no-renames",
            "--name-status",
            "-z",
            parent,
            "HEAD",
        ]
    ).split(b"\0")
    entries: list[dict[str, object]] = []
    index = 0
    while index < len(changed) and changed[index]:
        status = changed[index].decode("utf-8")
        relative = changed[index + 1].decode("utf-8")
        index += 2
        pure = pathlib.PurePosixPath(relative)
        if pure.is_absolute() or ".." in pure.parts or pure.as_posix() != relative:
            fail(f"noncanonical exported path: {relative}")
        row: dict[str, object] = {"path": relative, "status": status[0]}
        if status[0] != "D":
            tree_row = git_output("ls-tree", "HEAD", "--", relative)
            if not tree_row:
                fail(f"tree entry missing for {relative}")
            content = (ROOT / relative).read_bytes()
            row["mode"] = tree_row.split()[0]
            row["content_base64"] = base64.b64encode(content).decode("ascii")
        entries.append(row)
    output.write_text(
        json.dumps(
            {
                "schema": "cxxlens.issue-316-package-cache-tree.v1",
                "parent": parent,
                "generated_revision": revision,
                "expected_tree": expected_tree,
                "entries": entries,
            },
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "parent": parent,
                "expected_tree": expected_tree,
                "entry_count": len(entries),
            },
            sort_keys=True,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("apply")
    export_parser = subparsers.add_parser("export")
    export_parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.command == "apply":
        apply()
    else:
        export(arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
