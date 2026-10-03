#!/usr/bin/env python3
"""Exercise the installed doctor's ordinary capability and input diagnostics."""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

DOCTOR = pathlib.Path(sys.argv.pop(1)).resolve()


class DoctorTest(unittest.TestCase):
    def call(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(DOCTOR), *args], capture_output=True, text=True, check=False)

    def test_registered_and_missing_relations(self) -> None:
        found = self.call('relation-presence', 'cc.entity.v1', 'cc.call_site.v1')
        self.assertEqual(found.returncode, 0, found.stderr)
        document = json.loads(found.stdout)
        self.assertEqual(document['state'], 'proved')
        missing = self.call('relation-presence', 'cc.missing.v1')
        self.assertEqual(missing.returncode, 1, missing.stderr)
        self.assertEqual(json.loads(missing.stdout)['state'], 'unknown')

    def test_invalid_requests_fail_before_output(self) -> None:
        for args in [(), ('relation-presence',), ('relation-presence', 'cc.entity.v1', 'cc.entity.v1'),
                     ('relation-presence', 'bad'), ('relation-presence', 'cc.entity.v1', '--format', 'xml')]:
            result = self.call(*args)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(result.stdout, '')

    def test_relation_order_and_markdown(self) -> None:
        first = self.call('relation-presence', 'cc.entity.v1', 'cc.call_site.v1')
        second = self.call('relation-presence', 'cc.call_site.v1', 'cc.entity.v1')
        self.assertEqual(first.stdout, second.stdout)
        markdown = self.call('relation-presence', 'cc.entity.v1', '--format', 'markdown')
        self.assertEqual(markdown.returncode, 0, markdown.stderr)
        self.assertIn('cc.entity.v1', markdown.stdout)

    def test_missing_inputs_have_actionable_diagnostics(self) -> None:
        digest = 'semantic-v2:sha256:' + '1' * 64
        project = {'schema': 'cxxlens.sdk-doctor-project.v2', 'document_version': '2.0.0',
                   'project': {'project_id': 'project:example', 'catalog_id': 'catalog:' + digest,
                               'catalog_digest': digest, 'logical_root': 'project://example',
                               'environment_digest': 'sha256:' + '2' * 64,
                               'environment': {'release_version': '2.0.0', 'surface': 'provider-sdk',
                                               'os': 'linux', 'architecture': 'x86_64',
                                               'compiler_provider_major': 'clang22', 'linkage': 'static'},
                               'provider_candidates': []}}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'project.json'
            path.write_text(json.dumps(project), encoding='utf-8')
            result = self.call('missing', '--project', str(path), '--use-case', 'cxxlens.clang22.materialize-and-query.v1')
            self.assertEqual(result.returncode, 1, result.stderr)
            document = json.loads(result.stdout)
            self.assertIn(document['result']['state'], ('unknown', 'partial'))
            self.assertTrue(document['missing'])
            self.assertTrue(document['completion_plan'])
            path.write_text('{"schema":"first","schema":"second"}', encoding='utf-8')
            duplicate = self.call('missing', '--project', str(path), '--use-case', 'cxxlens.clang22.materialize-and-query.v1')
            self.assertEqual(duplicate.returncode, 2)
            self.assertEqual(duplicate.stdout, '')


if __name__ == '__main__':
    unittest.main()
