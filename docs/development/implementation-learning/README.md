# Implementation learning and design feedback

この文書は、実装で初めて得られた知見を失わず、未検証のメモを authority にもしないための開発 workflow です。

## Authority boundary

次の三層を混同しません。

1. 統合設計、machine-readable contract、accepted ADR は normative authority です。明示的に置換されるまで従います。
2. [Mental models](mental-models/authority-and-learning-loop.md) は non-normative な説明です。理解を助けますが、契約を上書きしません。
3. [Design feedback records](records/README.md) は observation から disposition までを保持する non-normative な履歴です。

「文書より良いと思う」ことは silent deviation の許可ではありません。material な疑義を evidence とともに記録し、必要なら対象実装を
止め、ADR/contract を先に改訂してから実装を合わせます。

## Reading workflow

implementation issue の開始時に、通常の authority を読んだ後で次を確認します。

1. 触る subsystem と contract ID に対応する curated mental model
2. record index の `observed`、`investigating`、`proposed`、`deferred`
3. `scope` または `authority_refs` が対象 issue と交差する record 本文

mental model や record と authority が衝突した場合は authority を優先し、衝突自体を新しい design feedback として扱います。

## Capture threshold

次のいずれかに該当する material な発見だけを record にします。全 issue の日誌は作りません。

- contract と再現可能な実装事実が矛盾する
- correctness、security、compatibility、determinism に影響する hidden assumption が見つかった
- 同じ workaround が複数箇所で必要になり、abstraction の欠落が疑われる
- subsystem を越えて再利用できる mental model が得られた
- public contract または不可逆な実装判断に、より良い代替案が見つかった

局所的な typo、既知 contract の単純な実装ミス、再利用価値のない作業ログは通常の issue/evidence だけで扱います。

## Record creation

1. 専用 GitHub design-feedback issue を作り、番号を予約します。
2. 番号を zero-pad して `DF-0171` のような ID とし、`records/df-0171-<slug>.md` を
   [_template.md](records/_template.md) から作ります。
3. 発見元の implementation issue を `implementation_issues` に一つ以上列挙します。空配列による completion check の迂回は許しません。
4. issue form の `Record path` が `pending-issue-number` の場合、調査や対象実装を続ける前に最終 path へ更新します。
5. `authority_refs` は統合設計、accepted ADR、schema/catalog、または明示された execution contract だけを指します。archive、
   implementation-learning 自身、source/evidence file は authority として扱わず、実装上の根拠は `Evidence` に記載します。
6. `python3 tools/quality/check_ng_design_feedback.py generate --root .` で index を再生成します。
7. `check` で schema、本文、参照、状態条件を検証します。

初回 observation、working mental model、evidence は原則として後から消しません。誤りは `Disposition` に dated correction を追記し、
status と structured metadata を更新します。

## Proceed or block

次は `implementation_disposition: blocked` とします。

- current contract のままでは correctness/security invariant を満たせない
- public semantics、identity、compatibility を誤って固定する
- data loss または不可逆 effect の可能性がある

`may-proceed` は、current contract が依然として健全で、提案が可逆であり、先送りしても不正な public state を作らない場合だけです。
その根拠を `Alternatives and trade-offs` と `Disposition` に記載します。
`contract`、`invariant`、`security`、`compatibility`、`irreversible` impact の active record は常に `blocked` とし、対象外の
作業だけを継続できます。

## Review and disposition

- `impact: local` は self review でよい。
- `contract`、`invariant`、`security`、`compatibility`、`irreversible` を accepted にするには independent review を完了し、
  `review.author` と異なる identity を `review.reviewer` に記録します。`review.refs` には tracking issue 上の具体的な
  `https://github.com/horiyamayoh/cxxlens/issues/<number>#issuecomment-...` URL を含め、レビューを canonical repository の
  record tracking issue に結び付けます。別 repository の同番号 issue は review evidence になりません。
- accepted record は ADR/contract/catalog/test/traceability の `resolution_refs` を持たなければなりません。high-risk acceptance は
  filename prefix だけでなく `Status: Accepted` の ADR を必要とします。record 自体は authority ではありません。
- rejected は理由を、deferred は follow-up issue と再評価 trigger を、superseded は後継 DF ID を残します。
- high-risk change の reviewer を確保できない場合は `proposed` / `blocked` のままにします。

## Issue completion checkpoint

implementation issue の完了 evidence に `Learning checkpoint: none` または `Learning checkpoint: DF-....` を記録します。
完了前に次を実行します。

```sh
python3 tools/quality/check_ng_design_feedback.py issue-ready --root . --issue '#171'
```

未解決の blocking record があれば対象 issue を閉じません。non-blocking の未決事項は専用 tracking issue/record を保持します。
reusable な accepted insight は curated mental model へ反映します。

readiness report は ADR、schema、handbook、checker を `authorities` に含めますが、mental model、record、生成 index、template、
issue form は別の `implementation_learning_assets` として digest します。追跡対象であることを authority への昇格と混同しません。
