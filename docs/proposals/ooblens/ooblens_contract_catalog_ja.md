# ooblens Contract, Class and Function Catalog

| Field | Value |
| --- | --- |
| Document ID | `OOBLENS-CATALOG-001` |
| Version | `0.1.0-proposal` |
| Status | non-normative proposal |
| Parent | `OOBLENS-SRAD-001` |
| Owner issue | `#188` |

本書は coding agent が package、class、function、relation、schema、error、lifetime、threading を
推測せず実装 issue へ分解できる粒度の catalog です。signature は design intent であり、
accepted public API ではありません。

---

## 1. Package and target DAG

```text
ooblens::base
    ↓
ooblens::model
    ↓
ooblens::contracts
    ↓
ooblens::analysis
    ↓
ooblens::proof
    ↓
ooblens::report

cxxlens::provider_sdk ──────→ ooblens::cxxlens_adapter
cxxlens::clang22_provider_sdk → ooblens::clang22_provider
ooblens::cxxlens_adapter + ooblens::analysis + ooblens::proof → ooblens::orchestrator
ooblens::orchestrator + ooblens::report → ooblens CLI
```

Proposed CMake packages:

```text
ooblens
  ooblens::base
  ooblens::model
  ooblens::contracts
  ooblens::analysis
  ooblens::proof
  ooblens::report
  ooblens::ooblens

ooblensCxxlensAdapter
  ooblens::cxxlens_adapter

ooblensClang22Provider
  ooblens::clang22_provider
```

Rules:

- `base/model/contracts/analysis/proof/report` expose no Clang/LLVM type.
- `clang22_provider` alone opts into exact Clang 22.
- `report` consumes checked certificate values, not AST or solver objects.
- solver process integration is behind `solver_port`.
- app-owned artifact CAS is behind `artifact_store`.
- cxxlens adapter depends on public cxxlens author SDK only.
- no target depends upward on CLI.

---

## 2. Repository layout proposal

```text
include/ooblens/
  base.hpp
  model.hpp
  contract.hpp
  analysis.hpp
  proof.hpp
  report.hpp
  cxxlens.hpp

src/base/
src/model/
src/contracts/
src/analysis/
src/proof/
src/report/
src/cxxlens/
src/clang22/
tools/ooblens/
tools/ooblens-proof-check/
tools/ooblens-report/
schemas/
models/standard/
examples/
tests/unit/
tests/integration/
tests/negative/
tests/install/
```

Generated relation tags:

```text
include/ooblens/relations/
```

Generated schema/API files are never hand edited.

---

## 3. Core value types

### 3.1 IDs

All IDs are opaque validated UTF-8 strings with a registered semantic domain.

```cpp
struct product_scope_id;
struct link_unit_id;
struct entry_point_id;
struct function_realization_id;
struct cfg_block_id;
struct ir_value_id;
struct memory_object_id;
struct memory_access_id;
struct contract_id;
struct predicate_id;
struct obligation_id;
struct proof_result_id;
struct certificate_id;
struct artifact_digest;
struct model_id;
```

No identity depends on:

- pointer/address
- absolute checkout root
- timestamp
- PID/TID
- unordered iteration
- display name
- task completion order
- solver variable numbering
- physical backend plan

### 3.2 Machine semantics

```cpp
enum class signedness : std::uint8_t {
    unsigned_value,
    signed_value,
};

struct integer_type {
    std::uint32_t width_bits;
    signedness sign;
    result<void> validate() const;
};

struct target_semantics {
    std::string target_triple;
    std::string data_layout_digest;
    std::string abi_digest;
    std::string language_semantics;
    std::string frontend_semantics;
    result<void> validate() const;
    std::string id() const;
};
```

### 3.3 Symbolic expressions

Closed expression vocabulary for v1:

