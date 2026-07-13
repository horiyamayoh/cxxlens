# Complete API executable test coverage

`schemas/cxxlens_public_api_contract.yaml` の `implementation_state: conformant` かつ
`readiness.state: complete` は、実装ファイルが存在するという自己申告ではない。次の4つが一致した場合に
のみ complete として認証する。

1. catalog の exact declaration と signature fingerprint
2. `schemas/cxxlens_api_test_coverage.yaml` の API 単位 test evidence
3. M0/M1/M2 completion manifest の exactly-once vector membership
4. CTest に登録された executable unit/acceptance case と存在する fixture/source

各 API coverage row は `tests/unit/` の direct unit test、public header または installed consumer 経由の
acceptance test、全 declaration member index、全 use-case/requirement、期待結果を持つ。family member index は
exact signature の `;` 区切り順であり、一部だけを認証できない。acceptance test は unit test と別の CTest
IDで、catalog の use-case ID を test registry 上でも明示的に被覆する。

Positive、negative、ambiguous は category 名だけでは証拠にならない。各 category は API 固有 case ID、
実行する CTest ID、単一の evidence path、期待結果を持つ。未実装 API の task packet は空の category 要件を
保持して blocked のままとなり、concrete evidence が揃うまで ready/complete へ遷移できない。

次のコマンドは manifest と report を catalog から再生成する。通常の検証では `check` を使用し、drift を
自動修復しない。

```sh
python3 tools/quality/check_api_test_coverage.py generate --root .
python3 tools/quality/check_api_test_coverage.py check --root .
python3 tests/quality/test_api_test_coverage.py
```

`schemas/cxxlens_api_test_coverage_report.json` は complete API、declaration member、declared use-case の
認証数と未被覆集合を記録する。完了条件は API 47/47、member 162/162、use-case 3/3、全 uncovered 配列が
空であること。API、signature、test binding、completion vector の変更では manifest/report と negative
fixtures を同時に更新する。
