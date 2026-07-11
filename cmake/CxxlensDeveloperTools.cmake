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
  cxxlens-quality cxxlens-api-contract-check cxxlens-design-package-check
  cxxlens-public-boundary-check cxxlens-text-lint)
if(TARGET cxxlens-format-check)
  add_dependencies(cxxlens-quality cxxlens-format-check)
endif()
if(TARGET cxxlens-clang-tidy)
  add_dependencies(cxxlens-quality cxxlens-clang-tidy)
endif()

if(CXXLENS_BUILD_DOCS)
  find_package(Doxygen 1.9.8 REQUIRED)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

  add_custom_target(
    docs
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