```cpp
enum class expression_kind : std::uint8_t {
    boolean_constant,
    integer_constant,
    value_ref,
    object_extent,
    object_size,
    container_size,
    result_value,
    old_value,
    checked_add,
    checked_subtract,
    checked_multiply,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    logical_not,
    logical_and,
    logical_or,
    conditional,
};

struct expression {
    expression_kind kind;
    std::optional<integer_type> type;
    std::vector<expression> operands;
    std::string reference;
    std::vector<std::byte> constant;
    result<void> validate() const;
    std::vector<std::byte> canonical_encoding() const;
    predicate_id id() const;
};
```

Expression tree is immutable after validation. Builders may be mutable/thread-confined.

### 3.4 Conditions

```cpp
struct product_condition {
    std::string universe;
    std::vector<std::string> fragments;
    result<void> validate() const;
};
```

This maps exactly to cxxlens claim conditions at the adapter boundary.

---

## 4. Product scope API

```cpp
enum class scope_dimension : std::uint8_t {
    source_build,
    link,
    dynamic_loading,
    entrypoints,
    inputs,
    call_targets,
    external_calls,
    memory,
    control,
    conditions,
    concurrency,
    native_code,
    budget,
};

enum class closure_state : std::uint8_t {
    closed,
    open,
    unsupported,
    truncated,
};

struct closure_dimension_result {
    scope_dimension dimension;
    closure_state state;
    std::vector<std::string> evidence;
    std::vector<std::string> unresolved;
    result<void> validate() const;
};

struct entry_point_contract {
    entry_point_id id;
    function_realization_id function;
    product_condition presence;
    expression input_predicate;
    std::vector<model_id> environment_models;
    result<void> validate() const;
};

struct product_scope {
    product_scope_id id;
    std::string catalog_id;
    std::vector<link_unit_id> link_units;
    std::vector<entry_point_contract> entry_points;
    std::vector<model_id> external_models;
    target_semantics target;
    product_condition condition;
    std::string verification_profile;
    std::string trust_policy_digest;
    std::string resource_policy_digest;
    result<void> validate() const;
    std::vector<std::byte> canonical_projection() const;
};
```

Factory:

```cpp
result<product_scope> make_product_scope(product_scope_draft draft);
```

Validator recomputes ID bottom-up. Caller-supplied ID is never trusted.

---

## 5. Contract model API

```cpp
enum class contract_kind : std::uint8_t {
    precondition,
    postcondition,
    object_invariant,
    loop_invariant,
    external_summary_assumption,
    environment_assumption,
};

enum class contract_authority : std::uint8_t {
    verified,
    declared,
    trusted_external,
    inferred_candidate,
    runtime_observed,
};

struct contract_source {
    std::string adapter_id;
    std::string adapter_semantic_digest;
    std::string source_span;
    std::string evidence;
};

struct contract {
    contract_id id;
    contract_kind kind;
    contract_authority authority;
    function_realization_id function;
    expression predicate;
    contract_source source;
    product_condition presence;
    std::string interpretation;
    result<void> validate() const;
};

struct argument_binding {
    std::string formal;
    ir_value_id actual;
};

struct instantiated_contract {
    contract_id source_contract;
    memory_access_id call_or_access;
    expression bound_predicate;
    std::vector<argument_binding> bindings;
    result<void> validate() const;
};
```

Catalog:

```cpp
class contract_catalog {
public:
    static result<contract_catalog>
    build(std::span<const contract> contracts);

    result<std::span<const contract>>
    for_function(function_realization_id function) const;

    result<instantiated_contract>
    instantiate_precondition(
        contract_id contract,
        memory_access_id call_site,
        std::span<const argument_binding> bindings) const;

private:
    struct data;
    std::shared_ptr<const data> data_;
};
```

---

## 6. Verification program model

### 6.1 Function realization

```cpp
struct function_realization {
    function_realization_id id;
    std::string semantic_entity;
    link_unit_id link_unit;
    product_condition presence;
    std::string body_artifact;
    std::string source_anchor;
    bool externally_visible;
    result<void> validate() const;
};
```

### 6.2 CFG

