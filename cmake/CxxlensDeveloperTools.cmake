find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

find_program(CXXLENS_CLANG_FORMAT NAMES clang-format-22 clang-format)
if(CXXLENS_CLANG_FORMAT)
  add_custom_target(
    cxxlens-format-check
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_format.py" --clang-format
      "${CXXLENS_CLANG_FORMAT}" --root "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
else()
  message(
    WARNING "clang-format was not found; cxxlens-format-check is unavailable")
endif()

add_custom_target(
  cxxlens-api-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/validate_api_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_public_api_contract.yaml"
    --inventory
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/design/api_catalog_inventory.md"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/test_api_contract.py"
  VERBATIM)

add_custom_target(
  cxxlens-design-package-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/verify_checksums.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/design/SHA256SUMS"
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
  cxxlens-identity-path-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_identity_path.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-failure-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_failure_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-evidence-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_evidence_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-finding-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_finding_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-serialization-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_serialization_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-configuration-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_configuration_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-testing-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_testing_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-m0-completion-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_m0_completion.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-m1-completion-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_m1_completion.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-m2-completion-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_m2_completion.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-workspace-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_workspace_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-fact-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_fact_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-fact-reducer-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_fact_reducer.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-clang-adapter-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_clang_adapter.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-scheduler-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_scheduler_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-provisioning-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_provisioning_contract.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-preprocessor-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_preprocessor_extractor.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-semantic-extractor-contract-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_semantic_extractor.py"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(
  cxxlens-text-lint
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/run_text_lints.py" --root
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
    DEPENDS cxxlens
    VERBATIM)
endif()

add_custom_target(cxxlens-quality)
add_dependencies(
  cxxlens-quality
  cxxlens-api-contract-check
  cxxlens-clang-adapter-check
  cxxlens-configuration-contract-check
  cxxlens-design-package-check
  cxxlens-evidence-contract-check
  cxxlens-failure-contract-check
  cxxlens-fact-contract-check
  cxxlens-fact-reducer-contract-check
  cxxlens-finding-contract-check
  cxxlens-identity-path-check
  cxxlens-m0-completion-check
  cxxlens-m1-completion-check
  cxxlens-m2-completion-check
  cxxlens-public-boundary-check
  cxxlens-preprocessor-contract-check
  cxxlens-provisioning-contract-check
  cxxlens-runtime-port-check
  cxxlens-scheduler-contract-check
  cxxlens-semantic-extractor-contract-check
  cxxlens-serialization-contract-check
  cxxlens-testing-contract-check
  cxxlens-workspace-contract-check
  cxxlens-text-lint)
if(TARGET cxxlens-format-check)
  add_dependencies(cxxlens-quality cxxlens-format-check)
endif()
if(TARGET cxxlens-clang-tidy)
  add_dependencies(cxxlens-quality cxxlens-clang-tidy)
endif()

if(TARGET cxxlens-m0-test-binaries)
  add_custom_target(
    cxxlens-m0-acceptance
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}"
            --output-on-failure
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/run_m0_acceptance.py" --root
      "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --manifest
      "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_m0_completion.yaml" --report
      "${CMAKE_BINARY_DIR}/m0-acceptance-report.json" --compiler
      "${CMAKE_CXX_COMPILER}" --identity
      "$<TARGET_FILE:cxxlens-unit-canonical-identity>" --evidence
      "$<TARGET_FILE:cxxlens-unit-evidence-coverage>" --finding
      "$<TARGET_FILE:cxxlens-unit-finding-contract>" --serialization
      "$<TARGET_FILE:cxxlens-unit-m0-serialization>" --configuration
      "$<TARGET_FILE:cxxlens-unit-configuration-process>" --fixture
      "$<TARGET_FILE:cxxlens-unit-testing-fixture-process>"
    DEPENDS cxxlens-m0-test-binaries cxxlens-quality
    USES_TERMINAL VERBATIM)
endif()

if(TARGET cxxlens-m1-test-binaries)
  add_custom_target(
    cxxlens-m1-acceptance
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}"
            --output-on-failure
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/run_m1_acceptance.py" --root
      "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --manifest
      "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_m1_completion.yaml" --report
      "${CMAKE_BINARY_DIR}/m1-acceptance-report.json" --integration
      "$<TARGET_FILE:cxxlens-m1-conformance>" --scheduler
      "$<TARGET_FILE:cxxlens-unit-scheduler>" --provisioning
      "$<TARGET_FILE:cxxlens-unit-provisioning>"
    DEPENDS cxxlens-m1-test-binaries cxxlens-quality
    USES_TERMINAL VERBATIM)
endif()

if(TARGET cxxlens-m2-test-binaries)
  add_custom_target(
    cxxlens-m2-acceptance
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}"
            --output-on-failure
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/run_m2_acceptance.py" --root
      "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --manifest
      "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_m2_completion.yaml" --report
      "${CMAKE_BINARY_DIR}/m2-acceptance-report.json" --compiler
      "${CMAKE_CXX_COMPILER}" --integration
      "$<TARGET_FILE:cxxlens-m2-search-conformance>" --scheduler
      "$<TARGET_FILE:cxxlens-unit-scheduler>" --example-source
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/m2-flagship"
    DEPENDS cxxlens-m2-test-binaries cxxlens-quality
    USES_TERMINAL VERBATIM)
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
      "${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_doc_examples.py"
      --compiler "${CMAKE_CXX_COMPILER}" --include
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/cxxlens"
    DEPENDS docs
    VERBATIM)
  add_dependencies(cxxlens-quality cxxlens-doxygen-contract)
endif()
