# Issue #52 high-risk Contract Candidate validation

## Authority and outcome

`schemas/cxxlens_high_risk_contract_validation.yaml` is the machine-readable gate. It binds all nine
current #43–#51 candidate fingerprints to seven reproducible, test-only spikes. The evidence snapshot is
`tests/contract_spikes/high_risk_validation_evidence.json` with semantic digest
`sha256:c9336e6a2bd18ffc319088b6870c5f3761f2b0d6240175f8767a6d411d393922`.

All seven domains are `validated`; no candidate signature, semantics or schema change was required. This
does not mark any API implemented, conformant, complete, integrated or frozen. #53 remains the authority
for installed public-header integration and #54 remains the only freeze authority.

## Method and isolation

The spike harness models only the difficult public value/state boundaries. It is retained below `tests/`
as a reproducible contract fixture, is not linked into `cxxlens`, and is absent from install/export/public
headers. Every command is argv-form. The checker validates the manifest schema, exact candidate API
membership and fingerprints, evidence byte/semantic digests, reproducibility, backlinks and isolation.

The harness does not substitute a mock at a difficult boundary. It runs bounded graph/fixpoint/transaction/
census/matching/lifecycle models directly. The QA case additionally executes literal argv, timeout, output
limit, missing executable and signal cases; the gate reuses `unit.runtime-ports` to cover the concrete Phase A
process adapter. Production algorithm performance and optimization remain outside #52.

## Decisions

| Domain | Candidate | Result | Bounded evidence | Failure/ambiguity evidence |
|---|---|---|---|---|
| graph | #45/#46 | validated | 6 input nodes, 5 materialized under budget, 1 omitted, 7 edges | cycle and open-world row explicit; truncation retained |
| flow/resource | #50 | validated | recursive fixpoint in 3/8 iterations; 4-step path/counterexample | seven CFG availability states, non-convergence and unknown external explicit |
| transform | #48 | validated | two-file state machine, deterministic stage order | stale/overlap rejected; mid-write rollback atomic; rollback failure has recovery row |
| generation | #49 | validated | 6/6 surfaces accounted, finite 3-node cyclic closure | one ambiguous, one unsupported, duplicate artifact path explicit |
| review/gate | #47/#51 | validated | all six baseline states; diagnostics limited to 2 | duplicate equivalence remains ambiguous; partial gate is indeterminate |
| QA/process/coverage | #43/#51 | validated | five bounded process cases; output cap 64 bytes | timeout/unavailable/signal distinct; five coverage mismatch states; ambiguous/unmatched links retained |
| interop/extractor | #44 | validated | four lifecycle states; 3 facts reduce canonically to 2 | thread violation/schema/exception isolated; in-flight unregister waits |

Selectors, result/evidence/coverage/schema values and workspace/provider boundaries from #43/#45 are exercised
as shared dependencies in these domains. Their fingerprints are therefore gate inputs rather than silently
assumed infrastructure.

## Risk and contract-change log

- No contract was rejected or validated-with-change.
- No owner issue was reopened because all hypothesized lifetime, partial result, ordering, provider,
  schema and boundedness constraints were representable without signature distortion or circular ownership.
- No production implementation, installed target, public type or API was added.
- The retained harness is deleted when #53 owns equivalent public integration fixtures; existing Phase A
  process tests remain under their original owner.

## Gate for #53

`cxxlens-high-risk-contract-validation-check` fails when a candidate fingerprint/API membership changes,
evidence is stale, a domain or fixture class is missing, a command is not argv-form, a result is not resolved,
or spike code enters `include/`, `src/`, an installed target or export surface. #53 must depend on this gate.
