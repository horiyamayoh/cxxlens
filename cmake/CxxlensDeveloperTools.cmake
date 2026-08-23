find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(CXXLENS_PUBLIC_CALLABLE_CLANG NAMES clang++-22 REQUIRED)
find_program(CXXLENS_CLANG_FORMAT NAMES clang-format-22 REQUIRED)

# Developer quality is deliberately test-only. These targets run contract
# assertions and fail with a non-zero status; they do not write reports,
# receipts, provenance bundles, or release qualification documents.
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

# These checks exercise product contracts. The old release/readiness and
# ownership/report checkers intentionally do not appear here.
foreach(
  contract IN
  ITEMS release_contract
        clang22_materialization
        relation_contract
        query_contract
        semantic_guarantee
        snapshot_store_contract
        sqlite_store_contract
        provider_ng1
        provider_protocol
        provider_runtime
        security_contract
        source_closure_transport)
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
  cxxlens-ng-public-callable-inventory-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/public_callable_inventory.py"
    check --root "${CMAKE_CURRENT_SOURCE_DIR}" --compiler
    "${CXXLENS_PUBLIC_CALLABLE_CLANG}"
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
  cxxlens-sanitizer-coverage-check
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/check_sanitizer_coverage.py"
    contract --root "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

add_custom_target(cxxlens-quality)
add_dependencies(
  cxxlens-quality
  cxxlens-documentation-consistency-check
  cxxlens-ng-release-contract-check
  cxxlens-ng-clang22-materialization-check
  cxxlens-ng-relation-contract-check
  cxxlens-ng-query-contract-check
  cxxlens-ng-semantic-guarantee-check
  cxxlens-ng-snapshot-store-contract-check
  cxxlens-ng-sqlite-store-contract-check
  cxxlens-ng-provider-ng1-check
  cxxlens-ng-provider-protocol-check
  cxxlens-ng-provider-runtime-check
  cxxlens-ng-security-contract-check
  cxxlens-ng-sdk-contract-check
  cxxlens-ng-ci-supply-chain-check
  cxxlens-ng-public-callable-inventory-check
  cxxlens-public-boundary-check
  cxxlens-runtime-port-check
  cxxlens-sanitizer-coverage-check
  cxxlens-text-lint
  cxxlens-format-check)

find_program(CXXLENS_RUN_CLANG_TIDY NAMES run-clang-tidy-22 run-clang-tidy)
if(CXXLENS_RUN_CLANG_TIDY)
  # These two implementations are byte-exact source-closure bindings. Their
  # immutable bytes are checked by check_ng_source_closure_transport.py;
  # exclude only those files from the current clang-tidy policy while
  # retaining their compile and runtime contract tests.
  set(_cxxlens_tidy_source_filter
      "${CMAKE_CURRENT_SOURCE_DIR}/src/(?!llvm/clang22/materialization_request_v2_1\\.cpp$|llvm/clang22/provider_task_v3\\.cpp$).*")
  add_custom_target(
    cxxlens-clang-tidy
    COMMAND
      "${CXXLENS_RUN_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" -config-file
      "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy" -header-filter
      "${CMAKE_CURRENT_SOURCE_DIR}/(include/cxxlens|src)/.*"
      "${_cxxlens_tidy_source_filter}"
      DEPENDS ${CXXLENS_COMPILED_TARGETS}
      VERBATIM)
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
