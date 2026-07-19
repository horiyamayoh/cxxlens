# ooblens Implementation and Qualification Plan

| Field | Value |
| --- | --- |
| Document ID | `OOBLENS-PLAN-001` |
| Version | `0.1.0-proposal` |
| Status | non-normative proposal |
| Parent | `OOBLENS-SRAD-001` |
| Owner issue | `#188` |

本書は implementation issue graph、work units、acceptance vectors、quality gates、evidence、release profile を定義する
proposal です。各 unit は schema-first、issue-tracked、branch/PR、independent review、exact-head evidence を基本とします。

---

## 1. Delivery principles

1. product semantics before implementation
2. no direct-main write
3. one active write unit per shared contract
4. app-internal experimental surface first
5. no production support claim without installed whole-product evidence
6. no `finding count == 0` safety claim
7. no model/target/access omission
8. no solver verdict without certificate validation
9. no report fact not present in checked certificate
10. every completed implementation issue records learning checkpoint

---

## 2. Milestone profile

### M0 — Design closure

Deliverables:

- integrated design
- contract/class/function catalog
- implementation plan
- issue graph
- open-decision records
- independent adversarial review

No code.

### M1 — End-to-end GetHoge vertical slice

Target scenario:

```cpp
class hoge_store {
public:
    int get_hoge(int index) const
        /* declared pre: 0 <= index && index < m_hoge.size() */
    {
        return m_hoge[index];
    }

private:
    std::vector<int> m_hoge;
};
```

Product has at least:

- one safe entry path
- one violating entry path behind multiple branches/calls
- one closed virtual dispatch
- one build condition without violation
- one condition with violation

Required behavior:

- no local “could be OOB” lint on `get_hoge`
- callee body proved safe under precondition
- caller contract obligation generated at every target/call context
- violating entry input and path replayed
- safe variant proved closed
- violating variant reported
- UML/CFG/memory report generated
- memory/SQLite result parity
- root/jobs/order deterministic parity

### M2 — Product call closure

- multiple translation units
- direct calls
- closed virtual hierarchy
- selected function-pointer/callback flow
- link-unit/entrypoint model
- exact target closure accounting
- external-model unresolved

### M3 — Dynamic memory and containers

- `std::array`
- `std::span`
- `std::vector`
- `std::basic_string`
- pointer + length
- `new T[n]`
- malloc/calloc/realloc
- memory primitives
- model validation

### M4 — Loops and recursive summaries

- affine loops
- monotone size/index loops
- checked unwinding
- SCC fixpoint
- inductive invariant certificate
- recursion unresolved/refinement behavior

### M5 — Whole-product incremental production path

- cxxlens actual-source installed path available
- product materialization
- transactional derived snapshot
- warm-zero
- affected-only recompute
- file-backed SQLite close/reopen
- exact publication/evidence

### M6 — High-assurance proof

- independent checker A2
- proof-object or dual-solver option
- malformed/mutated certificate negatives
- model drift invalidation
- TCB report
- long-run qualification

### M7 — UB foundation extraction

- second property pack prototype
- identify reusable `libubmodel`/analysis SDK surfaces
- two-consumer evidence
- experimental cxxlens extension proposal if justified

---

## 3. Dependency-ordered work units

### U00 — Design authority and review

Scope:

- finalize documents
- open unresolved design decisions as dedicated issues
- obtain independent review focused on completeness claims, call-contract law, proof TCB, cxxlens boundary

Acceptance:

- no contradiction among three documents
- terminology/status/IDs exact
- review findings resolved or explicitly blocking

### U01 — Canonical base value types

Implement:

- opaque IDs
- target/machine semantics
- canonical symbolic expression
- product condition
- validation/error codes

Tests:

- positive round trip
- invalid UTF-8
- invalid enum
- overflowed width
- noncanonical expression
- order/root perturbation
- fuzz parser/encoder

### U02 — Product scope and closure contract

Implement:

- product scope schema
- entrypoint/input model
- closure dimensions
- validation
- config parser

Tests:

- missing entrypoint
- missing input
- open plugin
- unknown link input
- condition mismatch
- canonical set ordering
- scope ID mutation

### U03 — Contract adapter foundation

Implement:

- normalized predicate algebra
- declared/verified/trusted/inferred authority classes
- sidecar adapter
- one project macro adapter
- contract catalog and instantiation

Tests:

- actual/formal binding
- receiver field binding
- duplicate/conflicting contract
- impure predicate rejection
- adapter semantics change invalidates ID
- inferred candidate never assumed

### U04 — Verification IR schema and CAS

Implement:

- canonical module/function/CFG/value/memory subset
- artifact CAS
- model manifest
- strict parser/validator
- source mapping

