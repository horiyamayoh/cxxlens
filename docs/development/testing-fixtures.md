# Testing fixture protocol

fixture は deterministic input を構築し、production catalog/provider/kernel/query path を通します。AST node、
semantic claim、snapshot、query result を fixture 内で捏造して acceptance を通してはいけません。

各 relation/provider/query contract は最低限、positive、negative、ambiguous、unsupported、partial/failure の
vectorを持ちます。property failure は seed と case を表示し、同じ入力を再現可能にします。golden
normalization は宣言済み root と operational metadata だけを変換し、ID、source range、condition、reason
code、evidence、coverage、closure、unresolved、conflict を隠しません。

NG gate は jobs 1/2/8、forward/reverse/seeded order、root relocation、memory/SQLite、cold/warm、
static/shared、GCC/Clang public header、provider success/crash/timeout/malformed を組み合わせます。
semantic comparison は unordered relation digest と canonical export の双方で行います。

既存 M0/M1/M2 fixture は migration baseline です。価値のある挙動は ledger に従って relation/provider/
snapshot acceptance へ接続し、旧 API ID や central fact model を新 fixture の authority にしません。