```cpp
enum class edge_kind : std::uint8_t {
    unconditional,
    true_branch,
    false_branch,
    switch_case,
    switch_default,
    call_return,
    exceptional,
};

struct cfg_edge {
    cfg_block_id from;
    cfg_block_id to;
    edge_kind kind;
    std::optional<expression> condition;
    result<void> validate() const;
};

struct basic_block {
    cfg_block_id id;
    function_realization_id function;
    std::vector<std::string> instructions;
    result<void> validate() const;
};

struct function_graph {
    function_realization function;
    cfg_block_id entry;
    std::vector<cfg_block_id> normal_exits;
    std::vector<cfg_block_id> exceptional_exits;
    std::vector<basic_block> blocks;
    std::vector<cfg_edge> edges;
    result<void> validate() const;
};
```

### 6.3 Calls

```cpp
enum class call_kind : std::uint8_t {
    direct,
    virtual_call,
    function_pointer,
    member_pointer,
    callback,
    external_model,
    intrinsic,
};

struct call_target {
    function_realization_id function;
    expression applicability;
    std::vector<std::string> evidence;
};

struct call_target_set {
    std::string id;
    call_kind kind;
    std::vector<call_target> targets;
    bool complete;
    std::vector<std::string> closure_evidence;
    std::vector<std::string> unresolved;
    result<void> validate() const;
};
```

`complete == true` requires nonempty target set unless the call is proved unreachable.

### 6.4 Memory objects

```cpp
enum class storage_kind : std::uint8_t {
    automatic,
    static_storage,
    thread_storage,
    dynamic_allocation,
    subobject,
    external_object,
    abstract_container_payload,
};

struct object_layout {
    expression extent_bytes;
    expression alignment_bytes;
    std::optional<expression> element_count;
    std::optional<expression> element_size;
    std::vector<memory_object_id> subobjects;
    result<void> validate() const;
};

struct memory_object {
    memory_object_id id;
    storage_kind storage;
    function_realization_id owner_function;
    std::string source;
    object_layout layout;
    std::string provenance_root;
    product_condition presence;
    result<void> validate() const;
};
```

### 6.5 Accesses

```cpp
enum class access_kind : std::uint8_t {
    load,
    store,
    atomic_load,
    atomic_store,
    copy_source,
    copy_destination,
    fill,
    compare,
    pointer_formation,
};

struct memory_access {
    memory_access_id id;
    function_realization_id function;
    cfg_block_id block;
    access_kind kind;
    ir_value_id pointer;
    std::optional<memory_object_id> static_base;
    expression offset_bytes;
    expression width_bytes;
    std::string source;
    std::string language_operation;
    product_condition presence;
    result<void> validate() const;
};
```

---

## 7. Function summary API

```cpp
struct memory_footprint {
    std::vector<memory_object_id> reads;
    std::vector<memory_object_id> writes;
    std::vector<expression> read_ranges;
    std::vector<expression> write_ranges;
    result<void> validate() const;
};

struct alias_relation {
    ir_value_id output;
    ir_value_id input;
    expression offset;
};

struct object_state_update {
    memory_object_id object;
    std::string projection;
    expression value;
};

struct function_summary {
    function_realization_id function;
    expression required_precondition;
    expression guaranteed_postcondition;
    memory_footprint footprint;
    std::vector<alias_relation> aliases;
    std::vector<object_state_update> updates;
    std::vector<obligation_id> discharged_obligations;
    std::vector<std::string> invariant_artifacts;
    std::vector<std::string> model_dependencies;
    std::vector<std::string> unresolved;
    std::string summary_semantics;
    std::string digest;
    result<void> validate() const;
};
```

Builder:

```cpp
class summary_builder {
public:
    explicit summary_builder(function_realization_id function);

    result<void> require(expression predicate);
    result<void> guarantee(expression predicate);
    result<void> add_read(memory_object_id object, expression range);
    result<void> add_write(memory_object_id object, expression range);
    result<void> add_alias(alias_relation relation);
    result<void> add_update(object_state_update update);
    result<void> discharge(obligation_id obligation);
    result<void> add_invariant(std::string artifact);
    result<void> add_unresolved(std::string reason);

    result<function_summary> finish() &&;
};
```