Tests:

- duplicate IDs
- dangling edge/value
- invalid CFG
- artifact digest mismatch
- corrupted/truncated artifact
- canonical renumbering parity

### U05 — Clang 22 extraction vertical slice

Implement:

- one-TU exact Clang 22 extractor
- function/CFG lowering
- direct calls
- vector size/operator[] recognizer
- memory access inventory
- source/macro spans
- unsupported accounting

Tests:

- AST pointer escape compile-negative
- macro access identity
- same source different root
- malformed arguments
- unsupported Clang major
- access inventory exact count

### U06 — Link/product graph adapter

Prototype app-owned:

- compile database ingestion
- link-unit manifest
- cross-TU function identity binding
- entrypoint mapping
- direct call graph
- target set closure

Do not claim production cxxlens integration while #181/#182/#187 remain blocked.

Tests:

- missing TU
- duplicate link input
- DSO classified external
- plugin open
- cross-TU direct call
- same function display name distinct identity

### U07 — Memory object and dynamic extent model

Implement:

- object identities
- vector payload state
- symbolic extent
- access offset/width
- pointer + length pair
- standard memory operations minimum

Tests:

- empty vector
- size zero
- size/index equality boundary
- negative index conversion
- width > 1 element
- integer conversion before access
- one-past formation vs dereference

### U08 — Bounds obligation generator

Implement:

- lower/upper/width/provenance/subobject predicates
- first-UB dependency
- complete access-to-obligation accounting
- obligation relation publication draft

Tests:

- every access exactly one primary obligation group
- no local warning from declared precondition
- insufficient precondition classified separately
- width addition overflow
- unknown base unresolved

### U09 — Intraprocedural abstract engine

Implement:

- constant/interval/known-bits/congruence
- path conditions
- branch transfer
- simple loop fixpoint
- implication
- witness extraction for simple cases

Tests:

- safe/unsafe boundary
- off-by-one
- signed/unsigned conversion
- branch-dominated guard
- early return
- loop <= vs <
- budget partial

### U10 — Function summary engine

Implement:

- pre/post
- return/out values
- extent updates
- alias
- footprint
- summary validation
- callee application

Tests:

- size-preserving call
- size-changing call
- return index clamp
- conditional summary
- exception path
- summary semantics mutation invalidation

### U11 — Caller/callee contract closure

Implement:

- implementation obligation
- call-site obligation
- actual/formal/receiver binding
- all-target conjunction
- product entry-state propagation
- safe and violating GetHoge paths

This is the first product-value milestone.

Acceptance:

- body safe under precondition
- no body-level noise
- every reachable caller checked
- unreachable caller not reported
- one violating entry witness
- all-safe product closure certificate

### U12 — Closed virtual dispatch

Implement:

- hierarchy/override target set
- receiver dynamic-type conditions
- closure authority
- all-target call obligations

Tests:

- safe base/unsafe derived
- safe derived/unsafe base
- hidden complete hierarchy
- externally extensible hierarchy unresolved
- target set mutation
- impossible receiver type pruned with proof

### U13 — Function pointers and callback registration

Implement:

- address-taken set
- assignment flow
- selected callback registration models
- target completeness

Tests:

- direct assigned pointer
- conditional pointer
- callback table
- external overwrite unresolved
- ABI incompatible target rejection

### U14 — SMT refinement backend

Implement:

- bit-vector/pointer/extent encoding
- solver port process isolation
- model extraction
- deterministic request/result
- timeout/cancel/resource reporting

Tests:

- concrete violating model
- unsat implication
- malformed solver output
- solver disagreement path
- cancelled partial
- backend digest binding

### U15 — Counterexample certificate and replay checker

Implement:

- certificate schema
- deterministic IR interpreter
- entry input replay
- call/branch path verification
- object/access interval
- first-UB check

Tests:

- valid witness accepted
- one value changed rejected
- path edge removed rejected
- object extent changed rejected
- earlier overflow injected rejected
- wrong source/build/scope rejected

### U16 — Safety invariant certificate checker

Implement:

- entry initiation
- transition consecution
- call precondition implication
- access bounds implication
- target/access set closure
- model/trust validation

Tests:

- non-inductive invariant
- omitted block/access
- incomplete target set
- hidden unresolved
- invalid model dependency
- safe closed example accepted

### U17 — cxxlens source/finding adapter

Implement:

- read exact input snapshot
- preserve claims/coverage/unresolved
- make derived claims
- publish finding/closure snapshot transactionally
- memory/SQLite parity

Blocked production status:

- installed actual-source path pending #181/#182/#187
- production incremental coordinator pending #184

