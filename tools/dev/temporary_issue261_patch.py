#!/usr/bin/env python3
# Temporary exact patch driver for the two issue #261 P1 findings.

from __future__ import annotations

import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/quality/check_ng_source_closure_transport.py"
TESTS = ROOT / "tests/quality/test_ng_source_closure_transport.py"
CONTRACT = ROOT / "schemas/cxxlens_ng_source_closure_transport.yaml"
CONTRACT_SCHEMA = ROOT / "schemas/cxxlens_ng_source_closure_transport.schema.yaml"
WORK_UNITS = ROOT / "schemas/cxxlens_ng_work_units.yaml"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def lines(*values: str) -> str:
    return "\n".join(values) + "\n"


def patch_checker() -> None:
    text = CHECKER.read_text(encoding="utf-8")

    anchor = lines(
        "class TransferStateWitness:",
        '    """Executable exact-binding witness for one manifest and its canonical blob stream."""',
        "",
        "    def __init__(self, *, session_id: str, task_id: str, task_v4_digest: str,",
    )
    replacement = lines(
        "class TransferStateWitness:",
        '    """Executable exact-binding witness for one manifest and its canonical blob stream."""',
        "",
        "    @classmethod",
        "    def for_task_extension(",
        "        cls,",
        "        *,",
        "        session_id: str,",
        "        task_extension: dict[str, Any],",
        "        manifest_schema: dict[str, Any],",
        '    ) -> "TransferStateWitness":',
        '        """Bind a transfer to the already-sealed outer task-v4 extension."""',
        "",
        "        if not isinstance(task_extension, dict):",
        "            raise SourceClosureTransportError(",
        '                "outer task-v4 extension transfer binding is missing"',
        "            )",
        '        task_id = task_extension.get("task_id")',
        '        task_v4_digest = task_extension.get("task_v4_digest")',
        '        source_closure = task_extension.get("source_closure")',
        "        if not isinstance(source_closure, dict):",
        "            raise SourceClosureTransportError(",
        '                "outer task-v4 extension source closure binding is missing"',
        "            )",
        '        closure_id = source_closure.get("id")',
        '        closure_digest = source_closure.get("digest")',
        '        sealed_manifest_digest = source_closure.get("manifest_digest")',
        '        validate_wire_id("task_id", task_id)',
        "        if (",
        "            not isinstance(task_v4_digest, str)",
        '            or re.fullmatch(r"semantic-v2:sha256:[0-9a-f]{64}", task_v4_digest)',
        "            is None",
        '            or task_id != "task:" + task_v4_digest',
        "            or not isinstance(closure_id, str)",
        "            or re.fullmatch(",
        '                r"source-closure:semantic-v2:sha256:[0-9a-f]{64}",',
        "                closure_id,",
        "            )",
        "            is None",
        "            or not isinstance(closure_digest, str)",
        "            or re.fullmatch(",
        '                r"semantic-v2:sha256:[0-9a-f]{64}", closure_digest',
        "            )",
        "            is None",
        '            or closure_id != "source-closure:" + closure_digest',
        "            or not isinstance(sealed_manifest_digest, str)",
        "            or re.fullmatch(",
        '                r"semantic-v2:sha256:[0-9a-f]{64}",',
        "                sealed_manifest_digest,",
        "            )",
        "            is None",
        "        ):",
        "            raise SourceClosureTransportError(",
        '                "outer task-v4 extension transfer identity is invalid"',
        "            )",
        "        return cls(",
        "            session_id=session_id,",
        "            task_id=task_id,",
        "            task_v4_digest=task_v4_digest,",
        "            closure_id=closure_id,",
        "            closure_digest=closure_digest,",
        "            manifest_digest=sealed_manifest_digest,",
        "            manifest_schema=manifest_schema,",
        "        )",
        "",
        "    def __init__(self, *, session_id: str, task_id: str, task_v4_digest: str,",
    )
    text = replace_once(
        text, anchor, replacement, "TransferStateWitness task-extension factory"
    )

    anchor = lines(
        '            f"complete request 2.2 constructibility witness failed: {message}"',
        "        ) from error",
        '    adr = (root / ADR).read_text(encoding="utf-8")',
    )
    replacement = lines(
        '            f"complete request 2.2 constructibility witness failed: {message}"',
        "        ) from error",
        "",
        '    outer_extension = witness["task_extensions"][0]',
        "    outer_manifest_bytes = canonical_json(witness_manifest)",
        "    outer_descriptor = {",
        '        "kind": "descriptor",',
        '        "session_id": "provider-session:sha256:" + "4" * 64,',
        '        "task_id": outer_extension["task_id"],',
        '        "task_v4_digest": outer_extension["task_v4_digest"],',
        '        "closure_id": outer_extension["source_closure"]["id"],',
        '        "closure_digest": outer_extension["source_closure"]["digest"],',
        '        "manifest_digest": outer_extension["source_closure"]["manifest_digest"],',
        '        "total_bytes": len(outer_manifest_bytes),',
        '        "chunk_bytes": len(outer_manifest_bytes),',
        '        "chunk_count": 1,',
        "    }",
        "    outer_transfer = TransferStateWitness.for_task_extension(",
        '        session_id=outer_descriptor["session_id"],',
        "        task_extension=outer_extension,",
        "        manifest_schema=manifest_schema,",
        "    )",
        "    outer_transfer.apply(",
        '        "source_closure_manifest", outer_descriptor, b"", contract',
        "    )",
        '    if outer_transfer.state != "manifest-open":',
        "        raise SourceClosureTransportError(",
        '            "outer task-v4 extension did not admit its exact transfer descriptor"',
        "        )",
        "    foreign_descriptor = dict(outer_descriptor)",
        '    foreign_descriptor["task_id"] = (',
        '        "task:semantic-v2:sha256:" + "f" * 64',
        "    )",
        "    foreign_transfer = TransferStateWitness.for_task_extension(",
        '        session_id=outer_descriptor["session_id"],',
        "        task_extension=outer_extension,",
        "        manifest_schema=manifest_schema,",
        "    )",
        "    try:",
        "        foreign_transfer.apply(",
        '            "source_closure_manifest", foreign_descriptor, b"", contract',
        "        )",
        "    except SourceClosureTransportError as error:",
        '        if "identity binding mismatch" not in str(error):',
        "            raise",
        "    else:",
        "        raise SourceClosureTransportError(",
        '            "wire transfer accepted cross-task rebinding"',
        "        )",
        '    adr = (root / ADR).read_text(encoding="utf-8")',
    )
    text = replace_once(text, anchor, replacement, "request/transfer executable join")

    anchor = lines(
        "    transfer_witness = TransferStateWitness(",
        "        session_id=session_witness,",
        "        task_id=task_witness,",
        "        task_v4_digest=semantic_witness,",
        '        closure_id="source-closure:" + semantic,',
        "        closure_digest=semantic,",
        "        manifest_digest=manifest_witness,",
        "        manifest_schema=manifest_schema,",
        "    )",
    )
    replacement = lines(
        "    transfer_witness = TransferStateWitness.for_task_extension(",
        "        session_id=session_witness,",
        "        task_extension={",
        '            "task_id": task_witness,',
        '            "task_v4_digest": semantic_witness,',
        '            "source_closure": {',
        '                "id": "source-closure:" + semantic,',
        '                "digest": semantic,',
        '                "manifest_digest": manifest_witness,',
        "            },",
        "        },",
        "        manifest_schema=manifest_schema,",
        "    )",
    )
    text = replace_once(
        text, anchor, replacement, "complete wire witness outer task binding"
    )

    anchor = lines(
        '    if identity.get("manifest", {}).get("exact_fields") != [',
        '        "schema", "closure_id", "closure_digest", "members", "blobs"',
        '    ] or identity.get("blob_receipts", {}).get("digest") != (',
        '        "semantic-digest-of-canonical-complete-receipt-array-streamed"',
        "    ):",
        '        raise SourceClosureTransportError("manifest or bounded seal projection drift")',
    )
    replacement = anchor + lines(
        '    if contract["request_task_binding"].get("wire_transfer_identity") != (',
        '        "exact-outer-task-extension-task-id-task-v4-digest-and-source-closure-reference-before-first-transfer-frame"',
        "    ):",
        "        raise SourceClosureTransportError(",
        '            "outer task-v4 extension/wire transfer binding drift"',
        "        )",
    )
    text = replace_once(
        text, anchor, replacement, "authority wire-transfer binding assertion"
    )
    CHECKER.write_text(text, encoding="utf-8")