---

## 8. Abstract analysis API

### 8.1 Abstract values and state

```cpp
struct integer_interval {
    integer_type type;
    std::vector<std::byte> lower;
    std::vector<std::byte> upper;
    bool wraps;
    result<void> validate() const;
};

struct pointer_abstract_value {
    std::vector<memory_object_id> objects;
    expression offset;
    bool target_set_complete;
    std::vector<std::string> unresolved;
};

using abstract_value = std::variant<
    integer_interval,
    pointer_abstract_value,
    expression>;

struct abstract_state {
    std::map<ir_value_id, abstract_value, std::less<>> values;
    std::map<std::string, expression, std::less<>> object_projections;
    expression path_predicate;
    std::vector<std::string> assumptions;
    std::vector<std::string> unresolved;
    result<void> validate() const;
};
```

### 8.2 Transfer and domain interfaces

```cpp
class abstract_domain {
public:
    virtual ~abstract_domain() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view semantic_digest() const noexcept = 0;

    virtual result<abstract_state>
    join(const abstract_state& left, const abstract_state& right) const = 0;

    virtual result<abstract_state>
    widen(const abstract_state& previous, const abstract_state& next) const = 0;

    virtual result<abstract_state>
    narrow(const abstract_state& previous, const abstract_state& next) const = 0;

    virtual result<bool>
    implies(const abstract_state& state, const expression& predicate) const = 0;
};
```

Transfer:

```cpp
class transfer_semantics {
public:
    virtual ~transfer_semantics() = default;

    virtual result<abstract_state>
    execute_instruction(
        const program_model& model,
        std::string_view instruction_id,
        abstract_state input) const = 0;

    virtual result<abstract_state>
    apply_summary(
        const function_summary& summary,
        std::span<const ir_value_id> actuals,
        abstract_state input) const = 0;
};
```

### 8.3 Summary engine

```cpp
struct summary_request {
    function_realization_id function;
    product_scope_id scope;
    std::string property_pack;
    std::string precision_profile;
    std::vector<function_summary> callee_summaries;
    std::vector<contract> contracts;
    std::vector<model_id> models;
    result<void> validate() const;
};

class summary_engine {
public:
    result<function_summary>
    compute(const summary_request& request,
            const analysis_limits& limits,
            cancellation_probe* cancellation = nullptr) const;
};
```

---

## 9. Bounds property pack

```cpp
enum class boundary_kind : std::uint8_t {
    lower,
    upper,
    width_overflow,
    unknown_object,
    provenance,
    subobject,
};

struct bounds_obligation {
    obligation_id id;
    memory_access_id access;
    expression object_known;
    expression provenance_valid;
    expression lower_bound;
    expression addition_defined;
    expression upper_bound;
    expression language_subobject_rule;
    std::string property_semantics;
    result<void> validate() const;

    expression conjunction() const;
};
```

Generator:

```cpp
class bounds_obligation_generator {
public:
    result<std::vector<bounds_obligation>>
    generate(const program_model& model,
             const product_scope& scope) const;
};
```

Callee contract logic:

```cpp
struct implementation_obligation {
    function_realization_id function;
    expression assumed_precondition;
    std::vector<bounds_obligation> bounds;
};

struct call_contract_obligation {
    obligation_id id;
    std::string call_site;
    function_realization_id caller;
    function_realization_id callee;
    expression caller_reachable_state;
    expression instantiated_precondition;
    call_target_set targets;
};
```

---

## 10. Proof orchestration API

### 10.1 Result classes

```cpp
enum class proof_status : std::uint8_t {
    proved_safe_closed,
    proved_safe_subset,
    proved_oob,
    reachable_contract_violation,
    callee_contract_insufficient,
    runtime_observed_oob,
    unreachable_proved,
    unresolved_model,
    unresolved_target,
    unresolved_environment,
    unresolved_concurrency,
    unresolved_other_ub,
    unresolved_budget,
    unsupported_semantics,
    failed_before_result,
};
```