Tests:

- prior head survives failure
- stale expected parent
- hard/soft refs
- evidence/guarantee preserved
- memory/SQLite canonical parity

### U18 — Report and UML renderer

Implement:

- checked-certificate-only input
- HTML/JSON
- product path
- sequence
- CFG witness
- object diagram
- memory interval
- source slice
- TCB/model/assumption panel

Tests:

- source HTML escaping
- no unchecked result rendered as proved
- deterministic output digest
- accessibility text
- large graph truncation marked
- diagram data equals certificate

### U19 — Incremental coordinator

After or alongside cxxlens #184 resolution:

- partition plan
- summary/proof cache
- model/contract invalidation
- exact warm-zero
- transaction

Tests:

- source-only mutation
- contract-only mutation
- model-only mutation
- checker version mutation
- one SCC recompute
- corruption/stale/failure survival

### U20 — Installed real-product qualification

- relocated prefix
- static/shared
- compile DB + link plan
- multi-TU C++ product
- exact worker/checker/solver
- file-backed SQLite reopen
- root/jobs/order permutations
- sanitizer qualification
- schema-validated report artifact

---

## 4. Acceptance scenario catalog

### A001 Declared precondition, all callers safe

Expected:

```text
get_hoge implementation: proved_safe
call obligations: all proved
body warning: none
product closure: true
```

### A002 Declared precondition, one direct caller unsafe

Expected:

```text
reachable_contract_violation
entry input and path
index == size witness
consequent OOB access shown
```

### A003 Unsafe caller unreachable

Expected:

```text
unreachable_proved
no product finding
unreachability evidence retained
```

### A004 Insufficient callee precondition

```cpp
pre(index <= size())
return data[index];
```

Expected:

```text
callee_contract_insufficient
not caller-blame-only
witness under declared precondition
```

### A005 Guard before call

```cpp
if (index >= 0 && index < store.size())
    store.get_hoge(index);
```

Expected safe.

### A006 Guard loses relation through transformations

```cpp
if (raw < size)
    get_hoge(raw + offset);
```

Expected according to constraints; SMT refinement if required.

### A007 Safe base target, unsafe derived target

Expected all-target violation if derived reachable.

### A008 Externally extensible virtual hierarchy

Expected `unresolved_target`, never closed.

### A009 Vector size mutation between guard and call

Single-thread:

- summary tracks mutation and finds violation/safety.

Concurrent unknown:

- `unresolved_concurrency`.

### A010 Callback target flow

Registered closed callback set proves all targets or reports target-specific violation.

### A011 Pointer + length

```cpp
consume(buffer, length);
```

Caller must imply model precondition for every memory operation.

### A012 memcpy two-sided bounds

Source and destination obligations are independent and both reported.

### A013 Signed/unsigned conversion

Negative `int` converted to `size_t` must produce exact machine witness.

### A014 First UB is overflow

OOB result must not be primary if signed overflow precedes it.

### A015 Loop boundary

`i <= size` finds witness; `i < size` proves safe under invariant.

### A016 Recursive traversal

Inductive summary or exact bounded profile; otherwise unresolved.

### A017 Model drift

Changing vector model digest invalidates all dependent summaries/proofs.

### A018 Macro access

Report spelling/expansion/origin correctly.

### A019 Build variant divergence

Safe release variant and unsafe feature variant remain distinct.

### A020 Budget

Partial proved findings retained; closure false with exact budget reason.

### A021 Checker mutation negatives

Every certificate field affecting semantics is covered by a rejection fixture.

### A022 Backend disagreement

Preserve differential disagreement; no first-wins.

### A023 Root/jobs/order/backend parity

Semantic result/certificate digest identical across permutations.

### A024 Runtime fusion

ASan/UBSan observation joins only with exact binary/source/access identity.

### A025 Empty finding set with missing TU

Result is incomplete, not safe.

---

## 5. Qualification gates

### G0 Schema and canonical identity

- all schema validators green
- positive/negative vectors
- canonical digest parity
- fuzz parsers

### G1 Clang extraction

- exact major
- no native escape
- access inventory
- macro/source mapping
- unsupported accounting

### G2 Product model

- compile/link/entrypoint/input closure
- cross-TU identity
- call target completeness

### G3 Bounds semantics

- object/extent/access obligations
- integer/pointer semantics
- first-UB discipline

### G4 Interprocedural contract closure

- callee implementation
- caller obligations
- polymorphism/callback
- safe/unsafe scenarios

### G5 Proof and checker

- witness replay
- invariant checking
- mutation negatives
- TCB report

### G6 cxxlens snapshot parity

- claims/provenance/guarantee
- memory/SQLite
- transactional publication
- prior snapshot survival

