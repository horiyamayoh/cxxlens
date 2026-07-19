# Identity is not reconstruction authority

> Status: Non-normative explanatory model.

## Normative anchors

- [Next-generation integrated design §§17.4–17.6](../../../design/cxxlens_next_generation_integrated_design_ja.md#17-provider-model)
- [Next-generation integrated design §19.5](../../../design/cxxlens_next_generation_integrated_design_ja.md#19-clangllvm-provider)
- [ADR 0063](../../../design/adr/0063-project-catalog-bottom-up-identity.md)
- [ADR 0064](../../../design/adr/0064-portable-provider-task-session-binding.md)
- [ADR 0096](../../../design/adr/0096-clang22-installed-materialization-boundary.md)

## Scope

このモデルは、provider output や external evidence から standard claim を構築するときの identity と payload の境界を説明します。
個別 relation の field、digest domain、adoption policy は上記 authority と各 registry/contract が定義します。

## Model

stable ID は、同じ canonical payload が同じ対象を指すことを検証する authority です。通常は一方向 digest なので、ID だけから元の
payload、座標、role、policy を復元する authority にはなりません。

downstream が claim を構築または再検証する必要がある場合、producer は次を分けて保持します。

- canonical payload を再計算できる coherent value bundle
- bundle から再計算して exact compare する stable ID
- claim payload には直接投影しない provenance / diagnostic evidence

bundle が optional なら、partial field を補完せず all-or-none で扱います。bundle がない observation は、契約が許す場合だけ typed
unresolved と non-exact guarantee を保って残し、復元できない canonical claim を捏造しません。claim owner は、必要な value bundle と
upstream authority を実際に持つ ingestion/materialization boundary に置きます。

identity の field 名が同じでも、scope と derivation が違えば equality authority にはなりません。project より上流の catalog input ID と、project/
variant/source/toolchain payload から導出する relation ID は、明示 mapping と exact cross-binding で結びます。一方を他方の hash input に暗黙 alias
して fixed point を作ったり、opaque placeholder で循環を隠したりしません。

同様に semantic task ID と execution occurrence ID を分離します。同じ semantic task projection は複数 input execution で共有できますが、input
digest と execution ID が occurrence を区別します。result collection を task ID だけで map にすると valid execution を silent deduplicate するため、
contract が定める composite identity で対応付けます。

## Review questions

- consumer は ID の入力 payload を本当に保持しているか。
- digest、path、diagnostic prose、別 evidence から missing field を推測していないか。
- optional bundle の一部だけを受理する path がないか。
- 同名 ID field を equality authority なしに alias していないか。
- identity derivation graph に hash cycle がなく、各 node を topological order で構築できるか。
- semantic task ID だけで複数 execution/result を deduplicate していないか。
- provenance evidence と standard claim payload の owner を混同していないか。
- independent validator が bundle、再計算 ID、hard reference を publication 前に検証するか。

## Known tensions

full bundle は transport/storage cost と disclosure surface を増やします。必要な payload を無制限に複製するのではなく、claim の独立再検証に
必要な最小 projection を versioned contract として保持します。逆に cost を理由に ID だけへ縮退すると、consumer が hidden lookup や
推測へ依存し、relocation、replay、offline validation、backend parity を証明できなくなります。