### 10.2 Backend contract

```cpp
struct proof_request {
    product_scope scope;
    std::string model_artifact;
    std::vector<obligation_id> obligations;
    std::vector<function_summary> summaries;
    std::vector<contract> contracts;
    std::string precision_profile;
    analysis_limits limits;
    result<void> validate() const;
};

struct counterexample_step {
    function_realization_id function;
    cfg_block_id block;
    std::string instruction;
    std::map<std::string, std::string, std::less<>> values;
};

struct backend_result {
    proof_status status;
    std::vector<obligation_id> discharged;
    std::vector<obligation_id> violated;
    std::vector<counterexample_step> witness;
    std::vector<std::string> invariant_artifacts;
    std::vector<std::string> coverage;
    std::vector<std::string> unresolved;
    std::string certificate_fragment;
    std::string backend_id;
    std::string backend_semantics;
    std::string backend_binary_digest;
    resource_report resources;
    result<void> validate() const;
};

class proof_backend {
public:
    virtual ~proof_backend() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view semantic_digest() const noexcept = 0;
    virtual bool supports(const proof_request& request) const noexcept = 0;

    virtual result<backend_result>
    prove(const proof_request& request,
          cancellation_probe* cancellation = nullptr) = 0;
};
```

### 10.3 Orchestrator

```cpp
struct refinement_policy {
    std::vector<std::string> backend_order;
    std::uint64_t maximum_path_splits;
    std::uint64_t maximum_summary_specializations;
    std::uint64_t maximum_inline_depth;
    result<void> validate() const;
};

class proof_orchestrator {
public:
    result<proof_bundle>
    run(const proof_request& request,
        const refinement_policy& policy,
        std::span<proof_backend* const> backends,
        cancellation_probe* cancellation = nullptr) const;
};
```

Invariant: backend `unknown` never overwrites a stronger checked result.

---

## 11. Certificate API

### 11.1 Envelope

```cpp
enum class assurance_level : std::uint8_t {
    analysis_only,
    concrete_replay,
    invariant_checked,
    proof_object_checked,
    differential_checked,
};

struct proof_certificate {
    certificate_id id;
    product_scope_id scope;
    std::string source_snapshot;
    std::string model_artifact_digest;
    std::vector<obligation_id> obligations;
    proof_status status;
    assurance_level assurance;
    std::vector<std::string> closure_evidence;
    std::vector<counterexample_step> witness;
    std::vector<std::string> invariant_artifacts;
    std::vector<std::string> model_dependencies;
    std::vector<std::string> assumptions;
    std::vector<std::string> unresolved;
    std::string analysis_semantics;
    std::string checker_profile;
    std::string canonical_digest;
    result<void> validate_structure() const;
};
```

### 11.2 Checker

```cpp
struct checker_result {
    certificate_id certificate;
    bool accepted;
    assurance_level assurance;
    std::vector<std::string> checked_rules;
    std::vector<std::string> errors;
    std::string checker_id;
    std::string checker_semantics;
    std::string checker_binary_digest;
    std::string result_digest;
    result<void> validate() const;
};

class certificate_checker {
public:
    result<checker_result>
    check(const proof_certificate& certificate,
          const program_model& model,
          const model_registry& models,
          solver_port& solver) const;
};
```

Checker does not call analysis heuristics. It performs:

- canonical/identity validation
- product-scope binding
- complete target/access set validation
- witness deterministic replay
- first-UB validation
- invariant initiation/consecution/property checks
- call-site implication checks
- closure requirements
- assumption/model trust checks

---

## 12. Top-level orchestration

