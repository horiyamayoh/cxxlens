#!/usr/bin/env python3
"""Apply the exact cache-hit dependency-resolution fix for PR #317."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]


def patch_bootstrap() -> None:
    path = ROOT / "tools/ci/bootstrap_supply_chain.py"
    text = path.read_text(encoding="utf-8")

    helper_marker = "\ndef package_cache_authority_digest(lock: dict[str, Any]) -> str:\n"
    helper = '''

def configure_llvm_repository(llvm: dict[str, Any], key_content: bytes) -> None:
    """Install the verified LLVM source and refresh its exact package index."""
    signing_key = llvm["signing_key"]
    with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-bootstrap-") as temporary:
        directory = pathlib.Path(temporary)
        keyring = verify_key(
            key_content, signing_key["primary_fingerprint"], directory
        )
        source = directory / "cxxlens-llvm.list"
        source.write_text(
            "deb [arch={architecture} signed-by={keyring}] {repository} "
            "{suite} {component}\\n".format(
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
'''
    if "def configure_llvm_repository(" not in text:
        if text.count(helper_marker) != 1:
            raise RuntimeError("LLVM repository helper insertion marker differs")
        text = text.replace(helper_marker, helper + helper_marker, 1)

    old_verify = '''    with tempfile.TemporaryDirectory(prefix="cxxlens-llvm-key-") as temporary:
        verify_key(key_content, signing_key["primary_fingerprint"], pathlib.Path(temporary))
'''
    new_verify = "    configure_llvm_repository(llvm, key_content)\n"
    if old_verify in text:
        text = text.replace(old_verify, new_verify, 1)
    elif new_verify not in text:
        raise RuntimeError("LLVM repository configuration call marker differs")

    block_marker = (
        "    if missing:\n"
        "        with tempfile.TemporaryDirectory(prefix=\"cxxlens-llvm-bootstrap-\") as temporary:\n"
    )
    if block_marker in text:
        block_start = text.index(block_marker)
        package_line = text.index(
            "        package_requests = [f\"{name}={llvm['packages'][name]}\" for name in missing]\n",
            block_start,
        )
        text = text[: block_start + len("    if missing:\n")] + text[package_line:]
    path.write_text(text, encoding="utf-8")


def patch_tests() -> None:
    path = ROOT / "tests/quality/test_ng_ci_supply_chain.py"
    text = path.read_text(encoding="utf-8")
    import_marker = "    install_documentation,\n"
    if "    install,\n" not in text:
        if text.count(import_marker) != 1:
            raise RuntimeError("test import marker differs")
        text = text.replace(import_marker, "    install,\n" + import_marker, 1)

    test_marker = "    def test_verified_cached_package_is_reused(self) -> None:\n"
    regression = '''    def test_cache_hit_configures_repository_before_archive_resolution(self) -> None:
        lock = copy.deepcopy(self.lock)
        events = mock.Mock()
        with mock.patch("bootstrap_supply_chain.load_lock", return_value=lock), \\
             mock.patch("bootstrap_supply_chain.assert_runner"), \\
             mock.patch("bootstrap_supply_chain.download", return_value=b"verified-key"), \\
             mock.patch("bootstrap_supply_chain.verify_bytes"), \\
             mock.patch("bootstrap_supply_chain.configure_llvm_repository") as configure, \\
             mock.patch("bootstrap_supply_chain.package_cache_directory", return_value=pathlib.Path("/tmp/cache")), \\
             mock.patch("bootstrap_supply_chain.package_cache_hit_claimed", return_value=True), \\
             mock.patch("bootstrap_supply_chain.resolve_cached_archive", side_effect=RuntimeError("archive-resolution")) as resolve:
            events.attach_mock(configure, "configure")
            events.attach_mock(resolve, "resolve")
            with self.assertRaisesRegex(RuntimeError, "archive-resolution"):
                install(ROOT, "compiler")
        self.assertEqual(
            events.mock_calls[0],
            mock.call.configure(lock["llvm"], b"verified-key"),
        )

'''
    if "test_cache_hit_configures_repository_before_archive_resolution" not in text:
        if text.count(test_marker) != 1:
            raise RuntimeError("cache regression test insertion marker differs")
        text = text.replace(test_marker, regression + test_marker, 1)
    path.write_text(text, encoding="utf-8")


def main() -> None:
    patch_bootstrap()
    patch_tests()
    pathlib.Path(__file__).unlink()


if __name__ == "__main__":
    main()
