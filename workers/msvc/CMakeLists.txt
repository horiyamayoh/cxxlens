cmake_minimum_required(VERSION 3.25)

project(
  cxxlens_msvc_application_worker
  VERSION 1.0.0
  LANGUAGES CXX)

if(NOT WIN32
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.0"
   OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  message(
    FATAL_ERROR
      "The MSVC application workers require exact clang-cl 23.1.0 on Windows")
endif()

set(CXXLENS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_library(
  cxxlens_msvc_worker_base STATIC
  "${CXXLENS_ROOT}/src/sdk/common.cpp"
  "${CXXLENS_ROOT}/src/sdk/bounded_json_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/source_identity_internal.cpp"
  "${CXXLENS_ROOT}/src/runtime/msvc_capture_file_port.cpp"
  "${CXXLENS_ROOT}/src/runtime/msvc_process_port.cpp"
  "${CXXLENS_ROOT}/src/msvc_worker/msvc_capture_bundle.cpp"
  "${CXXLENS_ROOT}/src/msvc_worker/msvc_response_arguments.cpp"
  "${CXXLENS_ROOT}/src/msvc_worker/msvc_capture_session.cpp"
  "${CXXLENS_ROOT}/src/msvc_worker/msvc_source_dependencies.cpp")
target_compile_features(cxxlens_msvc_worker_base PUBLIC cxx_std_23)
target_include_directories(
  cxxlens_msvc_worker_base
  PUBLIC "${CXXLENS_ROOT}/include"
  PRIVATE "${CXXLENS_ROOT}/src")
target_compile_options(cxxlens_msvc_worker_base PRIVATE /W4 /WX /permissive-)

add_executable(cxxlens-msvc-capture
               "${CXXLENS_ROOT}/src/msvc_worker/capture_main.cpp")
target_link_libraries(cxxlens-msvc-capture PRIVATE cxxlens_msvc_worker_base)
target_include_directories(cxxlens-msvc-capture PRIVATE "${CXXLENS_ROOT}/src")
target_compile_features(cxxlens-msvc-capture PRIVATE cxx_std_23)
target_compile_options(cxxlens-msvc-capture PRIVATE /W4 /WX /permissive-)

include(CTest)
if(BUILD_TESTING)
  add_test(
    NAME application-analysis.msvc-canonical-vector
    COMMAND
      cxxlens-msvc-capture --check-vector
      "${CXXLENS_ROOT}/tests/fixtures/msvc_application_analysis/canonical_capture.sha256"
  )
  add_test(
    NAME application-analysis.msvc-proxy-self-recursion
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "CXXLENS_MSVC_REAL_CL=$<TARGET_FILE:cxxlens-msvc-capture>"
      "$<TARGET_FILE:cxxlens-msvc-capture>" /nologo)
  set_tests_properties(application-analysis.msvc-proxy-self-recursion
                       PROPERTIES WILL_FAIL TRUE)
endif()

install(TARGETS cxxlens-msvc-capture
        RUNTIME DESTINATION bin COMPONENT application-analysis-msvc-worker)
install(
  FILES "${CXXLENS_ROOT}/LICENSE" "${CXXLENS_ROOT}/NOTICE"
  DESTINATION share/doc/cxxlens
  COMPONENT application-analysis-msvc-worker)