```cpp
struct verification_request {
    product_scope scope;
    std::string source_snapshot_id;
    std::string prior_result_snapshot_id;
    std::string property_pack{"ooblens.bounds.v1"};
    refinement_policy refinement;
    analysis_limits limits;
    report_options report;
    result<void> validate() const;
};

struct verification_result {
    std::string semantic_snapshot_id;
    std::string publication_id;
    std::vector<proof_result_id> results;
    std::vector<certificate_id> certificates;
    std::vector<closure_dimension_result> closure;
    bool product_bounds_closed;
    std::vector<std::string> unresolved;
    resource_report resources;
    result<void> validate() const;
};

class verification_service {
public:
    result<verification_result>
    verify(const verification_request& request,
           product_model_source& source,
           artifact_store& artifacts,
           model_registry& models,
           std::span<proof_backend* const> backends,
           certificate_checker& checker,
           finding_publisher& publisher,
           cancellation_probe* cancellation = nullptr) const;
};
```

Canonical high-level flow:

```cpp
result<verification_result>
verify_product_bounds(const verification_request&);
```

No convenience overload silently supplies scope, model, trust or fallback.

---

## 13. Ports

```cpp
class product_model_source {
public:
    virtual result<product_input_bundle>
    load(const verification_request&) = 0;
};

class artifact_store {
public:
    virtual result<artifact_digest>
    put(std::span<const std::byte> canonical_content) = 0;

    virtual result<std::vector<std::byte>>
    get(artifact_digest digest) const = 0;
};

class solver_port {
public:
    virtual result<solver_response>
    solve(const solver_request&, cancellation_probe*) = 0;
};

class model_registry {
public:
    virtual result<semantic_model>
    resolve(model_id id) const = 0;

    virtual result<void>
    validate_dependency_set(std::span<const model_id>) const = 0;
};

class finding_publisher {
public:
    virtual result<std::string>
    publish(const checked_finding_batch&) = 0;
};

class report_sink {
public:
    virtual result<void>
    write(std::string_view logical_path,
          std::span<const std::byte> content) = 0;
};
```

Filesystem/process/time/hash are not called outside port implementations.

---

## 14. cxxlens relation proposal

These relations are initially owned by `ooblens.*`, except candidate build/cc relations explicitly marked.

### 14.1 `ooblens.product_scope.v1`

Key: `scope`

Columns:

```text
scope: product_scope_id
catalog: catalog_id
link_units: set<link_unit_id>
entrypoint_set_digest: digest
input_model_set_digest: digest
external_model_set_digest: digest
target_semantics: typed_id<target_semantics_id>
condition: condition_ref
profile: open_symbol<ooblens.verification-profile/1>
trust_policy: digest
resource_policy: digest
```

### 14.2 `ooblens.entry_point.v1`

Key: `entry`

```text
entry: entry_point_id
scope: product_scope_id
function: function_realization_id
input_predicate: predicate_id
environment_models: set<model_id>
source: optional<source_span_id>
```

### 14.3 `ooblens.contract.v1`

Key: `contract`

```text
contract: contract_id
function: function_realization_id
kind: closed_symbol<ooblens.contract-kind/1>
authority: closed_symbol<ooblens.contract-authority/1>
predicate: predicate_id
adapter: typed_id<contract_adapter_id>
source: optional<source_span_id>
artifact: artifact_digest
```

### 14.4 `ooblens.function_summary.v1`

Key: `summary`

```text
summary: function_summary_id
function: function_realization_id
scope: product_scope_id
required_precondition: predicate_id
guaranteed_postcondition: predicate_id
footprint_digest: digest
update_digest: digest
invariant_artifacts: set<artifact_digest>
model_dependencies: set<model_id>
unresolved: set<unresolved_id>
summary_semantics: digest
```

### 14.5 `ooblens.memory_access.v1`

Key: `access`

```text
access: memory_access_id
function: function_realization_id
source: source_span_id
kind: open_symbol<ooblens.access-kind/1>
operation: open_symbol<ooblens.language-operation/1>
static_base: optional<memory_object_id>
offset: predicate_id
width: predicate_id
model_artifact: artifact_digest
```

### 14.6 `ooblens.bounds_obligation.v1`

Key: `obligation`

```text
obligation: obligation_id
access: memory_access_id
scope: product_scope_id
predicate: predicate_id
property_semantics: digest
required_models: set<model_id>
```

