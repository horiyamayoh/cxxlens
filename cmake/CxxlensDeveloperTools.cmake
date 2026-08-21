find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(CXXLENS_PUBLIC_CALLABLE_CLANG NAMES clang++-22 REQUIRED)

find_program(CXXLENS_CLANG_FORMAT NAMES clang-format-22 REQUIRED)
add_custom_target(
  cxxlens-format-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_format.py" --clang-format
    "${CXXLENS_CLANG_FORMAT}" --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-text-lint
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/run_text_lints.py" --root
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-documentation-consistency-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_documentation_consistency.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-store-candidate-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_store_candidate.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

foreach(
  contract IN
  ITEMS release_contract
        release_qualification
        clang22_materialization
        production_scope_closure
        design_feedback
        relation_contract
        query_contract
        semantic_guarantee
        snapshot_store_contract
        sqlite_store_contract
        provider_protocol
        provider_ng1_qualification
        security_contract
        provider_runtime)
  string(REPLACE "_" "-" target_suffix "${contract}")
  add_custom_target(
    "cxxlens-ng-${target_suffix}-check"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_${contract}.py" check
      --root "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
endforeach()

set(CXXLENS_NG1_QUALIFICATION_REPORT
    ""
    CACHE
      FILEPATH
      "Exact NG1 qualification report to validate when provider evidence is available"
)
set(CXXLENS_NG1_PROVIDER_BINARY
    ""
    CACHE
      FILEPATH
      "Host-measured NG1 provider executable to bind to the qualification report"
)
set(CXXLENS_NG1_PROVIDER_SEMANTIC_CONTRACT
    ""
    CACHE
      FILEPATH
      "Selected provider semantic contract to bind to the qualification report")
set(_cxxlens_ng1_report_inputs
    "${CXXLENS_NG1_QUALIFICATION_REPORT}${CXXLENS_NG1_PROVIDER_BINARY}${CXXLENS_NG1_PROVIDER_SEMANTIC_CONTRACT}"
)
if(_cxxlens_ng1_report_inputs)
  if(NOT CXXLENS_NG1_QUALIFICATION_REPORT
     OR NOT CXXLENS_NG1_PROVIDER_BINARY
     OR NOT CXXLENS_NG1_PROVIDER_SEMANTIC_CONTRACT)
    message(
      FATAL_ERROR
        "NG1 report mode requires report, provider binary, and semantic contract"
    )
  endif()
  add_custom_target(
    cxxlens-ng-provider-ng1-qualification-report-check
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_provider_ng1_qualification.py"
      report --root "${CMAKE_CURRENT_SOURCE_DIR}" --report
      "${CXXLENS_NG1_QUALIFICATION_REPORT}" --provider-binary
      "${CXXLENS_NG1_PROVIDER_BINARY}" --provider-semantic-contract
      "${CXXLENS_NG1_PROVIDER_SEMANTIC_CONTRACT}"
    VERBATIM)
endif()

add_custom_target(
  cxxlens-ng-ci-supply-chain-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ci_supply_chain.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-sdk-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_sdk_contract.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}" --compiler "${CMAKE_CXX_COMPILER}"
    --scaffold "$<TARGET_FILE:cxxlens-provider-scaffold>" --doctor
    "$<TARGET_FILE:cxxlens-sdk-doctor>"
  DEPENDS cxxlens-provider-scaffold cxxlens-sdk-doctor
  VERBATIM)

add_custom_target(
  cxxlens-ng-migration-completion-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_migration_completion.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-foundation-completion-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_foundation_completion.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-api-development-readiness-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_api_development_readiness.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-development-decision-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_development_decisions.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-work-unit-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_work_units.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-agent-context-v2-corpus-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_agent_context_v2.py"
    corpus --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-autonomy-constructibility-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_autonomy_constructibility.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-wip-inventory-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_wip_inventory.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-autonomy-ci-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_autonomy_ci.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-source-closure-transport-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_source_closure_transport.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-constructibility-gate-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_quality_ownership.py"
    constructibility --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-sqlite-store-v3-qualification-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_sqlite_store_v3_qualification.py"
    contract --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-ng-public-callable-inventory-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/public_callable_inventory.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}" --compiler
    "${CXXLENS_PUBLIC_CALLABLE_CLANG}"
  VERBATIM)

