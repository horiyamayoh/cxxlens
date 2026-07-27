---
id: DF-0206
title: Bind SQLite writer SHM holders to native attachments
status: observed
kind: contract-contradiction
impact: invariant
confidence: high
implementation_disposition: blocked
scope:
  - store.sqlite-same-process-shm-mapping
  - store.sqlite-multi-instance-cas
  - provider.clang22-materialization-store-race
authority_refs:
  - docs/design/cxxlens_next_generation_integrated_design_ja.md
  - docs/design/adr/0013-ng-sqlite-physical-store.md
  - docs/design/adr/0097-sqlite-v3-chunked-payload-migration.md
  - schemas/cxxlens_ng_sqlite_store_contract.yaml
  - schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.yaml
  - schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
tracking_issue: '#206'
implementation_issues:
  - '#181'
resolution_refs: []
review:
  mode: independent
  status: pending
  author: codex-agent-sqlite-writer-attachment-observation
  reviewer: null
  refs: []
created: '2026-07-28'
---

# Bind SQLite writer SHM holders to native attachments

## Observation

At exact PR head `ad495cd7e43a48dee3274ebeadabe1efb4b78dbd`, the production-inert
DF-0205 coordinator promotes every successful writer `xShmMap` attempt to a distinct writer
holder. SQLite may call `xShmMap` for more than one page on one `sqlite3_file`, but its VFS
lifecycle supplies one `xShmUnmap` callback which releases that connection's complete native SHM
attachment.

The coordinator requires every promoted holder to enter and complete a native-cleanup obligation.
After the first holder enters cleanup, its callback receipt remains active; a second holder release
using the same invocation receipt is rejected by the callback total-order check. Therefore the
current model cannot represent two page-map successes on one attachment followed by one exact
native unmap. An adapter would have to invent multiple native callbacks, reuse one outcome as if it
were multiple unrelated effects, or quarantine a valid SQLite teardown.

## Working mental model

Mapping-generation authority and native-attachment lifetime are related but not identical. A
generation may contain multiple pages and multiple independent writer attachments. One exact
writer attachment belongs to a connection, alias, open epoch, and native `sqlite3_file`; it may
accumulate several holder-specific map/effect receipts. The attachment should admit one teardown
callback only after all its page-map callbacks have resolved, while another connection or alias
must retain a separate attachment and cleanup obligation.

Confidence is high that the present state machine is insufficient. The exact normative choice
between an attachment holder with accumulated map authorities and an atomic grouped-release
operation remains subject to counterexample review.

## Mismatch or opportunity

DF-0205 authority binds exact evidence to each writer map attempt/resulting holder, allows a
same-generation new page to update the page set, and requires last-holder retirement to delegate
native unmap without fabricating success. It does not define how several successful map attempts
on one native attachment aggregate into the single `xShmUnmap` callback exposed by SQLite.

This is an invariant-level hidden assumption. Production writer VFS binding and the reader
exception remain blocked until normative authority defines attachment cardinality, aggregation,
second-map failure, and one-shot cleanup. Contract-independent post-native cleanup reachability
and production-inert evidence/validator work may continue.

## Evidence

- `src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp` creates a new holder for every
  successful `promote` call and records one map receipt in that holder.
- `release_holder` creates one cleanup obligation per holder. A non-last holder is immediately
  marked `nonlast_native_cleanup_admitted`.
- `callback_can_start_locked` rejects a second nonterminal holder cleanup using the same callback
  invocation token, so one `xShmUnmap` cannot release two holders.
- `tests/unit/sdk/sqlite_same_process_shm_mapping_lease_test.cpp` covers multiple holders owned by
  distinct writer connections and requires a distinct cleanup callback for each. It has no
  positive case for page 0 plus page 1 on the same connection followed by one unmap.
- ADR 0097 and the SQLite/Snapshot contracts already require same-generation new-page growth,
  exact callback receipts, one-shot native cleanup, and no fabricated unmap success.
- Canonical tracking issue:
  <https://github.com/horiyamayoh/cxxlens/issues/206>.

The coordinator is not connected to the production VFS at this head, so the mismatch has not
activated a public behavior or weakened the current blanket native-OK rejection.

## Alternatives and trade-offs