### 14.7 `ooblens.call_contract_obligation.v1`

Key: `obligation`

```text
obligation: obligation_id
call: cc_call_id-or-ooblens_call_id
caller: function_realization_id
callee: function_realization_id
contract: contract_id
instantiated_predicate: predicate_id
target_set_digest: digest
target_set_complete: bool
```

### 14.8 `ooblens.proof_result.v1`

Key: `result`

```text
result: proof_result_id
scope: product_scope_id
obligation: obligation_id
status: closed_symbol<ooblens.proof-status/1>
certificate: optional<certificate_id>
assurance: closed_symbol<ooblens.assurance-level/1>
analysis_execution: typed_id<analysis_execution_id>
checker_execution: optional<typed_id<checker_execution_id>>
unresolved: set<unresolved_id>
```

### 14.9 `ooblens.certificate.v1`

Key: `certificate`

```text
certificate: certificate_id
scope: product_scope_id
status: closed_symbol<ooblens.proof-status/1>
artifact: artifact_digest
artifact_digest: digest
checker_profile: typed_id<checker_profile_id>
assurance: closed_symbol<ooblens.assurance-level/1>
model_dependencies: set<model_id>
```

### 14.10 `ooblens.product_closure.v1`

Key: `(scope, property_semantics)`

```text
scope: product_scope_id
property_semantics: digest
access_set_digest: digest
target_set_digest: digest
entrypoint_set_digest: digest
model_set_digest: digest
proof_result_set_digest: digest
checker_profile: checker_profile_id
closed: bool
dimension_results_artifact: artifact_digest
```

### 14.11 Candidate cxxlens relations

Not added by this proposal:

```text
build.link_unit
build.link_input
build.entry_point
cc.contract
cc.contract_binding
```

Until accepted, equivalent data remains under `ooblens.*`.

---

## 15. cxxlens adapter classes

```cpp
class cxxlens_product_model_source final : public product_model_source {
public:
    static result<cxxlens_product_model_source>
    bind(cxxlens::sdk::snapshot_handle snapshot,
         cxxlens_adapter_options options);

    result<product_input_bundle>
    load(const verification_request&) override;
};

class cxxlens_finding_publisher final : public finding_publisher {
public:
    static result<cxxlens_finding_publisher>
    open(cxxlens::sdk::snapshot_store& store,
         cxxlens::sdk::relation_engine engine,
         publication_options options);

    result<std::string>
    publish(const checked_finding_batch&) override;
};
```

Adapter preserves:

- exact input snapshot ID
- consumed partition content digests
- coverage/unresolved
- conditions/interpretation
- producer semantics
- guarantee/assumptions/verification modalities
- memory/SQLite parity

---

## 16. Clang 22 provider design

```cpp
class clang22_product_extractor {
public:
    result<extraction_bundle>
    extract(cxxlens::provider::clang22::borrowed_translation_unit& unit,
            const extraction_request& request);
};
```

Extraction outputs detached facts/artifacts only.

Sub-extractors:

```text
declaration/type mapper
contract adapter
source/macro mapper
Clang CFG lowerer
call/dispatch extractor
object/layout extractor
memory-access extractor
library-call recognizer
debug/source binding
unsupported detector
```

No AST pointer survives callback.

Provider output groups:

```text
product source facts
verification IR artifact manifest
access inventory
contract inventory
extraction coverage
unresolved/unsupported
```

---

## 17. Configuration schema proposal

`ooblens.yaml` conceptual form:

```yaml
schema: ooblens.configuration.v1

product:
  catalog: build/compile_commands.json
  link_units:
    - id: app
      output: build/app
      dynamic_loading: forbidden

entrypoints:
  - function: app::main
    inputs:
      argc: {range: [0, 1024]}
      argv: {model: posix.argv.v1}

  - function: api::handle_request
    condition: server
    inputs:
      request: {model: company.request.v3}

contracts:
  adapters:
    - id: company.expects.v1
      macro: COMPANY_EXPECTS
    - id: sidecar.v1
      files: [contracts/**/*.json]

models:
  standard_library: libcxx-22-cxx23.v1
  external:
    - models/sql_client.yaml
    - models/network_stack.yaml

verification:
  profile: ooblens.closed-inductive.v1
  property: ooblens.bounds.v1
  target_closure: required
  concurrency: thread-confined-or-unresolved
  assurance: invariant_checked

budgets:
  wall_ms: 3600000
  memory_bytes: 17179869184
  solver_calls: 100000
  path_partitions: 1000000
```

