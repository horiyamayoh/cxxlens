# Quality is a CTest concern.  Keep this file limited to registering ordinary
# CTest checks and to the optional Doxygen build; do not create a second graph
# of custom targets which executes the same checker scripts independently.
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  find_program(CXXLENS_CLANG_FORMAT NAMES clang-format-22 REQUIRED)
  set(_cxxlens_quality_check_root "${CMAKE_CURRENT_SOURCE_DIR}")

  add_test(
    NAME quality.format
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_cxxlens_quality_check_root}/tools/quality/check_format.py"
      --clang-format "${CXXLENS_CLANG_FORMAT}" --root
      "${_cxxlens_quality_check_root}")
  set_tests_properties(quality.format PROPERTIES LABELS "quality;style")

  add_test(
    NAME quality.text-lint
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_cxxlens_quality_check_root}/tools/quality/run_text_lints.py" --root
      "${_cxxlens_quality_check_root}")
  set_tests_properties(quality.text-lint PROPERTIES LABELS "quality;style")

  add_test(
    NAME quality.documentation-consistency
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_cxxlens_quality_check_root}/tools/quality/check_documentation_consistency.py"
      check --root "${_cxxlens_quality_check_root}")
  set_tests_properties(quality.documentation-consistency
                       PROPERTIES LABELS "quality;docs")

  add_test(
    NAME quality.public-boundary
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_cxxlens_quality_check_root}/tools/quality/check_public_headers.py"
      "${_cxxlens_quality_check_root}/include/cxxlens")
  set_tests_properties(quality.public-boundary PROPERTIES LABELS "quality;api")

  add_test(
    NAME quality.runtime-port
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_cxxlens_quality_check_root}/tools/quality/check_runtime_port_usage.py"
      "${_cxxlens_quality_check_root}/src")
  set_tests_properties(quality.runtime-port PROPERTIES LABELS "quality;api")

  # This target is only a convenience wrapper around CTest.  CI may use it or
  # invoke CTest directly, but the checker commands themselves have one owner:
  # CTest.  BUILD_TESTING=OFF therefore exposes no test-only quality target.
  add_custom_target(
    cxxlens-quality
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}"
            --label-regex "quality|security|docs" --output-on-failure
    DEPENDS cxxlens-ng-test-binaries
    USES_TERMINAL VERBATIM)
  unset(_cxxlens_quality_check_root)
endif()

find_program(CXXLENS_RUN_CLANG_TIDY NAMES run-clang-tidy-22 run-clang-tidy)
if(CXXLENS_RUN_CLANG_TIDY)
  add_custom_target(
    cxxlens-clang-tidy
    COMMAND
      "${CXXLENS_RUN_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" -config-file
      "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy" -header-filter
      "${CMAKE_CURRENT_SOURCE_DIR}/(include/cxxlens|src)/.*"
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

  if(BUILD_TESTING)
    add_test(NAME docs.generate COMMAND "${CMAKE_COMMAND}" --build
                                        "${CMAKE_BINARY_DIR}" --target docs)
    set_tests_properties(docs.generate PROPERTIES LABELS "docs" FIXTURES_SETUP
                                                  cxxlens_doxygen)
    add_test(
      NAME docs.doxygen-contract
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/quality/verify_doxygen.py"
        "${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml" --ng-catalog
        "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cxxlens_ng_public_api_catalog.yaml"
    )
    set_tests_properties(
      docs.doxygen-contract PROPERTIES LABELS "docs;quality" FIXTURES_REQUIRED
                                       cxxlens_doxygen)
  endif()
endif()
