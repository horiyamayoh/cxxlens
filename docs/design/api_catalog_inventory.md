# Generated public API inventory

This file is generated from `schemas/cxxlens_public_api_contract.yaml`; do not edit rows manually.

| API ID | Package | Header | Symbol | Atomic unit | Implementation state |
|---|---|---|---|---|---|
| API-CFG-001 | configuration | <cxxlens/cxxlens.hpp> | configuration::defaults | AU-CFG-001 | unimplemented |
| API-CFG-002 | configuration | <cxxlens/cxxlens.hpp> | configuration::{load,load_nearest} | AU-CFG-002 | unimplemented |
| API-CFG-003 | configuration | <cxxlens/cxxlens.hpp> | configuration::{with_profile,overlay} | AU-CFG-003 | unimplemented |
| API-CFG-004 | configuration | <cxxlens/cxxlens.hpp> | configuration::{validate,resolved_json,explain} | AU-CFG-004 | unimplemented |
| API-COPY-001 | copy | <cxxlens/generate.hpp> | copy::public_surface | AU-COPY-001 | unimplemented |
| API-COPY-002 | copy | <cxxlens/generate.hpp> | copy::required_types | AU-COPY-002 | unimplemented |
| API-CORE-001 | core | <cxxlens/core.hpp> | versions() | AU-CORE-001 | conformant |
| API-CORE-002 | core | <cxxlens/core.hpp> | capabilities() | AU-CORE-002 | unimplemented |
| API-CORE-003 | core | <cxxlens/core.hpp> | evidence::{add,items,to_json,to_markdown} | AU-CORE-003 | unimplemented |
| API-CORE-004 | core | <cxxlens/core.hpp> | coverage_report::{complete,units,to_json,to_markdown} | AU-CORE-004 | unimplemented |
| API-CORE-005 | core | <cxxlens/core.hpp> | finding::{id,why,unresolved_items,explain,to_json} | AU-CORE-005 | unimplemented |
| API-CORE-006 | core | <cxxlens/core.hpp> | finding_set::{minimum_confidence,minimum_severity,to_json,to_markdown,to_sarif} | AU-CORE-006 | unimplemented |
| API-EXP-001 | explain | <cxxlens/explain.hpp> | explain::{selector,finding,coverage,edit_plan,generation_plan} | AU-EXP-001 | unimplemented |
| API-EXP-002 | explain | <cxxlens/explain.hpp> | explain::why_not_matched | AU-EXP-002 | unimplemented |
| API-EXP-003 | explain | <cxxlens/explain.hpp> | explain::{for_selector,for_rule,for_codemod,for_generation} | AU-EXP-003 | unimplemented |
| API-EXP-004 | explain | <cxxlens/explain.hpp> | explain::api_catalog_json | AU-EXP-004 | unimplemented |
| API-FACT-001 | facts | <cxxlens/facts.hpp> | fact_profile::{minimal,semantic_search,refactor,generation,flow,full,include,exclude,precision} | AU-FACT-001 | unimplemented |
| API-FACT-002 | facts | <cxxlens/facts.hpp> | fact_store::find(fact_query) | AU-FACT-002 | unimplemented |
| API-FACT-003 | facts | <cxxlens/facts.hpp> | fact_store::symbols() | AU-FACT-003 | unimplemented |
| API-FACT-004 | facts | <cxxlens/facts.hpp> | fact_store::references(symbol_id) | AU-FACT-004 | unimplemented |
| API-FACT-005 | facts | <cxxlens/facts.hpp> | fact_store::calls() | AU-FACT-005 | unimplemented |
| API-FACT-006 | facts | <cxxlens/facts.hpp> | fact_store::inheritance() | AU-FACT-006 | unimplemented |
| API-FACT-007 | facts | <cxxlens/facts.hpp> | fact_store::overrides() | AU-FACT-007 | unimplemented |
| API-FACT-008 | facts | <cxxlens/facts.hpp> | fact_store::includes() | AU-FACT-008 | unimplemented |
| API-FACT-009 | facts | <cxxlens/facts.hpp> | fact_store::macros() | AU-FACT-009 | unimplemented |
| API-FACT-010 | facts | <cxxlens/facts.hpp> | fact_store::coverage(fact_profile,analysis_scope) | AU-FACT-010 | unimplemented |
| API-FLOW-001 | flow | <cxxlens/flow.hpp> | source_model factories | AU-FLOW-001 | unimplemented |
| API-FLOW-002 | flow | <cxxlens/flow.hpp> | sink_model factories | AU-FLOW-002 | unimplemented |
| API-FLOW-003 | flow | <cxxlens/flow.hpp> | barrier_model factories | AU-FLOW-003 | unimplemented |
| API-FLOW-004 | flow | <cxxlens/flow.hpp> | taint_policy builder family | AU-FLOW-004 | unimplemented |
| API-FLOW-005 | flow | <cxxlens/flow.hpp> | flow::run_taint | AU-FLOW-005 | unimplemented |
| API-FLOW-006 | flow | <cxxlens/flow.hpp> | resource_protocol builder family | AU-FLOW-006 | unimplemented |
| API-FLOW-007 | flow | <cxxlens/flow.hpp> | flow::check_resource_protocol | AU-FLOW-007 | unimplemented |
| API-FLOW-008 | flow | <cxxlens/flow.hpp> | flow::build_effect_summaries | AU-FLOW-008 | unimplemented |
| API-FUZZ-001 | fuzz | <cxxlens/generate.hpp> | fuzz::generator::for_function | AU-FUZZ-001 | unimplemented |
| API-FUZZ-002 | fuzz | <cxxlens/generate.hpp> | fuzz::generator::{input,infer_inputs,options} | AU-FUZZ-002 | unimplemented |
| API-FUZZ-003 | fuzz | <cxxlens/generate.hpp> | fuzz::generator::plan | AU-FUZZ-003 | unimplemented |
| API-GEN-001 | generate | <cxxlens/generate.hpp> | generation_plan::{census,decisions,artifacts,diagnostics,unresolved_items,coverage,preview,to_json,to_markdown} | AU-GEN-001 | unimplemented |
| API-GEN-002 | generate | <cxxlens/generate.hpp> | generation_plan::apply | AU-GEN-002 | unimplemented |
| API-GRAPH-001 | graph | <cxxlens/graph.hpp> | graph::class_hierarchy | AU-GRAPH-001 | unimplemented |
| API-GRAPH-002 | graph | <cxxlens/graph.hpp> | graph::override_graph | AU-GRAPH-002 | unimplemented |
| API-GRAPH-003 | graph | <cxxlens/graph.hpp> | graph::call_graph | AU-GRAPH-003 | unimplemented |
| API-GRAPH-004 | graph | <cxxlens/graph.hpp> | graph::include_graph | AU-GRAPH-004 | unimplemented |
| API-GRAPH-005 | graph | <cxxlens/graph.hpp> | impact_query builder and run | AU-GRAPH-005 | unimplemented |
| API-GRAPH-006 | graph | <cxxlens/graph.hpp> | semantic_graph::{subgraph,to_json,to_dot} | AU-GRAPH-006 | unimplemented |
| API-INT-001 | interop | <cxxlens/interop/clang.hpp> | interop::linked_clang_version | AU-INT-001 | unimplemented |
| API-INT-002 | interop | <cxxlens/interop/clang.hpp> | interop::with_translation_unit | AU-INT-002 | unimplemented |
| API-INT-003 | interop | <cxxlens/interop/clang.hpp> | fact_sink::{emit,emit_custom,emit_evidence,mark_partial} | AU-INT-003 | unimplemented |
| API-INT-004 | interop | <cxxlens/interop/clang.hpp> | interop::{register_extractor,unregister_extractor} | AU-INT-004 | unimplemented |
| API-METHOD-001 | method_harness | <cxxlens/generate.hpp> | method_harness::parse_method_spec | AU-METHOD-001 | unimplemented |
| API-METHOD-002 | method_harness | <cxxlens/generate.hpp> | method_harness::resolve_method | AU-METHOD-002 | unimplemented |
| API-METHOD-003 | method_harness | <cxxlens/generate.hpp> | method_harness::generator::{for_method,for_symbol,options} | AU-METHOD-003 | unimplemented |
| API-METHOD-004 | method_harness | <cxxlens/generate.hpp> | method_harness::generator::inspect | AU-METHOD-004 | unimplemented |
| API-METHOD-005 | method_harness | <cxxlens/generate.hpp> | method_harness::generator::plan | AU-METHOD-005 | unimplemented |
| API-MOCK-001 | mock | <cxxlens/generate.hpp> | mock::generator::{for_class,for_symbol,for_c_api_header} | AU-MOCK-001 | unimplemented |
| API-MOCK-002 | mock | <cxxlens/generate.hpp> | mock::generator::{options,include_method,exclude_method} | AU-MOCK-002 | unimplemented |
| API-MOCK-003 | mock | <cxxlens/generate.hpp> | mock::generator::plan | AU-MOCK-003 | unimplemented |
| API-MODEL-001 | models | <cxxlens/models.hpp> | api_model_pack::empty(id,version) | AU-MODEL-001 | unimplemented |
| API-MODEL-002 | models | <cxxlens/models.hpp> | api_model_pack::{function,method,replacement,source,sink,barrier} | AU-MODEL-002 | unimplemented |
| API-MODEL-003 | models | <cxxlens/models.hpp> | api_model_pack::merge | AU-MODEL-003 | unimplemented |
| API-MODEL-004 | models | <cxxlens/models.hpp> | api_model_pack::{load,save,to_json} | AU-MODEL-004 | unimplemented |
| API-QA-001 | qa | <cxxlens/qa.hpp> | qa::profile factory/builder family | AU-QA-001 | unimplemented |
| API-QA-002 | qa | <cxxlens/qa.hpp> | qa::workflow::{for_project,use,process_policy_,import_coverage,associate_with} | AU-QA-002 | unimplemented |
| API-QA-003 | qa | <cxxlens/qa.hpp> | qa::workflow::run | AU-QA-003 | unimplemented |
| API-QA-004 | qa | <cxxlens/qa.hpp> | qa::import_source_coverage | AU-QA-004 | unimplemented |
| API-REPORT-001 | report | <cxxlens/report.hpp> | report::render(finding_set) | AU-REPORT-001 | unimplemented |
| API-REPORT-002 | report | <cxxlens/report.hpp> | report::render(edit_plan) | AU-REPORT-002 | unimplemented |
| API-REPORT-003 | report | <cxxlens/report.hpp> | report::render(generation_plan) | AU-REPORT-003 | unimplemented |
| API-REPORT-004 | report | <cxxlens/report.hpp> | report::render(review_report) | AU-REPORT-004 | unimplemented |
| API-RULE-001 | rules | <cxxlens/rules.hpp> | rules::make_rule | AU-RULE-001 | unimplemented |
| API-RULE-002 | rules | <cxxlens/rules.hpp> | rule_builder::{metadata,when,unless,scope,severity_level,confidence_at_least,diagnose,note,fix,explain,build} | AU-RULE-002 | unimplemented |
| API-RULE-003 | rules | <cxxlens/rules.hpp> | rule::run | AU-RULE-003 | unimplemented |
| API-RULE-004 | rules | <cxxlens/rules.hpp> | rule_pack::{add,enable,disable,run} | AU-RULE-004 | unimplemented |
| API-RULE-005 | rules | <cxxlens/rules.hpp> | suppression_policy builder family | AU-RULE-005 | unimplemented |
| API-RV-001 | review | <cxxlens/review.hpp> | diff_view::{from_unified_diff,from_git,contains,changed_files,to_json} | AU-RV-001 | unimplemented |
| API-RV-002 | review | <cxxlens/review.hpp> | baseline::{load,save,contains_equivalent,to_json} | AU-RV-002 | unimplemented |
| API-RV-003 | review | <cxxlens/review.hpp> | gate_policy factory/builder family | AU-RV-003 | unimplemented |
| API-RV-004 | review | <cxxlens/review.hpp> | review::workflow builder family | AU-RV-004 | unimplemented |
| API-RV-005 | review | <cxxlens/review.hpp> | review::workflow::run | AU-RV-005 | unimplemented |
| API-SEL-001 | select | <cxxlens/select.hpp> | file_selector builder family | AU-SEL-001 | unimplemented |
| API-SEL-002 | select | <cxxlens/select.hpp> | symbol_selector builder family | AU-SEL-002 | unimplemented |
| API-SEL-003 | select | <cxxlens/select.hpp> | type_selector builder family | AU-SEL-003 | unimplemented |
| API-SEL-004 | select | <cxxlens/select.hpp> | expression_selector builder family | AU-SEL-004 | unimplemented |
| API-SEL-005 | select | <cxxlens/select.hpp> | reference_selector builder family | AU-SEL-005 | unimplemented |
| API-SEL-006 | select | <cxxlens/select.hpp> | call_selector builder family | AU-SEL-006 | unimplemented |
| API-SEL-007 | select | <cxxlens/select.hpp> | conversion/include/macro selector families | AU-SEL-007 | unimplemented |
| API-SEL-008 | select | <cxxlens/select.hpp> | any_symbol/function/method/record/variable/macro/type helpers | AU-SEL-008 | unimplemented |
| API-SEL-009 | select | <cxxlens/select.hpp> | calls_to/calls_to_function/calls_to_method helpers | AU-SEL-009 | unimplemented |
| API-SEL-010 | select | <cxxlens/select.hpp> | semantic_selector::{from_json,requirements,explain,to_json} | AU-SEL-010 | unimplemented |
| API-SEL-011 | select | <cxxlens/select.hpp> | semantic(typed_selector) | AU-SEL-011 | unimplemented |
| API-SRCH-001 | search | <cxxlens/search.hpp> | search::symbols | AU-SRCH-001 | unimplemented |
| API-SRCH-002 | search | <cxxlens/search.hpp> | search::references | AU-SRCH-002 | unimplemented |
| API-SRCH-003 | search | <cxxlens/search.hpp> | search::calls | AU-SRCH-003 | unimplemented |
| API-SRCH-004 | search | <cxxlens/search.hpp> | search::calls_to_function | AU-SRCH-004 | unimplemented |
| API-SRCH-005 | search | <cxxlens/search.hpp> | search::calls_to_method | AU-SRCH-005 | unimplemented |
| API-SRCH-006 | search | <cxxlens/search.hpp> | search::inheritance | AU-SRCH-006 | unimplemented |
| API-SRCH-007 | search | <cxxlens/search.hpp> | search::overrides | AU-SRCH-007 | unimplemented |
| API-SRCH-008 | search | <cxxlens/search.hpp> | search::includes | AU-SRCH-008 | unimplemented |
| API-SRCH-009 | search | <cxxlens/search.hpp> | search::macros | AU-SRCH-009 | unimplemented |
| API-SRCH-010 | search | <cxxlens/search.hpp> | search::conversions | AU-SRCH-010 | unimplemented |
| API-TEST-001 | testing | <cxxlens/testing.hpp> | workspace_fixture::{cpp,c,main_file,add_file,add_header,add_variant,standard,target,argument,generated,system_header,open} | AU-TEST-001 | unimplemented |
| API-TEST-002 | testing | <cxxlens/testing.hpp> | result_assertion builder/check | AU-TEST-002 | unimplemented |
| API-TEST-003 | testing | <cxxlens/testing.hpp> | edit_plan_assertion builder/check | AU-TEST-003 | unimplemented |
| API-TEST-004 | testing | <cxxlens/testing.hpp> | generation_plan_assertion builder/check | AU-TEST-004 | unimplemented |
| API-TEST-005 | testing | <cxxlens/testing.hpp> | testing::check_determinism | AU-TEST-005 | unimplemented |
| API-TEST-006 | testing | <cxxlens/testing.hpp> | testing::assert_schema_conforms | AU-TEST-006 | unimplemented |
| API-TF-001 | transform | <cxxlens/transform.hpp> | transform::replace_function_call | AU-TF-001 | unimplemented |
| API-TF-002 | transform | <cxxlens/transform.hpp> | transform::replace_method_call | AU-TF-002 | unimplemented |
| API-TF-003 | transform | <cxxlens/transform.hpp> | transform::rewrite_calls | AU-TF-003 | unimplemented |
| API-TF-004 | transform | <cxxlens/transform.hpp> | transform::rename_symbol | AU-TF-004 | unimplemented |
| API-TF-005 | transform | <cxxlens/transform.hpp> | transform::add_include_where_needed | AU-TF-005 | unimplemented |
| API-TF-006 | transform | <cxxlens/transform.hpp> | transform::remove_unused_includes | AU-TF-006 | unimplemented |
| API-TF-007 | transform | <cxxlens/transform.hpp> | codemod::plan | AU-TF-007 | unimplemented |
| API-TF-008 | transform | <cxxlens/transform.hpp> | edit_plan::preview_unified_diff | AU-TF-008 | unimplemented |
| API-TF-009 | transform | <cxxlens/transform.hpp> | edit_plan::apply | AU-TF-009 | unimplemented |
| API-WS-001 | workspace | <cxxlens/workspace.hpp> | workspace_options::from_compilation_database(path) | AU-WS-001 | unimplemented |
| API-WS-002 | workspace | <cxxlens/workspace.hpp> | workspace::open(workspace_options, execution_context) | AU-WS-002 | unimplemented |
| API-WS-003 | workspace | <cxxlens/workspace.hpp> | workspace::compile_units() | AU-WS-003 | unimplemented |
| API-WS-004 | workspace | <cxxlens/workspace.hpp> | workspace::command_for(path) | AU-WS-004 | unimplemented |
| API-WS-005 | workspace | <cxxlens/workspace.hpp> | workspace::ensure(fact_profile, analysis_scope, execution_context) | AU-WS-005 | unimplemented |
| API-WS-006 | workspace | <cxxlens/workspace.hpp> | workspace::facts() | AU-WS-006 | unimplemented |
| API-WS-007 | workspace | <cxxlens/workspace.hpp> | workspace::doctor(execution_context) | AU-WS-007 | unimplemented |
| API-WS-008 | workspace | <cxxlens/workspace.hpp> | analysis_scope::{all,files,compile_units,changed_files,include_headers,variants} | AU-WS-008 | unimplemented |
