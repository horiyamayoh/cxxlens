# ADR 0055: provider child は inherited FD と異種 syscall ABI を fail closed にする

- Status: Accepted
- Date: 2026-07-17
- Issue: #112

## Context

Linux adapter は fork 後に標準入出力を接続したが、親の non-CLOEXEC descriptor を閉じずに exec していた。既存 socket/file は
seccomp の socket 生成拒否を迂回する capability となり、`network-syscall-deny` / enforced report と実効状態が一致しなかった。
seccomp BPF も `seccomp_data.arch` を検証せず syscall number だけを比較していた。

## Decision

child は stdin/stdout/stderr の `dup2` 成功後、`close_range(3, UINT_MAX, CLOSE_RANGE_UNSHARE)` を必須とする。これにより threaded
host の descriptor table から分離し、0/1/2 以外を exec 前に閉じる。現 contract に追加 allowlist はなく、cleanup または dup が
失敗すれば exit 126 として `security.sandbox-insufficient` / assurance none にする。

network seccomp filter は syscall number 読み取り前に `seccomp_data.arch` を build target の accepted audit architecture
(`AUDIT_ARCH_X86_64` または `AUDIT_ARCH_AARCH64`) と比較し、不一致を `SECCOMP_RET_KILL_PROCESS` で拒否する。未対応 build
architecture は filter installation 失敗として enforced を返さない。`inherited-fd-close-range` と `seccomp-audit-arch` を built-in
policy mechanisms に追加し、policy digest と sandbox evidence v2 に bind する。

## Verification

process fixture は親で開いた non-CLOEXEC socketpair が child main の 0/1/2 以外に存在しないことを検査する。既存 network syscall
拒否、mechanism installation failure、clean/dirty host の同じ enforced report、policy digest vectors を合わせて検証する。