All relative paths are resolved against a logical project root and canonicalized without becoming semantic identity.

---

## 18. Error taxonomy

### Input/scope

```text
ooblens.scope-invalid
ooblens.link-unit-open
ooblens.entrypoint-missing
ooblens.input-domain-missing
ooblens.condition-universe-mismatch
ooblens.target-semantics-mismatch
```

### Contracts/models

```text
ooblens.contract-invalid
ooblens.contract-adapter-unsupported
ooblens.contract-binding-invalid
ooblens.model-not-found
ooblens.model-invalid
ooblens.model-trust-rejected
ooblens.model-dependency-cycle
```

### Extraction/model

```text
ooblens.ir-invalid
ooblens.cfg-invalid
ooblens.call-target-incomplete
ooblens.memory-object-invalid
ooblens.memory-access-invalid
ooblens.pointer-provenance-unresolved
ooblens.unsupported-native-semantics
```

### Analysis

```text
ooblens.summary-invalid
ooblens.fixpoint-budget
ooblens.path-budget
ooblens.backend-unsupported
ooblens.backend-failed
ooblens.backend-result-invalid
ooblens.other-ub-precedes-property
```

### Proof/checker

```text
ooblens.certificate-invalid
ooblens.certificate-scope-mismatch
ooblens.certificate-model-mismatch
ooblens.witness-replay-failed
ooblens.first-ub-invalid
ooblens.invariant-not-inductive
ooblens.call-contract-not-proved
ooblens.closure-incomplete
ooblens.checker-result-invalid
```

### Publication/report

```text
ooblens.publication-rejected
ooblens.prior-head-stale
ooblens.artifact-digest-mismatch
ooblens.report-input-unchecked
ooblens.report-render-failed
```

Error control never depends on diagnostic prose substring.

---

## 19. Lifetime and threading

| Type | Lifetime/threading |
| --- | --- |
| immutable value types | own detached data; concurrent-readable |
| builders | move-only or thread-confined |
| `program_model` | immutable shared ownership |
| cursors | thread-confined; view expires on advance |
| backend request/result | own detached data |
| cancellation probe | synchronously borrowed, noexcept |
| solver port | implementation-defined concurrency; request isolated |
| certificate checker | reentrant if solver port supports it |
| report renderer | pure over checked certificate |
| Clang borrowed TU | callback-scoped, never stored/moved |
| cxxlens snapshot handle | pins immutable generation |
| publication writer | thread-confined transaction |

---

## 20. Deterministic ordering

Canonical order:

```text
ID bytes
then descriptor/version
then source snapshot/span
then semantic payload
```

Never use:

- map/hash insertion order
- Clang pointer order
- CFG allocation order without canonical renumbering
- solver symbol order
- worker completion order
- file discovery order

CFG blocks/instructions receive canonical IDs derived from function realization and normalized source/structural occurrence,
not process-local numbering.

---

## 21. Public entry commands

```text
ooblens scan --config ooblens.yaml
ooblens verify --snapshot <id>
ooblens explain --result <id>
ooblens report --snapshot <id> --output report/
ooblens status --snapshot <id>
ooblens model validate <file>
ooblens contract list --function <name>
ooblens proof-check <certificate>
```

Exit status:

```text
0: requested operation succeeded; policy threshold satisfied
1: proved violation exceeds configured threshold
2: incomplete/unresolved exceeds configured closure policy
3: invalid input/config/model/certificate
4: operational failure before sealed result
```

CLI exit status does not alter semantic result identity.