### G7 Incrementality

- warm-zero
- affected-only
- full recompute parity
- corruption/stale failure

### G8 Installed product

- relocated install
- static/shared
- real multi-TU
- exact toolchain/binaries
- file-backed SQLite reopen

### GR Production report

Report binds:

- revision/tree
- cxxlens/ooblens versions
- toolchain/target
- worker/solver/checker binary digests
- product scope
- model/contract sets
- access/target/entrypoint sets
- proof result/certificate set digests
- closure dimensions
- test/gate artifacts

---

## 6. Performance qualification

Initial benchmark classes:

| Class | Scale |
| --- | --- |
| micro | 1–10 functions, 1–100 accesses |
| component | 1k functions, 10k accesses |
| medium product | 25k functions, 100k accesses |
| target product | 100k+ functions, 500k+ accesses |

Measure:

- extraction wall/cpu/memory
- model size
- access inventory rate
- summary cache hit
- SCC iterations
- proof obligations/sec
- solver calls
- witness/check time
- report size/time
- cold/warm incremental delta

Performance claims never weaken semantic budgets or closure rules.

---

## 7. CI matrix

```text
Compiler:
  exact Clang 22 for native provider
  supported host C++23 compiler for generic targets

Build:
  static
  shared

Store:
  memory
  SQLite in-memory
  SQLite file close/reopen

Sanitizer:
  ASan/UBSan
  TSan where supported

Order:
  jobs 1/2/8
  forward/reverse/shuffle fixture order
  relocated root

Backends:
  built-in abstract
  SMT adapter
  optional CBMC/CHC lanes

Profiles:
  open-audit
  closed-bounded
  closed-inductive
```

---

## 8. Independent review plan

Review roles:

1. C++ semantics reviewer
2. formal verification reviewer
3. cxxlens contract/invariant reviewer
4. security/parser/sandbox reviewer
5. product UX/report reviewer
6. performance/scalability reviewer

Blocking review questions:

- Does contract assumption always create a caller obligation?
- Can any missing target/access/model be collapsed into safe?
- Is product closure dimension-complete?
- Can witness replay reach a different semantics than analysis?
- Can a prior UB invalidate the OOB witness?
- Can renderer invent or omit proof facts?
- Does any proposed cxxlens API leak backend-specific semantics?
- Are public candidates justified beyond one app?
- Can model changes silently preserve stale proofs?
- Can variant/link/plugin differences silently fallback?

---

## 9. Candidate issue graph

```text
#188 Design parent
  -> design review decisions
  -> U01 base/canonical
  -> U02 product scope
  -> U03 contracts
  -> U04 verification IR
  -> U05 Clang extraction
  -> U06 link/product graph
  -> U07 memory/extent
  -> U08 obligations
  -> U09 abstract engine
  -> U10 summaries
  -> U11 contract closure
  -> U12 virtual targets
  -> U13 callbacks
  -> U14 SMT
  -> U15 witness checker
  -> U16 safety checker
  -> U17 cxxlens adapter
  -> U18 report
  -> U19 incrementality
  -> U20 qualification
```

Shared contract units are serialized. Read-only research/review may proceed in parallel.

---

## 10. Initial implementation recommendation

To minimize application size while preserving the intended product value:

1. use cxxlens for identity, snapshot, claims, provenance, coverage, unresolved and incremental fingerprints;
2. keep solver-oriented verification IR internal and content-addressed;
3. implement one exact Clang 22 extractor;
4. begin with direct calls plus one closed virtual hierarchy;
5. support symbolic `std::vector::size()` and declared preconditions from the first vertical slice;
6. use a small built-in interval/relational engine for common cases;
7. add an SMT backend only for unresolved relevant slices;
8. implement concrete witness checker before broadening detection;
9. implement safety invariant checker before any “OOB-free” product claim;
10. defer stable cxxlens API proposals until ooblens and a second analysis consumer demonstrate reuse.

This avoids a toy fixed-array lint while also avoiding an initial attempt to implement the entire C++ abstract machine.

---

## 11. Completion definition for the design program

Issue #188 design work is complete when:

- three proposal documents are internally consistent;
- GetHoge semantics are explicit and local-noise behavior is fixed;
- product closure and 100% claim are machine-definable;
- dynamic extent, polymorphism, inputs and external models are addressed;
- proof/safety/unresolved statuses are exhaustive;
- cxxlens and app responsibilities are unambiguous;
- class/function/relation/error contracts are specified;
- implementation units and acceptance vectors cover the vertical path;
- independent review has no unresolved correctness blocker;
- draft PR records that implementation and public API changes remain future issues.