def patch_tests() -> None:
    text = TESTS.read_text(encoding="utf-8")
    anchor = lines(
        "    def test_transfer_state_witness_rejects_foreign_gap_and_zero_manifest(self) -> None:"
    )
    replacement = lines(
        "    def test_transfer_state_witness_binds_outer_task_before_first_frame(self) -> None:",
        '        contract = yaml.safe_load((ROOT / CONTRACT).read_text(encoding="utf-8"))',
        "        schema = yaml.safe_load(",
        '            (ROOT / MANIFEST_SCHEMA).read_text(encoding="utf-8")',
        "        )",
        "        request = self.bound_request()",
        "        manifest = self.bind_manifest(request)",
        '        extension = request["task_extensions"][0]',
        "        payload = canonical_json(manifest)",
        "        descriptor = {",
        '            "kind": "descriptor",',
        '            "session_id": SESSION_ID,',
        '            "task_id": extension["task_id"],',
        '            "task_v4_digest": extension["task_v4_digest"],',
        '            "closure_id": extension["source_closure"]["id"],',
        '            "closure_digest": extension["source_closure"]["digest"],',
        '            "manifest_digest": extension["source_closure"]["manifest_digest"],',
        '            "total_bytes": len(payload),',
        '            "chunk_bytes": len(payload),',
        '            "chunk_count": 1,',
        "        }",
        "        exact = TransferStateWitness.for_task_extension(",
        "            session_id=SESSION_ID,",
        "            task_extension=extension,",
        "            manifest_schema=schema,",
        "        )",
        '        exact.apply("source_closure_manifest", descriptor, b"", contract)',
        '        self.assertEqual(exact.state, "manifest-open")',
        "",
        "        foreign = dict(descriptor)",
        '        foreign["task_id"] = "task:semantic-v2:sha256:" + "f" * 64',
        "        rebound = TransferStateWitness.for_task_extension(",
        "            session_id=SESSION_ID,",
        "            task_extension=extension,",
        "            manifest_schema=schema,",
        "        )",
        "        with self.assertRaisesRegex(",
        '            SourceClosureTransportError, "identity binding mismatch"',
        "        ):",
        "            rebound.apply(",
        '                "source_closure_manifest", foreign, b"", contract',
        "            )",
        "",
        "    def test_transfer_state_witness_rejects_foreign_gap_and_zero_manifest(self) -> None:",
    )
    text = replace_once(
        text, anchor, replacement, "cross-task rebinding regression test"
    )
    TESTS.write_text(text, encoding="utf-8")


