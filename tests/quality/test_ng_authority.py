#!/usr/bin/env python3
"""Positive and fail-closed tests for the NG authority transition."""

from __future__ import annotations

import copy
import pathlib
import sys
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_authority import (  # noqa: E402
    AuthorityError,
    api_signatures,
    load_yaml,
    schema_validate,
    validate,
    validate_baseline,
)


class NgAuthorityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.baseline = load_yaml(ROOT / "schemas/cxxlens_legacy_api_baseline.yaml")
        cls.catalog = load_yaml(ROOT / "schemas/cxxlens_public_api_contract.yaml")
        cls.freeze_schema = load_yaml(
            ROOT / "schemas/cxxlens_public_api_contract_freeze.schema.yaml"
        )
        cls.freeze = load_yaml(ROOT / "schemas/cxxlens_public_api_contract_freeze.yaml")

    def test_transition_is_active_and_legacy_dispatch_is_revoked(self) -> None:
        transition, baseline = validate(ROOT)
        self.assertEqual(transition["state"], "active")
        self.assertFalse(transition["dispatch"]["legacy_phase_c_authorized"])
        self.assertEqual(baseline["catalog"]["apis"], 124)

    def test_legacy_api_addition_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["packages"][0]["apis"].pop()
        self.assertNotEqual(
            api_signatures(catalog),
            self.baseline["catalog"]["signatures"],
        )

    def test_legacy_signature_change_is_rejected(self) -> None:
        catalog = copy.deepcopy(self.catalog)
        catalog["packages"][0]["apis"][0]["declaration"]["signature_fingerprint"] = (
            "sha256:" + "0" * 64
        )
        original_load = load_yaml

        def load_with_changed_catalog(path: pathlib.Path) -> dict:
            if path.name == "cxxlens_public_api_contract.yaml":
                return catalog
            return original_load(path)

        with mock.patch(
            "check_ng_authority.load_yaml", side_effect=load_with_changed_catalog
        ):
            with self.assertRaisesRegex(AuthorityError, "ID or signature"):
                validate_baseline(ROOT, self.baseline)

    def test_old_phase_c_authorization_is_schema_rejected(self) -> None:
        freeze = copy.deepcopy(self.freeze)
        freeze["phase_c_authorized"] = True
        with self.assertRaisesRegex(AuthorityError, "schema validation"):
            schema_validate(freeze, self.freeze_schema, "legacy API freeze")


if __name__ == "__main__":
    unittest.main()
