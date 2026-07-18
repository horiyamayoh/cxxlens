# ADR 0092: 全 public callable を stable ID 付き exact inventory で管理する

- Status: Accepted
- Date: 2026-07-19
- Issue: #169
- Depends on: ADR 0012, ADR 0089, ADR 0091

## Context

Public API Catalog は package、target、header、API family とその ownership を管理するが、一つの family row に複数の
constructor、member、overload、template、operator を含められる。この粒度だけでは、公開 header に追加された callable、削除済み
declaration の残存 row、cv/ref/noexcept/default argument の drift、一 callable の重複 ownership を機械的に検出できない。

Doxygen XML は documentation の独立検査に必要だが、Doxygen の synthetic ID、表示用 signature、抽出可否を C++ declaration
inventory の authority にすると、Doxygen 設定や version の差が public API identity を変更する。generated relation header も、
Relation Registry から再生成できることに加え、commit された bytes と callable inventory の双方が同じ revision で一致しなければ、
手編集や stale output を見逃す。

## Decision

`schemas/cxxlens_ng_public_callable_inventory.yaml` を全 installed public callable の one-callable-per-row contract とする。Public API
Catalog は header/family/package の admission と ownership を引き続き所有し、各 inventory row は catalog entry、package、target に
exact bind する。row は少なくとも stable callable ID、fully qualified name、callable kind、structured canonical signature、declaring
header、origin、status、stability、qualification、owner、implementation/test/example/Doxygen evidence を持つ。

canonical signature は C++ token 単位で literal spelling を保持し、return type、parameter type/name/default argument、template parameter と
scope/position 付き constraint、static/virtual/constexpr/override/final、cv/ref/nested noexcept、deleted/defaulted state を別 field として保持する。
stable callable ID は source line、Doxygen ID、表示 prose、signature digest から導出しない。fully qualified name、kind、非再利用の
overload slot を domain-separated hash へ入力し、signature や declaration location が変わっても同じ ID を維持する。allocator は scope ごとの
high-water mark と消滅済み scope を保持し、slot 再利用、high-water 低下、ID swap、曖昧な複数 overload migration を拒否する。

inventory の包含対象は public free function/function template、constructor/destructor、member/static member、operator、virtual、明示
deleted/defaulted、inline/constexpr、および admitted generated relation tag の callable とする。private/protected、detail namespace、
non-installed header、compiler-implicit declaration は除外する。除外は extractor の欠落ではなく versioned policy とし、誤って inventory
へ入った private callable も failure にする。

public C++ declaration の観測 authority は、常時実行する locked Clang 22 の AST census とする。別 major、Doxygen、regex、手書き
allowlist への silent fallback は禁止する。
AST と tracked inventory を header-to-inventory / inventory-to-header の双方向で比較し、未登録 declaration、orphan row、signature drift、
duplicate row、複数 installed header/catalog entry ownership を fail closed にする。Doxygen XML は同じ callable 集合と signature projection を
独立に双方向検査する。Clang と Doxygen の column 座標値は同一視せず、同一 header/line 内の各 source order で対応付けた両 anchor を row に
保存し、anchor ごとの name/projection を検査する。Doxygen を AST census の代替にも、AST の成功をもって省略できる検査にもしてはならない。
cross-header/redeclaration identity は pretty type spelling ではなく Clang の mangled identity と `previousDecl` chain で判定し、alias spelling で
重複 ownership を回避できないようにする。同一 header の複数 declaration も default、specifier、origin を含む projection が異なれば、先頭を
採用せず unsupported drift として拒否する。macro-expanded public declaration は spelling を exact に復元できない限り黙って omission せず拒否する。

generated public header は accepted Relation Registry から全 admitted relation を再生成し、commit 済み bytes と一致させてから census
する。generated callable も通常 row と同じ exact contract を持ち、手編集、部分 regeneration、stale generated output は
`cxxlens-quality` を失敗させる。

human review Markdown は同じ inventory から決定的に生成し、対象 commit SHA/tree、inventory canonical digest、callable count、extractor
major、Doxygen correspondence digest を本文に保持する。別 commit の artifact、digest drift、手編集された review projection は readiness と
release evidence に使用できない。inventory、AST/Doxygen correspondence、generated freshness、review artifact は
`cxxlens-quality`、Wave 0 readiness、GR release qualification の同一 revision chain に組み込む。
stable-ID transition は親 commit の inventory と比較し、CI の shallow checkout で親 history が得られない場合は pass/skip にせず失敗する。

## Consequences

- API family の短い catalog summary を callable completeness の証明として扱わない。
- public callable の追加、削除、overload/qualifier/default の変更は inventory と review artifact の明示差分になる。
- Doxygen は public header と同数であるだけでなく、個々の callable の exact correspondence を持つ。
- stable ID allocator の履歴は削除後も保持し、過去の overload slot を別 callable に割り当てない。
- Clang 22 が利用できない環境では exact census を skipped/pass にせず、production quality gate を fail closed にする。
- generated header の registry binding、byte freshness、callable inventory は一つの変更で同時に更新する。

## Verification

positive fixture は同じ入力からの inventory/review artifact の再生成が byte-for-byte deterministic であり、overload、template、
constructor/destructor、operator、cv/ref/noexcept/default argument、deleted/defaulted、inline/constexpr、generated callable を保持することを
確認する。negative fixture は未登録 free/member/overload、qualifier/default drift、declaration 削除後の orphan row、複数 entry ownership、
private callable の誤登録、generated header の手編集を個別に拒否する。Doxygen gate は AST gate とは独立に XML の missing/extra/signature
drift を拒否する。
