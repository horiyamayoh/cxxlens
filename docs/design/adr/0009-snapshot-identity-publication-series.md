# ADR 0009: snapshot identity DAG と publication series を分離する

- Status: Accepted
- Date: 2026-07-16
- Decision owner: store-kernel
- Decision issue: #63
- Tracking issue: #56

## Context

従来案では closure certificate が `input_snapshot` を持つ一方、snapshot manifest が closure ID を
含んでいた。この形では、certificate が「それを収容する snapshot」を入力として誤参照した場合に
identity cycle が生じる。また `current(catalog_id)` は、channel、engine generation、condition universe、
interpretation/trust policy が異なる複数の publication series を区別できない。

direct observation に snapshot input を強制すること、partition content と closure subject の binding が
弱いこと、compaction/format migration/corruption 時の reader pin と prior publication の扱いが未定義なことも、
immutable store を実装する前に解消する必要がある。

## Decision

identity は `cxxlens-canonical-tuple-v1` の versioned length-prefixed binary tuple を SHA-256 で hash する。
domain tag と identity contract version を必ず prefix に含め、256 bit digest を切り詰めず保存する。digest
衝突を検出した場合は candidate を quarantine し、既存 object を保持して structured failure を返す。

semantic identity graph は次の DAG とする。

```text
semantic key -> assertion -> claim content
                               |
                               v
coverage -> claim set -> partition content
                        |             |
                        +------> closure certificate
                                      |
catalog/registry/policy + partition manifests + closure IDs -> snapshot
snapshot + exact series selector -> publication record
```

partition manifest の semantic projection は closure ID を含まない。closure certificate は relation
descriptor、subject partition ID、partition content digest、coverage digest、key domain、condition、
interpretation、assumptions、kind、producer semantics、evidence digest に bind する。certificate は containing
snapshot ID と input snapshot ID を持たない。snapshot は canonical sorted partition manifest と canonical
sorted closure certificate ID を含むため、逆向き edge は存在しない。

claim の producer input は tagged basis とする。`direct` は source/invocation/toolchain 等をまとめた
`basis_digest` を持ち、snapshot を持たない。`derived` は strict-prior `input_snapshot`、canonical sorted
consumed partition content digests、transform semantics を持つ。containing snapshot はどちらにも許可しない。

`current` の authority は次の exact selector とする。

```text
catalog ID
channel ID
engine generation ID
condition universe ID
relation registry digest
interpretation policy digest
trust policy digest
```

この tuple から `snapshot_series_id` を作り、store は `current(snapshot_series_selector)` を提供する。
catalog-only lookup、selector の silent default、series を跨ぐ newest/first-wins は禁止する。

publication は copy-on-write transaction と compare-and-swap series head を使う。staged/validating object は
reader に見せず、失敗時は head を変更しない。reader handle は publication の物理 generation を pin する。
compaction と format migration は別 generation に書き、semantic digest を再検証してから locator を atomically
swap する。全 publication に同じ generation を付けず、prior resolver order で distinct な checked generation を割り当てる。SQLite は write
transaction 内の full committed authority censusを replacement set として一 commit で置換し、memory/SQLite とも resolver order を保存する。
pinned generation は handle 解放まで回収しない。current head の corruption は prior head への silent
fallback を行わず structured failure とし、明示的に開いた intact prior publication は引き続き読める。

Issue #131 により semantic snapshot ID の `open` も publication record 集合を authority とする。同じ snapshot ID の
committed record を publication sequence、physical generation の順で解決してから corrupt state と decoded payload の exact bindingを
検査する。選択された最新 record が corrupt または payload unavailable なら `store.snapshot-corrupt` とし、古い publicationへ
fallbackしない。同じ sequence/generation の異 publication ID が候補なら `store.snapshot-ambiguous` とする。`canonical_export`、query bind、derived basis lookup はこの checked resolver または exact
`open_publication` を使用する。staged/rejected/rolled-back record は semantic open の候補にしない。

snapshot format は semantic snapshot ID に含めない。exact-compatible reader または登録済み deterministic
migration chain だけが open でき、migration 後も semantic digest が一致しなければ commit しない。

## Consequences

- 同じ semantic content は memory/SQLite、root、job 数、arrival order、physical format に依存せず同じ
  snapshot ID になる。
- lineage、publication sequence、created time、writer、physical locator は operational record に隔離される。
- derived claim は prior snapshot を参照できるが、収容先 snapshot を参照できない。
- `snapshot_store::current(catalog_id)` という旧 signature は次世代 API に採用しない。
- hash algorithm の変更は identity contract major の変更であり、silent rehash は許可しない。

## Verification

`tools/quality/check_ng_snapshot_store_contract.py` が binary canonical encoding、identity DAG、claim input basis、
closure binding、snapshot/series digest、publication transaction、reader pin、compaction、format migration、
corruption/collision policyを検査する。conformance vector は root/jobs/order/backend perturbation、cycle、
containing snapshot、incomplete selector、failed publish、pinned compaction、corrupt head、format mismatch を含む。
