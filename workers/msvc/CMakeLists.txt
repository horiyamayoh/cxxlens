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

# The official Windows LLVM archive exports an XML package dependency but does
# not ship it. The worker does not use the Windows manifest library that owns
# that dependency. Satisfy only CMake's global imported-target validation; an
# accidental XML code reference remains an unresolved linker error rather than a
# fallback implementation. LLVM Support does use zlib and zstd, so those
# packages must be supplied by the caller and are deliberately not stubbed.
add_library(LibXml2::LibXml2 INTERFACE IMPORTED)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
if(TARGET zstd::libzstd AND NOT TARGET zstd::libzstd_static)
  add_library(zstd::libzstd_static ALIAS zstd::libzstd)
endif()
find_package(LLVM 23.1.0 EXACT CONFIG REQUIRED)
find_package(Clang 23.1.0 EXACT CONFIG REQUIRED)
if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL "23.1.0")
  message(
    FATAL_ERROR "The MSVC replay worker requires exact LLVM 23.1.0 packages")
endif()

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

add_library(
  cxxlens_clangcl_worker_core STATIC
  "${CXXLENS_ROOT}/src/sdk/common.cpp"
  "${CXXLENS_ROOT}/src/sdk/bounded_json_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/application_analysis_limits.cpp"
  "${CXXLENS_ROOT}/src/sdk/bounded_canonical_binary_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/compiler_replay_input_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/detached_provider_run_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/detached_provider_run_builder_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider.cpp"
  "${CXXLENS_ROOT}/src/sdk/openssl_detached_run_crypto_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider_manifest_codec_internal.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider_ng1_transport.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider_ng1_validation.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider_protocol_v2_adapter.cpp"
  "${CXXLENS_ROOT}/src/sdk/provider_runtime.cpp"
  "${CXXLENS_ROOT}/src/sdk/relation.cpp"
  "${CXXLENS_ROOT}/src/sdk/source_identity_internal.cpp"
  "${CXXLENS_ROOT}/src/runtime/detached_run_signing_file_port_internal.cpp"
  "${CXXLENS_ROOT}/src/protocol_v2/cbor.cpp"
  "${CXXLENS_ROOT}/src/protocol_v2/codec.cpp"
  "${CXXLENS_ROOT}/src/protocol_v2/closure.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/observation_normalizer.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/provider_worker.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/source_authority_binder.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/worker_ingress.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/worker_observation_codec.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/worker_observer.cpp"
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/worker_parser.cpp")
target_link_libraries(
  cxxlens_clangcl_worker_core PRIVATE clangAST clangFrontend clangIndex
                                      clangTooling OpenSSL::Crypto)
target_include_directories(
  cxxlens_clangcl_worker_core PUBLIC "${CXXLENS_ROOT}/include"
                                     "${CXXLENS_ROOT}/src")
target_include_directories(cxxlens_clangcl_worker_core SYSTEM
                           PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(
  cxxlens_clangcl_worker_core
  PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
          CXXLENS_PROVIDER_LOGICAL_TRANSCRIPT_ONLY)
target_compile_features(cxxlens_clangcl_worker_core PUBLIC cxx_std_23)
target_compile_options(cxxlens_clangcl_worker_core PRIVATE /W4 /WX /permissive-
                                                           /Gy)

add_executable(
  cxxlens-clangcl-worker-23
  "${CXXLENS_ROOT}/src/llvm/clang23_gcc_replay/clangcl_worker_main.cpp")
target_link_libraries(cxxlens-clangcl-worker-23
                      PRIVATE cxxlens_clangcl_worker_core)
target_include_directories(cxxlens-clangcl-worker-23
                           PRIVATE "${CXXLENS_ROOT}/src")
target_compile_features(cxxlens-clangcl-worker-23 PRIVATE cxx_std_23)
target_compile_options(cxxlens-clangcl-worker-23 PRIVATE /W4 /WX /permissive-)

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

  add_executable(
    cxxlens-clangcl-worker-test
    "${CXXLENS_ROOT}/tests/unit/adapter/clang23_gcc_replay/clangcl_worker_test.cpp"
  )
  target_link_libraries(cxxlens-clangcl-worker-test
                        PRIVATE cxxlens_clangcl_worker_core)
  target_include_directories(cxxlens-clangcl-worker-test
                             PRIVATE "${CXXLENS_ROOT}/src")
  target_compile_features(cxxlens-clangcl-worker-test PRIVATE cxx_std_23)
  target_compile_options(cxxlens-clangcl-worker-test PRIVATE /W4 /WX
                                                             /permissive-)
  add_test(NAME application-analysis.clangcl-worker
           COMMAND cxxlens-clangcl-worker-test)
  add_executable(
    cxxlens-provider-worker-ingress-test
    "${CXXLENS_ROOT}/tests/unit/adapter/clang23_gcc_replay/worker_ingress_test.cpp"
  )
  target_link_libraries(cxxlens-provider-worker-ingress-test
                        PRIVATE cxxlens_clangcl_worker_core)
  target_include_directories(cxxlens-provider-worker-ingress-test
                             PRIVATE "${CXXLENS_ROOT}/src")
  target_compile_features(cxxlens-provider-worker-ingress-test
                          PRIVATE cxx_std_23)
  target_compile_options(cxxlens-provider-worker-ingress-test
                         PRIVATE /W4 /WX /permissive-)
  add_test(NAME application-analysis.provider-worker-detached-run
           COMMAND cxxlens-provider-worker-ingress-test)
  add_executable(
    cxxlens-openssl-detached-run-crypto-test
    "${CXXLENS_ROOT}/tests/unit/sdk/openssl_detached_run_crypto_test.cpp")
  target_link_libraries(cxxlens-openssl-detached-run-crypto-test
                        PRIVATE cxxlens_clangcl_worker_core)
  target_include_directories(cxxlens-openssl-detached-run-crypto-test
                             PRIVATE "${CXXLENS_ROOT}/src")
  target_compile_features(cxxlens-openssl-detached-run-crypto-test
                          PRIVATE cxx_std_23)
  target_compile_options(cxxlens-openssl-detached-run-crypto-test
                         PRIVATE /W4 /WX /permissive-)
  add_test(NAME application-analysis.openssl-detached-run-crypto
           COMMAND cxxlens-openssl-detached-run-crypto-test)
  add_executable(
    cxxlens-detached-run-signing-file-port-test
    "${CXXLENS_ROOT}/tests/unit/sdk/detached_run_signing_file_port_test.cpp")
  target_link_libraries(cxxlens-detached-run-signing-file-port-test
                        PRIVATE cxxlens_clangcl_worker_core)
  target_include_directories(cxxlens-detached-run-signing-file-port-test
                             PRIVATE "${CXXLENS_ROOT}/src")
  target_compile_features(cxxlens-detached-run-signing-file-port-test
                          PRIVATE cxx_std_23)
  target_compile_options(cxxlens-detached-run-signing-file-port-test
                         PRIVATE /W4 /WX /permissive-)
  add_test(NAME application-analysis.detached-run-signing-file-port
           COMMAND cxxlens-detached-run-signing-file-port-test)
  add_test(NAME application-analysis.clangcl-worker-version
           COMMAND cxxlens-clangcl-worker-23 --version)
endif()

install(TARGETS cxxlens-msvc-capture cxxlens-clangcl-worker-23
        RUNTIME DESTINATION bin COMPONENT application-analysis-msvc-worker)
install(
  FILES "${CXXLENS_ROOT}/LICENSE" "${CXXLENS_ROOT}/NOTICE"
  DESTINATION share/doc/cxxlens
  COMPONENT application-analysis-msvc-worker)
