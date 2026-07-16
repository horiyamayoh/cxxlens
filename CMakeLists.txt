cmake_minimum_required(VERSION 3.25)

project(
  cxxlens
  VERSION 0.1.0
  DESCRIPTION "C and C++ Semantic Relation Platform"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds are not supported. Use a CMake preset or a separate build directory."
  )
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CXXLENS_WARNINGS_AS_ERRORS "Treat cxxlens compiler warnings as errors"
       OFF)
option(CXXLENS_BUILD_SHARED "Build cxxlens as a shared library" OFF)
option(CXXLENS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling cxxlens" OFF)
option(CXXLENS_ENABLE_IWYU "Run include-what-you-use while compiling cxxlens"
       OFF)
option(CXXLENS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CXXLENS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CXXLENS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(CXXLENS_BUILD_DOCS "Enable Doxygen documentation targets" OFF)
option(CXXLENS_BUILD_QUALITY_TOOLS "Enable local quality targets" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CxxlensWarnings)
include(CxxlensSanitizers)
include(CxxlensClangTargets)

if(CXXLENS_BUILD_SHARED)
  set(CXXLENS_LIBRARY_TYPE SHARED)
else()
  set(CXXLENS_LIBRARY_TYPE STATIC)
endif()

add_library(
  cxxlens_provider_sdk
  ${CXXLENS_LIBRARY_TYPE}
  src/sdk/claim.cpp
  src/sdk/common.cpp
  src/sdk/provider.cpp
  src/sdk/query.cpp
  src/sdk/recipe.cpp
  src/sdk/relation.cpp
  src/sdk/store.cpp
  src/sdk/testing.cpp)
add_library(cxxlens::provider_sdk ALIAS cxxlens_provider_sdk)
target_compile_features(cxxlens_provider_sdk PUBLIC cxx_std_23)
target_include_directories(
  cxxlens_provider_sdk
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(
  cxxlens_provider_sdk
  PROPERTIES CXX_EXTENSIONS OFF
             EXPORT_NAME provider_sdk
             VERSION "${PROJECT_VERSION}"
             SOVERSION "${PROJECT_VERSION_MAJOR}")
cxxlens_set_project_warnings(cxxlens_provider_sdk)
cxxlens_enable_sanitizers(cxxlens_provider_sdk)
target_link_libraries(cxxlens_provider_sdk PRIVATE "${CMAKE_DL_LIBS}")

add_library(
  cxxlens
  ${CXXLENS_LIBRARY_TYPE}
  src/config/configuration.cpp
  src/config/configuration_spec.cpp
  src/config/environment_adapter.cpp
  src/config/yaml_loader.cpp
  src/core/canonical_encoding.cpp
  src/core/canonical_json.cpp
  src/core/evidence.cpp
  src/core/failure.cpp
  src/core/finding.cpp
  src/core/json_projections.cpp
  src/core/schema_registry.cpp
  src/core/version.cpp
  src/explain/explain.cpp
  src/facts/fact_contract.cpp
  src/facts/fact_profile.cpp
  src/facts/reducer.cpp
  src/graph/virtual_candidate_resolver.cpp
  src/llvm/common/borrowed_lifetime.cpp
  src/llvm/common/frontend_port.cpp
  src/llvm/common/frontend_worker_ipc.cpp
  src/llvm/clang22/frontend_job.cpp
  src/llvm/clang22/provider_sdk.cpp
  src/llvm/clang22/frontend_worker.cpp
  src/llvm/clang22/call_relation_extractor.cpp
  src/llvm/clang22/preprocessor_extractor.cpp
  src/llvm/clang22/source_map_adapter.cpp
  src/llvm/clang22/symbol_type_extractor.cpp
  src/runtime/fault_plan.cpp
  src/runtime/dynamic_library_adapter.cpp
  src/runtime/filesystem_adapter.cpp
  src/runtime/hash_adapter.cpp
  src/runtime/process_adapter.cpp
  src/runtime/time_adapter.cpp
  src/query/query_executor.cpp
  src/query/query_plan.cpp
  src/query/predicate_evaluator.cpp
  src/search/search.cpp
  src/search/search_report.cpp
  src/select/selector.cpp
  src/select/selector_json.cpp
  src/source/source_span.cpp
  src/store/binary_codec.cpp
  src/store/fact_values.cpp
  src/store/in_memory/in_memory_store.cpp
  src/store/ng_legacy_fact_store_adapter.cpp
  src/store/sqlite/sqlite_api.cpp
  src/store/sqlite/sqlite_store.cpp
  src/store/store_port.cpp
  src/testing/fault_plan.cpp
  src/testing/json_parser.cpp
  src/testing/result_assertion.cpp
  src/testing/workspace_fixture.cpp
  src/workspace/catalog.cpp
  src/workspace/frontend_scheduler_worker.cpp
  src/workspace/provisioning.cpp
  src/workspace/scheduler.cpp)
add_library(cxxlens::cxxlens ALIAS cxxlens)

target_compile_features(cxxlens PUBLIC cxx_std_23)
target_include_directories(
  cxxlens PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                 "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(
  cxxlens
  PROPERTIES CXX_EXTENSIONS OFF
             EXPORT_NAME cxxlens
             VERSION "${PROJECT_VERSION}"
             SOVERSION "${PROJECT_VERSION_MAJOR}")

cxxlens_set_project_warnings(cxxlens)
cxxlens_enable_sanitizers(cxxlens)
cxxlens_configure_clang22(cxxlens)

add_library(cxxlens_clang22_provider_sdk INTERFACE)
add_library(cxxlens::clang22_provider_sdk ALIAS cxxlens_clang22_provider_sdk)
target_link_libraries(cxxlens_clang22_provider_sdk
                      INTERFACE cxxlens::cxxlens cxxlens::provider_sdk)
target_compile_features(cxxlens_clang22_provider_sdk INTERFACE cxx_std_23)
set_target_properties(cxxlens_clang22_provider_sdk
                      PROPERTIES EXPORT_NAME clang22_provider_sdk)

add_executable(cxxlens-provider-scaffold tools/sdk/provider_scaffold_main.cpp)
target_link_libraries(cxxlens-provider-scaffold PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-provider-scaffold PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-provider-scaffold)
cxxlens_enable_sanitizers(cxxlens-provider-scaffold)

add_executable(cxxlens-sdk-doctor tools/sdk/sdk_doctor_main.cpp)
target_link_libraries(cxxlens-sdk-doctor PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-sdk-doctor PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-sdk-doctor)
cxxlens_enable_sanitizers(cxxlens-sdk-doctor)

add_executable(cxxlens-frontend-worker
               src/llvm/clang22/frontend_worker_main.cpp)
target_link_libraries(cxxlens-frontend-worker PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-frontend-worker
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens-frontend-worker PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-frontend-worker)
cxxlens_enable_sanitizers(cxxlens-frontend-worker)

if(CXXLENS_ENABLE_CLANG_TIDY)
  find_program(CXXLENS_CLANG_TIDY NAMES clang-tidy-22 clang-tidy REQUIRED)
  set_target_properties(
    cxxlens PROPERTIES CXX_CLANG_TIDY
                       "${CXXLENS_CLANG_TIDY};--warnings-as-errors=*")
endif()

if(CXXLENS_ENABLE_IWYU)
  find_program(CXXLENS_IWYU NAMES include-what-you-use iwyu REQUIRED)
  set_target_properties(cxxlens PROPERTIES CXX_INCLUDE_WHAT_YOU_USE
                                           "${CXXLENS_IWYU}")
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(CXXLENS_BUILD_QUALITY_TOOLS OR CXXLENS_BUILD_DOCS)
  include(CxxlensDeveloperTools)
endif()

install(
  TARGETS cxxlens cxxlens-frontend-worker
  EXPORT cxxlensTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  TARGETS cxxlens_provider_sdk
  EXPORT cxxlensProviderSDKTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS cxxlens-provider-scaffold cxxlens-sdk-doctor
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  PROGRAMS tools/sdk/relation_idl_compiler.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  RENAME cxxlens-relation-idl)
install(TARGETS cxxlens_clang22_provider_sdk
        EXPORT cxxlensClang22ProviderSDKTargets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  FILES schemas/cxxlens.config.explain.schema.yaml
        schemas/cxxlens.config.resolved.schema.yaml
        schemas/cxxlens.config.schema.yaml
        schemas/cxxlens.agent-task-packet-corpus.v1.json
        schemas/cxxlens.agent-task-packet-validation-report.v1.json
        schemas/cxxlens.agent-task-packet-validation-report.v1.schema.yaml
        schemas/cxxlens.agent-task-packet.v1.schema.yaml
        schemas/cxxlens.agent-ownership.v1.json
        schemas/cxxlens.agent-ownership.v1.schema.yaml
        schemas/cxxlens.dependency-request.examples.v1.json
        schemas/cxxlens.dependency-request.v1.schema.yaml
        schemas/cxxlens.api-ready.report.v1.json
        schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml
        schemas/cxxlens.api-ready.v1.schema.yaml
        schemas/cxxlens.readiness.authorization.v1.json
        schemas/cxxlens.readiness.authorization.v1.schema.yaml
        schemas/cxxlens_agent_task_card.schema.yaml
        schemas/cxxlens_public_api_contract.yaml
        schemas/cxxlens_api_catalog.schema.yaml
        schemas/cxxlens_api_test_coverage.yaml
        schemas/cxxlens_api_test_coverage.schema.yaml
        schemas/cxxlens_api_test_coverage_report.json
        schemas/cxxlens_api_test_coverage_report.schema.yaml
        schemas/cxxlens_global_contract_conventions.yaml
        schemas/cxxlens_global_contract_conventions.schema.yaml
        schemas/cxxlens_package_contract_candidates.yaml
        schemas/cxxlens_package_contract_candidates.schema.yaml
        schemas/cxxlens_phase_b_contract_integration.yaml
        schemas/cxxlens_phase_b_contract_integration.schema.yaml
        schemas/cxxlens_public_api_contract_freeze.yaml
        schemas/cxxlens_public_api_contract_freeze.schema.yaml
        schemas/cxxlens_ng_authority_transition.yaml
        schemas/cxxlens_ng_authority_transition.schema.yaml
        schemas/cxxlens_ng_authority_transition_report.schema.yaml
        schemas/cxxlens_ng_catalog_bootstrap.schema.yaml
        schemas/cxxlens_ng_claim_envelope.schema.yaml
        schemas/cxxlens_ng_relation_registry.yaml
        schemas/cxxlens_ng_relation_registry.schema.yaml
        schemas/cxxlens_ng_relation_conformance_vectors.yaml
        schemas/cxxlens_ng_relation_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_relation_conformance_report.schema.yaml
        schemas/cxxlens_ng_logical_query_contract.yaml
        schemas/cxxlens_ng_logical_query_contract.schema.yaml
        schemas/cxxlens_ng_logical_query_ir.schema.yaml
        schemas/cxxlens_ng_query_conformance_vectors.yaml
        schemas/cxxlens_ng_query_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_query_conformance_report.schema.yaml
        schemas/cxxlens_ng_semantic_guarantee_contract.yaml
        schemas/cxxlens_ng_semantic_guarantee_contract.schema.yaml
        schemas/cxxlens_ng_semantic_metadata.schema.yaml
        schemas/cxxlens_ng_semantic_conformance_vectors.yaml
        schemas/cxxlens_ng_semantic_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_semantic_conformance_report.schema.yaml
        schemas/cxxlens_ng_snapshot_store_contract.yaml
        schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
        schemas/cxxlens_ng_snapshot_manifest.schema.yaml
        schemas/cxxlens_ng_store_conformance_vectors.yaml
        schemas/cxxlens_ng_store_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_store_conformance_report.schema.yaml
        schemas/cxxlens_ng_sqlite_store_contract.yaml
        schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
        schemas/cxxlens_ng_provider_protocol.yaml
        schemas/cxxlens_ng_provider_protocol.schema.yaml
        schemas/cxxlens_ng_provider_manifest.schema.yaml
        schemas/cxxlens_ng_provider_task.schema.yaml
        schemas/cxxlens_ng_provider_conformance_vectors.yaml
        schemas/cxxlens_ng_provider_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_provider_conformance_report.schema.yaml
        schemas/cxxlens_ng_provider_fuzz_corpus.yaml
        schemas/cxxlens_ng_provider_fuzz_corpus.schema.yaml
        schemas/cxxlens_ng_security_profile.schema.yaml
        schemas/cxxlens_ng_public_api_catalog.yaml
        schemas/cxxlens_ng_public_api_catalog.schema.yaml
        schemas/cxxlens_ng_acceptance_manifest.yaml
        schemas/cxxlens_ng_security_profile.yaml
        schemas/cxxlens_ng_namespace_registry.yaml
        schemas/cxxlens_ng_namespace_registry.schema.yaml
        schemas/cxxlens_ng_provider_certification_registry.yaml
        schemas/cxxlens_ng_provider_certification_registry.schema.yaml
        schemas/cxxlens_ng_provider_support_matrix.yaml
        schemas/cxxlens_ng_provider_support_matrix.schema.yaml
        schemas/cxxlens_ng_security_conformance_vectors.yaml
        schemas/cxxlens_ng_security_conformance_vectors.schema.yaml
        schemas/cxxlens_ng_security_conformance_report.schema.yaml
        schemas/cxxlens_ng_release_bundle.yaml
        schemas/cxxlens_ng_release_bundle.schema.yaml
        schemas/cxxlens_ng_compatibility_request.schema.yaml
        schemas/cxxlens_ng_compatibility_report.schema.yaml
        schemas/cxxlens_asset_migration_policy.yaml
        schemas/cxxlens_asset_migration_policy.schema.yaml
        schemas/cxxlens_asset_migration_ledger.json
        schemas/cxxlens_asset_migration_ledger.schema.yaml
        schemas/cxxlens_legacy_api_baseline.yaml
        schemas/cxxlens_legacy_api_baseline.schema.yaml
        schemas/cxxlens_contract_ownership.yaml
        schemas/cxxlens_contract_ownership.schema.yaml
        schemas/cxxlens_coverage.schema.yaml
        schemas/cxxlens_custom_fact.schema.yaml
        schemas/cxxlens_diagnostic.schema.yaml
        schemas/cxxlens_error.schema.yaml
        schemas/cxxlens_evidence_coverage_contract.yaml
        schemas/cxxlens_evidence.schema.yaml
        schemas/cxxlens_explanation.schema.yaml
        schemas/cxxlens_fact.schema.yaml
        schemas/cxxlens_fact_contract.yaml
        schemas/cxxlens_fact_contract.schema.yaml
        schemas/cxxlens_fact_conformance.yaml
        schemas/cxxlens_fact_conformance.schema.yaml
        schemas/cxxlens_fact_profile.schema.yaml
        schemas/cxxlens_frontend_batch.schema.yaml
        schemas/cxxlens_frontend_worker_ipc.contract.yaml
        schemas/cxxlens_failure_contract.yaml
        schemas/cxxlens_finding_contract.yaml
        schemas/cxxlens_finding.schema.yaml
        schemas/cxxlens_finding_set.schema.yaml
        schemas/cxxlens_identity_contract.yaml
        schemas/cxxlens_m0_acceptance_report.schema.yaml
        schemas/cxxlens_m0_completion.schema.yaml
        schemas/cxxlens_m0_completion.yaml
        schemas/cxxlens_m1_acceptance_report.schema.yaml
        schemas/cxxlens_m1_completion.schema.yaml
        schemas/cxxlens_m1_completion.yaml
        schemas/cxxlens_m2_acceptance_report.schema.yaml
        schemas/cxxlens_m2_completion.schema.yaml
        schemas/cxxlens_m2_completion.yaml
        schemas/cxxlens_observation.schema.yaml
        schemas/cxxlens_schema_compatibility.yaml
        schemas/cxxlens_source_span.schema.yaml
        schemas/cxxlens_scheduler_trace.schema.yaml
        schemas/cxxlens_sqlite_store_contract.schema.yaml
        schemas/cxxlens_sqlite_store_contract.yaml
        schemas/cxxlens_testing_fixture.schema.yaml
        schemas/cxxlens_unresolved.schema.yaml
        schemas/cxxlens_analysis_scope.schema.yaml
        schemas/cxxlens_capabilities.schema.yaml
        schemas/cxxlens_coverage_delta.schema.yaml
        schemas/cxxlens_fact_requirement.schema.yaml
        schemas/cxxlens_incremental_provisioning_contract.yaml
        schemas/cxxlens_provisioning_trace.schema.yaml
        schemas/cxxlens_query_plan.schema.yaml
        schemas/cxxlens_query_execution.schema.yaml
        schemas/cxxlens_query_trace.schema.yaml
        schemas/cxxlens_selector.schema.yaml
        schemas/cxxlens_selector_reason_codes.yaml
        schemas/cxxlens_search_report.schema.yaml
        schemas/cxxlens_search_options.schema.yaml
        schemas/cxxlens_graph.schema.yaml
        schemas/cxxlens_graph_options.schema.yaml
        schemas/cxxlens_rule.schema.yaml
        schemas/cxxlens_suppression.schema.yaml
        schemas/cxxlens_report_options.schema.yaml
        schemas/cxxlens_rendered_report.schema.yaml
        schemas/cxxlens_transform_options.schema.yaml
        schemas/cxxlens_edit_plan.schema.yaml
        schemas/cxxlens_apply_result.schema.yaml
        schemas/cxxlens_transform_transaction.yaml
        schemas/cxxlens_generation_plan.schema.yaml
        schemas/cxxlens_generation_result.schema.yaml
        schemas/cxxlens_method_spec.schema.yaml
        schemas/cxxlens_generation_options.schema.yaml
        schemas/cxxlens_flow_options.schema.yaml
        schemas/cxxlens_taint_report.schema.yaml
        schemas/cxxlens_resource_report.schema.yaml
        schemas/cxxlens_effect_summary.schema.yaml
        schemas/cxxlens_api_model_pack.schema.yaml
        schemas/cxxlens_review_diff.schema.yaml
        schemas/cxxlens_review_baseline.schema.yaml
        schemas/cxxlens_review_report.schema.yaml
        schemas/cxxlens_qa_profile.schema.yaml
        schemas/cxxlens_qa_report.schema.yaml
        schemas/cxxlens_virtual_candidate_resolution.schema.yaml
        schemas/cxxlens_workspace_doctor.schema.yaml
        schemas/cxxlens_workspace_context.schema.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxlens/schemas")
install(FILES README.md SECURITY.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens")
install(DIRECTORY examples/m2-flagship
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens/examples")
install(DIRECTORY examples/packages
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens/examples")
install(DIRECTORY examples/sdk
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens/examples")

configure_package_config_file(
  cmake/CxxlensConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/cxxlensConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlens")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(
  EXPORT cxxlensTargets
  FILE cxxlensTargets.cmake
  NAMESPACE cxxlens::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlens")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cxxlensConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/cxxlensConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlens")

configure_package_config_file(
  cmake/cxxlensProviderSDKConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(
  EXPORT cxxlensProviderSDKTargets
  FILE cxxlensProviderSDKTargets.cmake
  NAMESPACE cxxlens::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")

configure_package_config_file(
  cmake/cxxlensClang22ProviderSDKConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensClang22ProviderSDK")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(
  EXPORT cxxlensClang22ProviderSDKTargets
  FILE cxxlensClang22ProviderSDKTargets.cmake
  NAMESPACE cxxlens::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensClang22ProviderSDK")
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensClang22ProviderSDK")