1. Treat one writer holder as one exact native attachment. The first successful map creates the
   holder; later exact same-attachment maps atomically add their map/effect authorities and page
   receipts. One unmap releases the holder. This directly models SQLite, but the contract must
   define exact connection/alias/open-epoch binding, concurrent map ordering, and failure of a
   later map after an earlier page is live.
2. Retain one holder per map attempt and add an atomic grouped-release receipt. The group must be
   sealed before native unmap, contain the complete exact same-attachment holder set, reject
   foreign or missing holders, and consume one callback/native outcome exactly once. This is less
   invasive to promotion but creates a separate aggregation state machine.
3. Delegate or report one native unmap per map attempt. This contradicts the actual callback
   lifecycle and can invalidate pages still in use, so it is rejected.
4. Assume production SQLite maps only one page. The accepted authority explicitly covers new
   pages and does not grant this assumption, so it is rejected.

## Recommendation

Amend DF-0205 authority so a writer holder represents one exact native SHM attachment and retains
an ordered set of holder-specific map authorities. Bind the attachment to one process/runtime/VFS
cohort, alias lifetime, connection token, open epoch, native main handle, and mapping generation.
Require later maps on that attachment to complete the existing predelegate, post-map, extend-pair,
effect, identity, page-set, and size checks before atomically joining the holder.

Define a single attachment teardown transition that hides the holder once, waits only for the
existing bounded in-flight set, delegates one exact `xShmUnmap`, and applies that one outcome to
the attachment as a whole. Distinct connections/aliases remain distinct holders. A failed later
native map with no mapping resolves only its in-flight attempt; a native mapping followed by
failed post-validation must unmap the complete attachment and retire/quarantine all authority
bound to it.

Add positive and negative matrix entries for multi-page one-attachment/one-unmap, accidental
cross-attachment aggregation, incomplete holder sets, duplicate unmap, second-map failure, and
concurrent last-unmap versus later-map admission. Production integration must wait for an
independent exact authority review.

## Disposition

2026-07-28: Observation recorded during the independent integration audit of the production-inert
DF-0205 coordinator at `ad495cd7e43a48dee3274ebeadabe1efb4b78dbd`. Issue #206 and this record
block the affected writer VFS integration. No normative authority or production source behavior
has changed. Post-native cleanup reachability is a separate local implementation correction under
the already-accepted DF-0205 contract and may proceed.

2026-07-28: A non-authorizing amendment proposal,
`cxxlens.sqlite.writer-shm-native-attachment.v1`, was drafted into integrated design, ADR 0013/0097,
and identical SQLite/Snapshot contract and schema-mirror objects. It retains per-map authorities
but groups the complete exact-attachment member set into one native cleanup outcome, and adds
cross-attachment, partial-set, duplicate-unmap, second-page failure, remap, close, and map/unmap-race
counterexamples. The proposal remains `proposed-unqualified-non-authorizing`; this record remains
`observed`, blocked, with empty `resolution_refs` and independent review pending. No attachment
group implementation or production binding is authorized by this draft.

2026-07-28: Independent counterexample review of exact proposal commit
`3c52b7e01a4d2a4e382940017d1dfb8f07f1be54` returned `P0=0 / P1=2 / P2=1` and
rejected it. The blocking review is recorded at
<https://github.com/horiyamayoh/cxxlens/issues/206#issuecomment-5097510242>. A non-last
attachment could remove the only live support for one page without defining fresh-reader
admission, page receipt transfer, or the relation between page authority and monotonic sealed
size. Separately, a gate snapshot could promote a partial map-before-gate group while a later map
became in-flight or post-native. Focused nested status, authorization, identity, platform, and
these two rules also lacked direct mutation coverage.

2026-07-28: The revised four-mirror proposal tracks exact page support per live attachment group,
atomically recomputes fresh-reader-admissible pages after non-last cleanup, prevents retired
receipt transfer, and keeps sealed size only as a monotonic physical observation. It total-orders
gate completion with later-map admission: map-winning attempts become bounded gate blockers,
gate-winning attempts use callback-local gate-before-map promotion, and timeout, unknown outcome,
or open-epoch drift hides and quarantines the complete group. Direct mutation negatives and the
expanded close/remap matrix bind these rules. The revised enclosing lease digest is
`sha256:612d450d22b676e4144b76f61cab60cade3ae860f3457b7ec168a9bd00cd9550`.
This record remains `observed` / blocked with empty `resolution_refs`; the revised proposal remains
`proposed-unqualified-non-authorizing` pending a fresh independent exact review.