def patch_contracts() -> None:
    text = CONTRACT.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "document_version: 1.1.0",
        "document_version: 1.1.1",
        "transport contract patch version",
    )
    text = replace_once(
        text,
        "  task_reference: resolves-exactly-one-request-closure\n",
        "  task_reference: resolves-exactly-one-request-closure\n"
        "  wire_transfer_identity: exact-outer-task-extension-task-id-task-v4-digest-and-source-closure-reference-before-first-transfer-frame\n",
        "normative request-to-wire identity binding",
    )
    text = replace_once(
        text,
        "counterexamples:\n",
        "counterexamples:\n"
        "  - valid-transfer-rebound-to-different-outer-task\n",
        "cross-task rebinding counterexample",
    )
    CONTRACT.write_text(text, encoding="utf-8")

    text = CONTRACT_SCHEMA.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "document_version: {const: 1.1.0}",
        "document_version: {const: 1.1.1}",
        "transport schema patch version",
    )
    text = replace_once(
        text,
        "task_reference: resolves-exactly-one-request-closure, base_task_v3_digest:",
        "task_reference: resolves-exactly-one-request-closure, "
        "wire_transfer_identity: exact-outer-task-extension-task-id-task-v4-digest-and-source-closure-reference-before-first-transfer-frame, "
        "base_task_v3_digest:",
        "transport schema request-to-wire binding",
    )
    CONTRACT_SCHEMA.write_text(text, encoding="utf-8")


def patch_work_units() -> None:
    text = WORK_UNITS.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "schemas/cxxlens_ng_source_closure_transport.yaml, schemas/cxxlens_ng_provider_protocol.yaml",
        "schemas/cxxlens_ng_source_closure_transport.yaml, "
        "schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml, "
        "schemas/cxxlens_ng_provider_protocol.yaml",
        "request v2.2 authority source ownership",
    )
    text = replace_once(
        text,
        "schemas/cxxlens_ng_provider_protocol.schema.yaml, schemas/cxxlens_ng_provider_task_v4.schema.yaml",
        "schemas/cxxlens_ng_provider_protocol.schema.yaml, "
        "schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml, "
        "schemas/cxxlens_ng_provider_task_v4.schema.yaml",
        "request v2.2 owned path",
    )
    WORK_UNITS.write_text(text, encoding="utf-8")

    digests = subprocess.check_output(
        [
            "python3",
            "tools/quality/check_ng_work_units.py",
            "digests",
            "--root",
            ".",
        ],
        cwd=ROOT,
        text=True,
    )
    match = re.search(r"^#261 (sha256:[0-9a-f]{64})$", digests, re.MULTILINE)
    if match is None:
        raise RuntimeError("could not derive #261 authority digest")

    text = WORK_UNITS.read_text(encoding="utf-8")
    issue_start = text.index("  - issue: '#261'")
    issue_end = text.index("\n  - issue:", issue_start + 1)
    block = text[issue_start:issue_end]
    block, count = re.subn(
        r"authority_digest: sha256:[0-9a-f]{64}",
        "authority_digest: " + match.group(1),
        block,
        count=1,
    )
    if count != 1:
        raise RuntimeError("could not replace #261 authority digest")
    WORK_UNITS.write_text(
        text[:issue_start] + block + text[issue_end:], encoding="utf-8"
    )


def main() -> None:
    patch_checker()
    patch_tests()
    patch_contracts()
    patch_work_units()
    subprocess.run(
        [
            "python3",
            "tools/quality/verify_checksums.py",
            "generate",
            "--root",
            ".",
        ],
        cwd=ROOT,
        check=True,
    )


if __name__ == "__main__":
    main()
