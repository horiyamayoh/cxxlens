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
endif()

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

foreach(
  contract IN
  ITEMS release_contract
        relation_contract
        query_contract
        semantic_guarantee
        snapshot_store_contract
        provider_protocol
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
  cxxlens-ng-provider-protocol-check
  cxxlens-ng-provider-runtime-check
  cxxlens-ng-api-development-readiness-check
  cxxlens-ng-ci-supply-chain-check
  cxxlens-ng-migration-completion-check
  cxxlens-ng-query-contract-check
  cxxlens-ng-relation-contract-check
  cxxlens-ng-release-contract-check
  cxxlens-ng-sdk-contract-check
  cxxlens-ng-security-contract-check
  cxxlens-ng-semantic-guarantee-check
  cxxlens-ng-snapshot-store-contract-check
  cxxlens-public-boundary-check
  cxxlens-quality-ownership-check
  cxxlens-runtime-port-check
  cxxlens-sanitizer-coverage-check
  cxxlens-text-lint)
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
    DEPENDS docs
    VERBATIM)
  add_dependencies(cxxlens-quality cxxlens-doxygen-contract)
endif()
