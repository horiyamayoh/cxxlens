cmake_minimum_required(VERSION 3.25)

project(
  cxxlens
  VERSION 0.1.0
  DESCRIPTION "Use-case-driven C and C++ semantic analysis library"
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
        schemas/cxxlens_virtual_candidate_resolution.schema.yaml
        schemas/cxxlens_workspace_doctor.schema.yaml
        schemas/cxxlens_workspace_context.schema.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxlens/schemas")
install(FILES README.md SECURITY.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens")
install(DIRECTORY examples/m2-flagship
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