if(TARGET cxxlens-g5-runtime)
  add_custom_target(
    cxxlens-ng-g5-qualification-check
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_ng_g5_qualification.py"
      check --root "${CMAKE_CURRENT_SOURCE_DIR}" --runtime
      "$<TARGET_FILE:cxxlens-g5-runtime>"
    DEPENDS cxxlens-g5-runtime
    VERBATIM)
endif()

add_custom_target(
  cxxlens-sanitizer-coverage-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_sanitizer_coverage.py"
    contract --root "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_sanitizer_coverage.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}" --build-dir "${CMAKE_BINARY_DIR}"
    --expected "${CXXLENS_SANITIZER_EXPECTED}"
  VERBATIM)

add_custom_target(
  cxxlens-public-boundary-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_public_headers.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/cxxlens"
  VERBATIM)

add_custom_target(
  cxxlens-runtime-port-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_runtime_port_usage.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  VERBATIM)

add_custom_target(
  cxxlens-quality-ownership-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_quality_ownership.py" check
    --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-design-package-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/verify_checksums.py" check --root
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

find_program(CXXLENS_RUN_CLANG_TIDY NAMES run-clang-tidy-22 run-clang-tidy)
if(CXXLENS_RUN_CLANG_TIDY)
  add_custom_target(
    cxxlens-clang-tidy
    COMMAND
      "${CXXLENS_RUN_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" -config-file
      "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy" -header-filter
      "${CMAKE_CURRENT_SOURCE_DIR}/(include/cxxlens|src)/.*"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/.*"
    DEPENDS ${CXXLENS_COMPILED_TARGETS}
    VERBATIM)
endif()

add_custom_target(cxxlens-quality)
add_dependencies(
  cxxlens-quality
  cxxlens-design-package-check
  cxxlens-documentation-consistency-check
  cxxlens-ng-design-feedback-check
  cxxlens-ng-development-decision-check
  cxxlens-ng-source-closure-transport-check
  cxxlens-ng-clang22-materialization-check
  cxxlens-ng-provider-protocol-check
  cxxlens-ng-provider-ng1-qualification-check
  cxxlens-ng-provider-runtime-check
  cxxlens-ng-production-scope-closure-check
  cxxlens-ng-api-development-readiness-check
  cxxlens-ng-constructibility-gate-check
  cxxlens-ng-ci-supply-chain-check
  cxxlens-ng-migration-completion-check
  cxxlens-ng-query-contract-check
  cxxlens-ng-relation-contract-check
  cxxlens-ng-public-callable-inventory-check
  cxxlens-ng-release-contract-check
  cxxlens-ng-release-qualification-check
  cxxlens-ng-sdk-contract-check
  cxxlens-ng-security-contract-check
  cxxlens-ng-semantic-guarantee-check
  cxxlens-ng-snapshot-store-contract-check
  cxxlens-ng-sqlite-store-contract-check
  cxxlens-ng-sqlite-store-v3-qualification-check
  cxxlens-public-boundary-check
  cxxlens-quality-ownership-check
  cxxlens-runtime-port-check
  cxxlens-sanitizer-coverage-check
  cxxlens-text-lint)
if(TARGET cxxlens-ng-provider-ng1-qualification-report-check)
  add_dependencies(cxxlens-quality
                   cxxlens-ng-provider-ng1-qualification-report-check)
endif()
if(TARGET cxxlens-format-check)
  add_dependencies(cxxlens-quality cxxlens-format-check)
endif()
if(TARGET cxxlens-ng-g5-qualification-check)
  add_dependencies(cxxlens-quality cxxlens-ng-g5-qualification-check)
endif()
if(CXXLENS_BUILD_DOCS)
  find_package(Doxygen 1.9.8 REQUIRED)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)
  add_custom_target(
    docs
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${CMAKE_CURRENT_BINARY_DIR}/doxygen"
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
  add_custom_target(
    cxxlens-doxygen-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/verify_doxygen.py"
      "${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml" --ng-catalog
      "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_ng_public_api_catalog.yaml"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/public_callable_inventory.py"
      check-doxygen --root "${CMAKE_CURRENT_SOURCE_DIR}" --doxygen-xml
      "${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml"
    DEPENDS docs
    VERBATIM)
  add_dependencies(cxxlens-quality cxxlens-doxygen-contract)
endif()
