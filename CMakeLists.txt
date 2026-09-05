cmake_minimum_required(VERSION 3.25)

project(
  cxxlens
  VERSION 1.0.0
  DESCRIPTION "C and C++ Semantic Relation Platform"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds are not supported. Use a preset or a separate build directory."
  )
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CXXLENS_WARNINGS_AS_ERRORS "Treat cxxlens compiler warnings as errors"
       OFF)
option(CXXLENS_BUILD_SHARED "Build cxxlens components as shared libraries" OFF)
option(CXXLENS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling cxxlens" OFF)
option(CXXLENS_ENABLE_IWYU "Run include-what-you-use while compiling cxxlens"
       OFF)
option(CXXLENS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CXXLENS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CXXLENS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(CXXLENS_BUILD_DOCS "Enable Doxygen documentation targets" OFF)
option(CXXLENS_BUILD_QUALITY_TOOLS "Enable local quality targets" ON)
option(CXXLENS_BUILD_CLANG22_COMPONENTS
       "Build Clang 22 native provider, materializer, and worker components" ON)
option(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER
       "Build the exact Clang 23.1.0 GCC-mode replay worker" OFF)

if(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER)
  if(CXXLENS_BUILD_CLANG22_COMPONENTS)
    message(
      FATAL_ERROR
        "The Clang 23 GCC replay worker must be configured separately from Clang 22 components"
    )
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
     OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.0")
    message(
      FATAL_ERROR
        "The GCC replay worker requires the exact Clang 23.1.0 compiler")
  endif()
  find_package(LLVM 23.1.0 EXACT CONFIG REQUIRED)
  find_package(Clang 23.1.0 EXACT CONFIG REQUIRED)
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
  if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL "23.1.0")
    message(
      FATAL_ERROR "The GCC replay worker requires exact LLVM 23.1.0 packages")
  endif()
  message(STATUS "Enabled exact Clang 23.1.0 GCC replay worker")
endif()

set(CXXLENS_SOURCE_REVISION
    ""
    CACHE STRING
          "Exact 40-hex source commit embedded in installed occurrence identity"
)
set(CXXLENS_SOURCE_TREE
    ""
    CACHE STRING
          "Exact 40-hex source tree embedded in installed occurrence identity")
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  if((CXXLENS_SOURCE_REVISION STREQUAL "" AND NOT CXXLENS_SOURCE_TREE STREQUAL
                                              "")
     OR (NOT CXXLENS_SOURCE_REVISION STREQUAL "" AND CXXLENS_SOURCE_TREE
                                                     STREQUAL ""))
    message(
      FATAL_ERROR
        "CXXLENS_SOURCE_REVISION and CXXLENS_SOURCE_TREE must be supplied together"
    )
  endif()
  if(CXXLENS_SOURCE_REVISION STREQUAL "" AND CXXLENS_SOURCE_TREE STREQUAL "")
    find_package(Git QUIET)
    if(NOT Git_FOUND OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
      message(
        FATAL_ERROR
          "Source provenance requires either an available git checkout or an explicit revision/tree pair"
      )
    endif()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _cxxlens_git_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _cxxlens_git_revision_status)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse "HEAD^{tree}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _cxxlens_git_tree
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _cxxlens_git_tree_status)
    if(NOT _cxxlens_git_revision_status EQUAL 0 OR NOT _cxxlens_git_tree_status
                                                   EQUAL 0)
      message(FATAL_ERROR "Cannot derive exact source provenance from git")
    endif()
    set(CXXLENS_SOURCE_REVISION "${_cxxlens_git_revision}")
    set(CXXLENS_SOURCE_TREE "${_cxxlens_git_tree}")
  endif()
  string(LENGTH "${CXXLENS_SOURCE_REVISION}" _cxxlens_source_revision_length)
  string(LENGTH "${CXXLENS_SOURCE_TREE}" _cxxlens_source_tree_length)
  if(NOT CXXLENS_SOURCE_REVISION MATCHES "^[0-9a-f]+$"
     OR NOT CXXLENS_SOURCE_TREE MATCHES "^[0-9a-f]+$"
     OR NOT _cxxlens_source_revision_length EQUAL 40
     OR NOT _cxxlens_source_tree_length EQUAL 40)
    message(
      FATAL_ERROR
        "CXXLENS_SOURCE_REVISION and CXXLENS_SOURCE_TREE must be exact 40-hex identities"
    )
  endif()
  if(CXXLENS_BUILD_SHARED)
    set(CXXLENS_OCCURRENCE_CONFIGURATION shared)
  else()
    set(CXXLENS_OCCURRENCE_CONFIGURATION static)
  endif()
  configure_file(
    cmake/GenerateClang22OccurrenceManifest.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/GenerateClang22OccurrenceManifest.cmake" @ONLY)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CxxlensWarnings)
include(CxxlensSanitizers)
include(CxxlensClangTargets)
cxxlens_validate_sanitizer_configuration()

if(NOT CXXLENS_BUILD_CLANG22_COMPONENTS)
  if(CXXLENS_CLANG_ADAPTER STREQUAL "ON")
    message(
      FATAL_ERROR
        "CXXLENS_CLANG_ADAPTER=ON requires CXXLENS_BUILD_CLANG22_COMPONENTS=ON")
  endif()
  set(CXXLENS_CLANG22_AVAILABLE
      FALSE
      CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
  set(CXXLENS_CLANG22_UNAVAILABLE_REASON
      "disabled-by-build-profile"
      CACHE INTERNAL "Reason the exact Clang 22 adapter is unavailable" FORCE)
  set(CXXLENS_CLANG22_LIBRARY_DIRS
      ""
      CACHE INTERNAL "Exact LLVM/Clang 22 runtime library directories" FORCE)
  message(
    STATUS
      "Clang 22 native components disabled by CXXLENS_BUILD_CLANG22_COMPONENTS=OFF"
  )
endif()

if(CXXLENS_BUILD_SHARED)
  set(CXXLENS_LIBRARY_TYPE SHARED)
else()
  set(CXXLENS_LIBRARY_TYPE STATIC)
endif()

function(cxxlens_configure_component target export_name)
  target_compile_features(${target} PUBLIC cxx_std_23)
  target_include_directories(
    ${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                     "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  set_target_properties(
    ${target}
    PROPERTIES CXX_EXTENSIONS OFF
               EXPORT_NAME "${export_name}"
               VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}")
  if(CXXLENS_BUILD_SHARED)
    if(APPLE)
      set_target_properties(${target} PROPERTIES INSTALL_RPATH "@loader_path")
    elseif(UNIX)
      set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    set_target_properties(${target} PROPERTIES INSTALL_RPATH_USE_LINK_PATH
                                               FALSE)
  endif()
  cxxlens_set_project_warnings(${target})
  cxxlens_enable_sanitizers(${target})
endfunction()

function(cxxlens_configure_installed_tool target)
  if(CXXLENS_BUILD_SHARED)
    if(APPLE)
      set(_cxxlens_tool_rpath "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
      set(_cxxlens_tool_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(DEFINED _cxxlens_tool_rpath)
      set_target_properties(
        ${target} PROPERTIES INSTALL_RPATH "${_cxxlens_tool_rpath}"
                             INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
  endif()
endfunction()

function(cxxlens_configure_installed_clang22_tool target)
  cxxlens_configure_installed_tool(${target})
  # The static distribution links the provider worker against the exact external
  # Clang/LLVM shared runtime. Its installed execution contract unsets ambient
  # loader variables, so retain the configured exact runtime directory in the
  # worker only; no adjacent-major or first-found fallback is permitted.
  if(NOT CXXLENS_BUILD_SHARED
     AND CXXLENS_CLANG22_AVAILABLE
     AND CXXLENS_CLANG22_LIBRARY_DIRS)
    set_target_properties(
      ${target} PROPERTIES INSTALL_RPATH "${CXXLENS_CLANG22_LIBRARY_DIRS}"
                           INSTALL_RPATH_USE_LINK_PATH FALSE)
  endif()
endfunction()

add_library(cxxlens_base ${CXXLENS_LIBRARY_TYPE} src/sdk/common.cpp
                         src/sdk/bounded_json_internal.cpp)
add_library(cxxlens::base ALIAS cxxlens_base)
cxxlens_configure_component(cxxlens_base base)

add_library(
  cxxlens_kernel
  ${CXXLENS_LIBRARY_TYPE}
  src/sdk/relation.cpp
  src/sdk/claim.cpp
  src/sdk/store.cpp
  src/sdk/store_candidate_projection_internal.cpp
  src/sdk/snapshot_store_v5_codec_internal.cpp
  src/sdk/store_operation_port_internal.cpp
  src/sdk/sqlite_connection_lifecycle_internal.cpp
  src/runtime/monotonic_clock_port.cpp
  src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp
  src/sdk/sqlite_same_process_shm_process_port_internal.cpp
  src/sdk/sqlite_same_process_shm_vfs_alias_registration_internal.cpp
  src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp
  src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp
  src/sdk/sqlite_writer_gate_outcome_internal.cpp
  src/sdk/sqlite_writer_shm_mapping_epoch_internal.cpp
  src/sdk/sqlite_writer_shm_mapping_semantics_internal.cpp
  src/sdk/sqlite_limit_length_control_internal.cpp
  src/sdk/sqlite_payload_streaming_internal.cpp
  src/sdk/sqlite_wal_receipt_internal.cpp
  src/sdk/sqlite_wal_source_capture_internal.cpp
  src/sdk/sqlite_wal_recovery_workspace_internal.cpp
  src/sdk/sqlite_private_snapshot_internal.cpp
  src/sdk/sqlite_default_observation.cpp
  src/sdk/sqlite_source_shm_readonly_preflight_internal.cpp
  src/sdk/sqlite_default_forwarding_vfs.cpp
  src/sdk/sqlite_store_terminal_internal.cpp
  src/sdk/sqlite_terminal_reclassifier_internal.cpp
  src/sdk/sqlite_exact_empty_cold_reclassifier_internal.cpp)
add_library(cxxlens::kernel ALIAS cxxlens_kernel)
cxxlens_configure_component(cxxlens_kernel kernel)
target_link_libraries(
  cxxlens_kernel
  PUBLIC cxxlens::base
  PRIVATE "${CMAKE_DL_LIBS}")
# Store fault/mutation fixtures are built by the test targets that exercise
# them.  The production kernel is always compiled without a testing macro or
# test-only source, including when BUILD_TESTING=ON, so the installed product
# has no testing seam.

# The bounded Store v6 phase core and Memory backend are source-private product
# components. Keep their implementation in one non-installed archive so direct
# contract tests and the later production Store binding share exactly one copy.
add_library(
  cxxlens_bounded_store_v6_memory_internal STATIC EXCLUDE_FROM_ALL
  src/sdk/bounded_store_v6_internal.cpp
  src/sdk/bounded_store_v6_memory_internal.cpp)
target_link_libraries(cxxlens_bounded_store_v6_memory_internal
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens_bounded_store_v6_memory_internal
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens_bounded_store_v6_memory_internal
                        PRIVATE cxx_std_23)
set_target_properties(
  cxxlens_bounded_store_v6_memory_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
cxxlens_set_project_warnings(cxxlens_bounded_store_v6_memory_internal)
cxxlens_enable_sanitizers(cxxlens_bounded_store_v6_memory_internal)

# The SQLite physical Store v6 port is a separate source-private archive. It
# reuses the phase core from the bounded Store target, while keeping the
# physical backend out of the memory implementation and installed product.
add_library(cxxlens_bounded_store_v6_sqlite_internal STATIC EXCLUDE_FROM_ALL
            src/sdk/bounded_store_v6_sqlite_internal.cpp)
target_link_libraries(
  cxxlens_bounded_store_v6_sqlite_internal
  PUBLIC cxxlens::base
  PRIVATE cxxlens::kernel cxxlens_bounded_store_v6_memory_internal
          "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens_bounded_store_v6_sqlite_internal
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens_bounded_store_v6_sqlite_internal
                        PRIVATE cxx_std_23)
set_target_properties(
  cxxlens_bounded_store_v6_sqlite_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
cxxlens_set_project_warnings(cxxlens_bounded_store_v6_sqlite_internal)
cxxlens_enable_sanitizers(cxxlens_bounded_store_v6_sqlite_internal)

add_library(
  cxxlens_query ${CXXLENS_LIBRARY_TYPE} src/sdk/query.cpp
                src/sdk/query_execution.cpp src/sdk/query_ir_decoder.cpp)
add_library(cxxlens::query ALIAS cxxlens_query)
cxxlens_configure_component(cxxlens_query query)
target_link_libraries(cxxlens_query PUBLIC cxxlens::kernel)

add_library(cxxlens_cpp INTERFACE)
add_library(cxxlens::cpp ALIAS cxxlens_cpp)
target_compile_features(cxxlens_cpp INTERFACE cxx_std_23)
target_include_directories(
  cxxlens_cpp INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(cxxlens_cpp INTERFACE cxxlens::query)
set_target_properties(cxxlens_cpp PROPERTIES EXPORT_NAME cpp)

add_library(cxxlens_recipes ${CXXLENS_LIBRARY_TYPE} src/sdk/recipe.cpp)
add_library(cxxlens::recipes ALIAS cxxlens_recipes)
cxxlens_configure_component(cxxlens_recipes recipes)
target_link_libraries(cxxlens_recipes PUBLIC cxxlens::query)

add_library(cxxlens INTERFACE)
add_library(cxxlens::cxxlens ALIAS cxxlens)
target_compile_features(cxxlens INTERFACE cxx_std_23)
target_include_directories(
  cxxlens INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(cxxlens INTERFACE cxxlens::base cxxlens::kernel
                                        cxxlens::query cxxlens::cpp)
set_target_properties(cxxlens PROPERTIES EXPORT_NAME cxxlens)

# Provider runtime ownership is split by change reason. The platform-neutral
# runtime core owns logical validation and result adoption, NG1 owns its
# lifecycle state machines, and the process target owns Linux/process/filesystem
# effects. These targets are assembled only through the private closure below;
# none is installed or exported.
add_library(cxxlens_provider_runtime_core_internal OBJECT
            EXCLUDE_FROM_ALL src/sdk/provider_runtime.cpp)
add_library(
  cxxlens_provider_ng1_runtime_internal OBJECT
  EXCLUDE_FROM_ALL src/sdk/provider_ng1_transport.cpp
  src/sdk/provider_ng1_validation.cpp src/sdk/provider_ng1_recovery.cpp
  src/sdk/provider_ng1_session.cpp src/sdk/provider_ng1_live_driver.cpp)
add_library(
  cxxlens_provider_process_internal OBJECT
  EXCLUDE_FROM_ALL src/runtime/provider_process_adapter.cpp
  src/sdk/provider_ng1_spill_port_internal.cpp)
add_library(
  cxxlens_process_authority_internal OBJECT
  EXCLUDE_FROM_ALL src/runtime/sealed_executable_internal.cpp
  src/runtime/gcc_probe_process_port_internal.cpp)
foreach(
  target IN
  ITEMS cxxlens_provider_runtime_core_internal
        cxxlens_provider_ng1_runtime_internal cxxlens_provider_process_internal)
  target_link_libraries(${target} PRIVATE cxxlens::cxxlens cxxlens::recipes)
  target_include_directories(${target}
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(${target} PRIVATE cxx_std_23)
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  cxxlens_set_project_warnings(${target})
  cxxlens_enable_sanitizers(${target})
endforeach()
target_link_libraries(cxxlens_process_authority_internal PRIVATE cxxlens::base)
target_include_directories(cxxlens_process_authority_internal
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens_process_authority_internal PRIVATE cxx_std_23)
set_target_properties(
  cxxlens_process_authority_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
cxxlens_set_project_warnings(cxxlens_process_authority_internal)
cxxlens_enable_sanitizers(cxxlens_process_authority_internal)

add_library(
  cxxlens_sdk
  ${CXXLENS_LIBRARY_TYPE}
  src/sdk/provider.cpp
  src/sdk/incremental.cpp
  src/sdk/application_analysis.cpp
  src/sdk/application_analysis_limits.cpp
  src/sdk/bounded_canonical_binary_internal.cpp
  src/sdk/application_analysis_service.cpp
  src/sdk/application_materialization_execution_internal.cpp
  src/sdk/application_materialization_adoption_internal.cpp
  src/sdk/build_capture_internal.cpp
  src/sdk/compile_commands_capture_internal.cpp
  src/sdk/gcc_auxiliary_capture_internal.cpp
  src/sdk/application_build_capture_adapter_internal.cpp
  src/sdk/gcc_capture_command_service_internal.cpp
  src/sdk/gcc_capture_file_service_internal.cpp
  src/sdk/gcc_capture_bundle_internal.cpp
  src/sdk/compiler_replay_input_internal.cpp
  src/sdk/provider_manifest_codec_internal.cpp
  src/sdk/detached_provider_run_internal.cpp
  src/sdk/detached_provider_run_builder_internal.cpp
  src/sdk/detached_provider_run_process_builder_internal.cpp
  src/sdk/detached_provider_run_adoption_internal.cpp
  src/sdk/gcc_replay_planner_internal.cpp
  src/sdk/msvc_replay_planner_internal.cpp
  src/sdk/gcc_toolchain_probe_internal.cpp
  src/msvc_worker/msvc_capture_bundle.cpp
  src/msvc_worker/msvc_response_arguments.cpp
  src/msvc_worker/msvc_source_dependencies.cpp
  src/sdk/source_identity_internal.cpp
  src/runtime/gcc_capture_file_port.cpp
  src/sdk/materialization_task_internal.cpp
  src/sdk/materialization_io_internal.cpp
  src/sdk/materialization_store_candidate_internal.cpp
  src/sdk/materialization_store_candidate_bridge_internal.cpp
  src/sdk/materialization_writer_internal.cpp)
add_library(cxxlens::sdk ALIAS cxxlens_sdk)
cxxlens_configure_component(cxxlens_sdk sdk)
target_sources(cxxlens_sdk
               PRIVATE $<TARGET_OBJECTS:cxxlens_process_authority_internal>)
target_link_libraries(cxxlens_sdk PUBLIC cxxlens::cxxlens cxxlens::recipes)

if(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER)
  add_library(cxxlens_detached_run_crypto_internal STATIC
              src/sdk/openssl_detached_run_crypto_internal.cpp)
  target_link_libraries(
    cxxlens_detached_run_crypto_internal
    PUBLIC cxxlens::sdk
    PRIVATE OpenSSL::Crypto)
  target_include_directories(cxxlens_detached_run_crypto_internal
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_detached_run_crypto_internal
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_detached_run_crypto_internal)
  cxxlens_enable_sanitizers(cxxlens_detached_run_crypto_internal)

  add_library(cxxlens_detached_run_signing_file_port_internal STATIC
              src/runtime/detached_run_signing_file_port_internal.cpp)
  target_link_libraries(cxxlens_detached_run_signing_file_port_internal
                        PUBLIC cxxlens::sdk)
  target_include_directories(cxxlens_detached_run_signing_file_port_internal
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_detached_run_signing_file_port_internal
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_detached_run_signing_file_port_internal)
  cxxlens_enable_sanitizers(cxxlens_detached_run_signing_file_port_internal)

  add_library(cxxlens_detached_run_trust_file_port_internal STATIC
              src/runtime/detached_run_trust_file_port_internal.cpp)
  target_link_libraries(cxxlens_detached_run_trust_file_port_internal
                        PUBLIC cxxlens::sdk)
  target_include_directories(cxxlens_detached_run_trust_file_port_internal
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_detached_run_trust_file_port_internal
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_detached_run_trust_file_port_internal)
  cxxlens_enable_sanitizers(cxxlens_detached_run_trust_file_port_internal)

  add_library(
    cxxlens_clang23_gcc_replay_ingress STATIC
    src/llvm/clang23_gcc_replay/observation_normalizer.cpp
    src/llvm/clang23_gcc_replay/provider_worker.cpp
    src/llvm/clang23_gcc_replay/source_authority_binder.cpp
    src/llvm/clang23_gcc_replay/worker_ingress.cpp
    src/llvm/clang23_gcc_replay/worker_observation_codec.cpp
    src/llvm/clang23_gcc_replay/worker_observer.cpp
    src/llvm/clang23_gcc_replay/worker_parser.cpp)
  target_link_libraries(
    cxxlens_clang23_gcc_replay_ingress
    PUBLIC cxxlens::sdk
    PRIVATE clang-cpp cxxlens_protocol_v2 cxxlens_detached_run_crypto_internal)
  target_include_directories(cxxlens_clang23_gcc_replay_ingress
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_include_directories(cxxlens_clang23_gcc_replay_ingress SYSTEM
                             PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(
    cxxlens_clang23_gcc_replay_ingress
    PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(cxxlens_clang23_gcc_replay_ingress PRIVATE -fno-rtti)
  endif()
  target_compile_features(cxxlens_clang23_gcc_replay_ingress PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang23_gcc_replay_ingress)
  cxxlens_enable_sanitizers(cxxlens_clang23_gcc_replay_ingress)

  add_library(cxxlens_clangcl_worker_command_internal STATIC
              src/llvm/clang23_gcc_replay/clangcl_worker_command_internal.cpp)
  target_link_libraries(
    cxxlens_clangcl_worker_command_internal
    PUBLIC cxxlens_clang23_gcc_replay_ingress
    PRIVATE cxxlens_detached_run_signing_file_port_internal
            cxxlens_detached_run_crypto_internal)
  target_include_directories(cxxlens_clangcl_worker_command_internal
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clangcl_worker_command_internal
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clangcl_worker_command_internal)
  cxxlens_enable_sanitizers(cxxlens_clangcl_worker_command_internal)

  add_executable(cxxlens-clang-gcc-replay-worker-23
                 src/llvm/clang23_gcc_replay/worker_main.cpp)
  target_link_libraries(cxxlens-clang-gcc-replay-worker-23
                        PRIVATE cxxlens_clang23_gcc_replay_ingress)
  target_compile_features(cxxlens-clang-gcc-replay-worker-23 PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang-gcc-replay-worker-23)
  cxxlens_enable_sanitizers(cxxlens-clang-gcc-replay-worker-23)
  cxxlens_configure_installed_tool(cxxlens-clang-gcc-replay-worker-23)
  if(UNIX AND NOT APPLE)
    set_target_properties(
      cxxlens-clang-gcc-replay-worker-23
      PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
                 INSTALL_RPATH_USE_LINK_PATH FALSE)
  endif()
endif()

# Compiler-neutral materialization I/O, candidate validation, and publication
# are one source-private SDK lifecycle.  Keeping the implementation in the SDK
# prevents frontend adapters and focused tests from assembling a second writer
# authority.
target_link_libraries(
  cxxlens_sdk
  PRIVATE $<BUILD_INTERFACE:cxxlens_bounded_store_v6_memory_internal>
          $<BUILD_INTERFACE:cxxlens_bounded_store_v6_sqlite_internal>)
target_include_directories(cxxlens_sdk
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Protocol 2 is a product transport component.  Its wire codec and closure state
# machine are independent of repository metadata and are available to
# provider/materializer integration tests without importing Clang headers.
add_library(
  cxxlens_protocol_v2 STATIC src/protocol_v2/cbor.cpp src/protocol_v2/codec.cpp
                             src/protocol_v2/closure.cpp)
target_link_libraries(cxxlens_protocol_v2 PUBLIC cxxlens::base)
target_include_directories(
  cxxlens_protocol_v2
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(cxxlens_protocol_v2 PRIVATE cxx_std_23)
set_target_properties(
  cxxlens_protocol_v2
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
cxxlens_set_project_warnings(cxxlens_protocol_v2)
cxxlens_enable_sanitizers(cxxlens_protocol_v2)

# Protocol adapters are implementation details of the SDK. Keep one object
# closure for the SDK and source-private consumers that must embed the adapter
# symbols; standalone worker closures retain their own static closure.
add_library(
  cxxlens_provider_protocol_v2_internal OBJECT
  src/sdk/provider_protocol_v2_adapter.cpp
  src/sdk/provider_protocol_v2_heartbeat_adapter.cpp)
target_link_libraries(cxxlens_provider_protocol_v2_internal
                      PRIVATE cxxlens_protocol_v2)
target_include_directories(cxxlens_provider_protocol_v2_internal
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens_provider_protocol_v2_internal
                        PRIVATE cxx_std_23)
set_target_properties(
  cxxlens_provider_protocol_v2_internal
  PROPERTIES POSITION_INDEPENDENT_CODE ON
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES)
cxxlens_set_project_warnings(cxxlens_provider_protocol_v2_internal)
cxxlens_enable_sanitizers(cxxlens_provider_protocol_v2_internal)

# The only runtime implementation closure consumed by production and private
# fixtures. Keeping the assembly here prevents workers/tests from reconstructing
# a second object graph or omitting one of the protocol/lifecycle/process
# owners.
set(CXXLENS_PROVIDER_RUNTIME_PRIVATE_OBJECTS
    $<TARGET_OBJECTS:cxxlens_process_authority_internal>
    $<TARGET_OBJECTS:cxxlens_provider_runtime_core_internal>
    $<TARGET_OBJECTS:cxxlens_provider_ng1_runtime_internal>
    $<TARGET_OBJECTS:cxxlens_provider_process_internal>
    $<TARGET_OBJECTS:cxxlens_provider_protocol_v2_internal>)
add_library(cxxlens_provider_runtime_private_closure INTERFACE)
target_sources(cxxlens_provider_runtime_private_closure
               INTERFACE ${CXXLENS_PROVIDER_RUNTIME_PRIVATE_OBJECTS})
target_link_libraries(cxxlens_provider_runtime_private_closure
                      INTERFACE cxxlens_protocol_v2)
# Embed the private implementation into the installed SDK itself so static and
# shared packages remain self-contained without exporting an internal target.
target_sources(cxxlens_sdk PRIVATE ${CXXLENS_PROVIDER_RUNTIME_PRIVATE_OBJECTS})
target_link_libraries(cxxlens_sdk PUBLIC cxxlens_protocol_v2)

add_library(cxxlens_clang22_provider_sdk ${CXXLENS_LIBRARY_TYPE}
                                         src/llvm/clang22/provider_sdk.cpp)
add_library(cxxlens::clang22_provider_sdk ALIAS cxxlens_clang22_provider_sdk)
cxxlens_configure_component(cxxlens_clang22_provider_sdk clang22_provider_sdk)
target_link_libraries(cxxlens_clang22_provider_sdk PUBLIC cxxlens::sdk)
target_include_directories(cxxlens_clang22_provider_sdk
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  cxxlens_configure_clang22(cxxlens_clang22_provider_sdk)
else()
  target_compile_definitions(cxxlens_clang22_provider_sdk
                             PRIVATE CXXLENS_HAS_CLANG22=0)
endif()
find_package(Threads REQUIRED)

# Native Clang 22 materialization and worker components are kept out of the
# compiler-neutral core profile. The public provider SDK stub remains available
# above so core consumers can compile and observe a structured unavailable mode.
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  # NFC validation is part of the installed materializer's self-contained-prefix
  # contract. Never let CMake silently select an ICU shared object or import
  # library for this private implementation.
  find_path(CXXLENS_ICU_STATIC_INCLUDE_DIR NAMES unicode/unorm2.h REQUIRED)
  if(WIN32)
    find_library(CXXLENS_ICUUC_STATIC_ARCHIVE NAMES sicuuc REQUIRED)
    find_library(CXXLENS_ICUDATA_STATIC_ARCHIVE NAMES sicudt REQUIRED)
    if(NOT CXXLENS_ICUUC_STATIC_ARCHIVE MATCHES "[/\\\\]sicuuc[^/\\\\]*\\.lib$"
       OR NOT CXXLENS_ICUDATA_STATIC_ARCHIVE MATCHES
          "[/\\\\]sicudt[^/\\\\]*\\.lib$")
      message(
        FATAL_ERROR
          "Static ICU proof failed: expected the ICU sicuuc/sicudt .lib naming contract"
      )
    endif()
  elseif(UNIX)
    set(_cxxlens_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(CXXLENS_ICUUC_STATIC_ARCHIVE NAMES icuuc REQUIRED)
    find_library(CXXLENS_ICUDATA_STATIC_ARCHIVE NAMES icudata REQUIRED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_cxxlens_saved_library_suffixes}")
    unset(_cxxlens_saved_library_suffixes)
    if(NOT CXXLENS_ICUUC_STATIC_ARCHIVE MATCHES "\\.a$"
       OR NOT CXXLENS_ICUDATA_STATIC_ARCHIVE MATCHES "\\.a$")
      message(
        FATAL_ERROR
          "Static ICU proof failed: icuuc and icudata must resolve to .a archives"
      )
    endif()
  else()
    message(
      FATAL_ERROR
        "Static ICU proof is not implemented for this platform; refusing a shared fallback"
    )
  endif()

  add_library(cxxlens_icu_static INTERFACE)
  target_include_directories(cxxlens_icu_static SYSTEM
                             INTERFACE "${CXXLENS_ICU_STATIC_INCLUDE_DIR}")
  target_compile_definitions(cxxlens_icu_static
                             INTERFACE U_STATIC_IMPLEMENTATION=1)
  target_link_libraries(
    cxxlens_icu_static
    INTERFACE "${CXXLENS_ICUUC_STATIC_ARCHIVE}"
              "${CXXLENS_ICUDATA_STATIC_ARCHIVE}" Threads::Threads)
  if(CMAKE_DL_LIBS)
    target_link_libraries(cxxlens_icu_static INTERFACE "${CMAKE_DL_LIBS}")
  endif()
  if(UNIX AND NOT APPLE)
    find_library(CXXLENS_MATH_LIBRARY NAMES m REQUIRED)
    target_link_libraries(cxxlens_icu_static
                          INTERFACE "${CXXLENS_MATH_LIBRARY}")
  endif()

  # Canonical task/observation codecs stay private to provider-owned execution
  # and materialization, without importing the Clang parser implementation.
  add_library(
    cxxlens_clang22_materialization_codecs STATIC
    src/llvm/clang22/provider_task_v4.cpp
    src/llvm/clang22/observation_v2.cpp
    src/llvm/clang22/source_closure.cpp
    src/llvm/clang22/source_closure_fd.cpp
    src/llvm/clang22/source_closure_receiver.cpp
    src/llvm/clang22/source_closure_spool.cpp
    src/llvm/clang22/source_closure_task_v4.cpp
    src/llvm/clang22/source_closure_transport.cpp
    src/llvm/clang22/source_closure_invocation.cpp
    src/llvm/clang22/source_closure_vfs.cpp
    src/llvm/clang22/unicode_nfc.cpp
    src/llvm/clang22/materialization_identity.cpp
    src/llvm/clang22/provider_worker_ingress.cpp)
  target_link_libraries(
    cxxlens_clang22_materialization_codecs
    PUBLIC cxxlens::sdk
    PRIVATE cxxlens_icu_static cxxlens_protocol_v2)
  # The provider SDK DSO keeps its Protocol 2 adapter definitions hidden. Give
  # the source-private codec archive its own frame/closure adapter object
  # without importing the heartbeat adapter's NG1 runtime dependency.
  target_sources(
    cxxlens_clang22_materialization_codecs
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_protocol_v2_internal>)
  target_include_directories(cxxlens_clang22_materialization_codecs
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materialization_codecs
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_materialization_codecs)
  cxxlens_enable_sanitizers(cxxlens_clang22_materialization_codecs)

  # Rooted SQLite VFS is a product safety primitive, not a dependency of the
  # installed materializer. Keep its implementation in a source-private archive
  # so focused VFS tests and prior-artifact reuse share one implementation
  # without pulling unrelated request/task code.
  add_library(cxxlens_clang22_materializer_rooted_vfs_support STATIC
              EXCLUDE_FROM_ALL src/llvm/clang22/materialization_rooted_vfs.cpp)
  target_link_libraries(
    cxxlens_clang22_materializer_rooted_vfs_support
    PUBLIC cxxlens::base
    PRIVATE cxxlens::kernel cxxlens::sdk cxxlens_clang22_materialization_codecs
            cxxlens::sdk)
  target_include_directories(cxxlens_clang22_materializer_rooted_vfs_support
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materializer_rooted_vfs_support
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_materializer_rooted_vfs_support)
  cxxlens_enable_sanitizers(cxxlens_clang22_materializer_rooted_vfs_support)

  # Installed occurrence identity is an artifact-integrity contract. Its
  # implementation is independent of the materializer admission path and is
  # therefore kept in its own source-private archive for the focused test and
  # future installed-consumer binding.
  add_library(cxxlens_clang22_materializer_occurrence_support STATIC
              EXCLUDE_FROM_ALL src/llvm/clang22/materialization_occurrence.cpp)
  target_link_libraries(
    cxxlens_clang22_materializer_occurrence_support
    PUBLIC cxxlens::base
    PRIVATE cxxlens::kernel cxxlens::sdk cxxlens_clang22_materialization_codecs
            cxxlens_clang22_materializer_rooted_vfs_support)
  target_include_directories(cxxlens_clang22_materializer_occurrence_support
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materializer_occurrence_support
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_materializer_occurrence_support)
  cxxlens_enable_sanitizers(cxxlens_clang22_materializer_occurrence_support)

  add_library(
    cxxlens_clang22_materialization_request_v2_2_support STATIC EXCLUDE_FROM_ALL
    src/llvm/clang22/materialization_request_v2_2.cpp)
  target_link_libraries(
    cxxlens_clang22_materialization_request_v2_2_support
    PUBLIC cxxlens::base
    PRIVATE cxxlens::sdk cxxlens_clang22_materialization_codecs)
  target_include_directories(
    cxxlens_clang22_materialization_request_v2_2_support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materialization_request_v2_2_support
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(
    cxxlens_clang22_materialization_request_v2_2_support)
  cxxlens_enable_sanitizers(
    cxxlens_clang22_materialization_request_v2_2_support)

  # The task-v4 claim boundary is source-private and remains a first-class typed
  # boundary so the v4 authority/receipt contract cannot depend on obsolete
  # request or task implementations.
  add_library(cxxlens_clang22_materializer_v4_claims STATIC EXCLUDE_FROM_ALL
              src/llvm/clang22/materialization_v4_claim_binding.cpp)
  target_link_libraries(
    cxxlens_clang22_materializer_v4_claims
    PUBLIC cxxlens::base
    PRIVATE cxxlens::kernel cxxlens_clang22_materialization_codecs)
  target_include_directories(cxxlens_clang22_materializer_v4_claims
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materializer_v4_claims
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_materializer_v4_claims)
  cxxlens_enable_sanitizers(cxxlens_clang22_materializer_v4_claims)

  # Worker/parser implementation stays out of the installed Clang 22 SDK DSO. It
  # is linked only by provider-owned executables and source-private tests.
  add_library(
    cxxlens_clang22_worker_core STATIC
    src/llvm/clang22/provider_worker_channel.cpp
    src/llvm/clang22/source_closure_native.cpp
    src/llvm/clang22/provider_worker_v4.cpp
    src/llvm/clang22/provider_worker_v4_ast_observer.cpp
    src/llvm/clang22/provider_worker_v4_output_normalizer.cpp)
  target_link_libraries(
    cxxlens_clang22_worker_core
    PUBLIC cxxlens::clang22_provider_sdk
    PRIVATE cxxlens_clang22_materialization_codecs)
  target_include_directories(cxxlens_clang22_worker_core
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_worker_core PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_worker_core)
  cxxlens_enable_sanitizers(cxxlens_clang22_worker_core)
  cxxlens_configure_clang22(cxxlens_clang22_worker_core)

  # The installed materializer boundary owns v2.2 ingress, Protocol 2.0 worker
  # handoff, and Clang claim adaptation. Compiler-neutral Store publication is
  # owned by the compiler-neutral SDK writer lifecycle.
  add_library(
    cxxlens_clang22_materializer_ingress STATIC
    src/llvm/clang22/installed_materializer_source_closure.cpp
    src/llvm/clang22/materializer_report_v2_2_encoder.cpp
    src/llvm/clang22/materializer_worker_bridge.cpp
    src/llvm/clang22/provider_trust_issuer_internal.cpp)
  target_link_libraries(
    cxxlens_clang22_materializer_ingress
    PUBLIC cxxlens::base cxxlens::sdk
    PRIVATE cxxlens_clang22_materialization_codecs
            cxxlens_icu_static
            cxxlens_clang22_materialization_request_v2_2_support
            cxxlens_clang22_worker_core
            cxxlens_clang22_materializer_rooted_vfs_support
            cxxlens_bounded_store_v6_memory_internal
            cxxlens_bounded_store_v6_sqlite_internal
            cxxlens_clang22_materializer_occurrence_support
            cxxlens_clang22_materializer_v4_claims)
  if(CXXLENS_BUILD_SHARED)
    target_link_libraries(cxxlens_clang22_materializer_ingress
                          PRIVATE cxxlens_provider_runtime_private_closure)
  endif()
  target_include_directories(cxxlens_clang22_materializer_ingress
                             PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens_clang22_materializer_ingress
                          PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens_clang22_materializer_ingress)
  cxxlens_enable_sanitizers(cxxlens_clang22_materializer_ingress)
endif()

add_executable(cxxlens-provider-scaffold tools/sdk/provider_scaffold_main.cpp)
target_link_libraries(cxxlens-provider-scaffold PRIVATE cxxlens::sdk)
target_compile_features(cxxlens-provider-scaffold PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-provider-scaffold)
cxxlens_enable_sanitizers(cxxlens-provider-scaffold)
cxxlens_configure_installed_tool(cxxlens-provider-scaffold)

add_executable(cxxlens-sdk-doctor tools/sdk/sdk_doctor_main.cpp)
target_link_libraries(cxxlens-sdk-doctor PRIVATE cxxlens::sdk)
target_compile_features(cxxlens-sdk-doctor PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-sdk-doctor)
cxxlens_enable_sanitizers(cxxlens-sdk-doctor)
cxxlens_configure_installed_tool(cxxlens-sdk-doctor)

# The user-facing command is intentionally thin: it delegates doctor/run parsing
# to the product-only resolver while keeping one stable installed entry point
# for SDK consumers. Native materialization remains behind the sealed Clang22
# worker/materializer tools and never accepts shell-concatenated input.
add_executable(cxxlens_cli tools/sdk/cxxlens_main.cpp
                           tools/sdk/sdk_doctor_main.cpp)
target_compile_definitions(cxxlens_cli PRIVATE CXXLENS_SDK_DOCTOR_NO_MAIN)
set_target_properties(cxxlens_cli PROPERTIES OUTPUT_NAME cxxlens)
target_link_libraries(cxxlens_cli PRIVATE cxxlens::sdk)
target_include_directories(cxxlens_cli
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens_cli PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens_cli)
cxxlens_enable_sanitizers(cxxlens_cli)
cxxlens_configure_installed_tool(cxxlens_cli)

if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  add_executable(cxxlens-clang-worker-22
                 src/llvm/clang22/provider_worker_main.cpp)
  target_link_libraries(
    cxxlens-clang-worker-22 PRIVATE cxxlens_clang22_worker_core
                                    cxxlens_clang22_materialization_codecs)
  if(CXXLENS_BUILD_SHARED)
    # The worker SDK is a static private closure when the public SDK is shared.
    # Its provider implementation references the hidden runtime boundary, so
    # consume the same private closure instead of rebuilding its object list.
    target_link_libraries(cxxlens-clang-worker-22
                          PRIVATE cxxlens_provider_runtime_private_closure)
  endif()
  target_include_directories(cxxlens-clang-worker-22
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens-clang-worker-22 PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang-worker-22)
  cxxlens_enable_sanitizers(cxxlens-clang-worker-22)
  cxxlens_configure_installed_clang22_tool(cxxlens-clang-worker-22)

  add_executable(cxxlens-clang22-materialize
                 tools/clang22/materialize_main_v2.cpp)
  target_link_libraries(cxxlens-clang22-materialize
                        PRIVATE cxxlens_clang22_materializer_ingress)
  target_include_directories(cxxlens-clang22-materialize
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(cxxlens-clang22-materialize PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang22-materialize)
  cxxlens_enable_sanitizers(cxxlens-clang22-materialize)
  cxxlens_configure_installed_clang22_tool(cxxlens-clang22-materialize)
endif()

# Keep the product dependency direction executable at configure time.  Alias
# names and object-library source expressions are normalized to their owning
# targets before checking; external/system targets remain the responsibility of
# their enclosing frontend or physical port.
function(cxxlens_assert_direct_project_dependencies target)
  set(_cxxlens_allowed_targets ${ARGN})
  set(_cxxlens_direct_entries)
  foreach(_cxxlens_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
                                     SOURCES)
    get_target_property(_cxxlens_property_entries "${target}"
                        "${_cxxlens_property}")
    if(_cxxlens_property_entries)
      list(APPEND _cxxlens_direct_entries ${_cxxlens_property_entries})
    endif()
  endforeach()

  foreach(_cxxlens_entry IN LISTS _cxxlens_direct_entries)
    if(_cxxlens_entry MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
      set(_cxxlens_dependency "${CMAKE_MATCH_1}")
    elseif(TARGET "${_cxxlens_entry}")
      set(_cxxlens_dependency "${_cxxlens_entry}")
    else()
      continue()
    endif()

    get_target_property(_cxxlens_aliased_target "${_cxxlens_dependency}"
                        ALIASED_TARGET)
    if(_cxxlens_aliased_target)
      set(_cxxlens_dependency "${_cxxlens_aliased_target}")
    endif()
    if(_cxxlens_dependency MATCHES "^cxxlens($|_)"
       AND NOT _cxxlens_dependency IN_LIST _cxxlens_allowed_targets)
      message(
        FATAL_ERROR
          "Forbidden direct cxxlens dependency: ${target} -> ${_cxxlens_dependency}"
      )
    endif()
  endforeach()
endfunction()

cxxlens_assert_direct_project_dependencies(cxxlens_base)
cxxlens_assert_direct_project_dependencies(cxxlens_kernel cxxlens_base)
cxxlens_assert_direct_project_dependencies(cxxlens_query cxxlens_kernel)
cxxlens_assert_direct_project_dependencies(cxxlens_cpp cxxlens_query)
cxxlens_assert_direct_project_dependencies(cxxlens_recipes cxxlens_query)
cxxlens_assert_direct_project_dependencies(cxxlens cxxlens_base cxxlens_kernel
                                           cxxlens_query cxxlens_cpp)
cxxlens_assert_direct_project_dependencies(cxxlens_protocol_v2 cxxlens_base)
cxxlens_assert_direct_project_dependencies(cxxlens_provider_protocol_v2_internal
                                           cxxlens_protocol_v2)
cxxlens_assert_direct_project_dependencies(
  cxxlens_provider_runtime_core_internal cxxlens cxxlens_recipes)
cxxlens_assert_direct_project_dependencies(cxxlens_provider_ng1_runtime_internal
                                           cxxlens cxxlens_recipes)
cxxlens_assert_direct_project_dependencies(cxxlens_provider_process_internal
                                           cxxlens cxxlens_recipes)
cxxlens_assert_direct_project_dependencies(cxxlens_process_authority_internal
                                           cxxlens_base)
cxxlens_assert_direct_project_dependencies(
  cxxlens_provider_runtime_private_closure
  cxxlens_process_authority_internal
  cxxlens_provider_runtime_core_internal
  cxxlens_provider_ng1_runtime_internal
  cxxlens_provider_process_internal
  cxxlens_provider_protocol_v2_internal
  cxxlens_protocol_v2)
cxxlens_assert_direct_project_dependencies(
  cxxlens_sdk
  cxxlens
  cxxlens_recipes
  cxxlens_protocol_v2
  cxxlens_process_authority_internal
  cxxlens_provider_runtime_core_internal
  cxxlens_provider_ng1_runtime_internal
  cxxlens_provider_process_internal
  cxxlens_provider_protocol_v2_internal
  cxxlens_bounded_store_v6_memory_internal
  cxxlens_bounded_store_v6_sqlite_internal)
cxxlens_assert_direct_project_dependencies(cxxlens_clang22_provider_sdk
                                           cxxlens_sdk)
if(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER)
  cxxlens_assert_direct_project_dependencies(
    cxxlens_detached_run_crypto_internal cxxlens_sdk)
  cxxlens_assert_direct_project_dependencies(
    cxxlens_detached_run_signing_file_port_internal cxxlens_sdk)
  cxxlens_assert_direct_project_dependencies(
    cxxlens_detached_run_trust_file_port_internal cxxlens_sdk)
  cxxlens_assert_direct_project_dependencies(
    cxxlens_clang23_gcc_replay_ingress cxxlens_sdk cxxlens_protocol_v2
    cxxlens_detached_run_crypto_internal)
  cxxlens_assert_direct_project_dependencies(
    cxxlens_clangcl_worker_command_internal cxxlens_clang23_gcc_replay_ingress
    cxxlens_detached_run_signing_file_port_internal
    cxxlens_detached_run_crypto_internal)
endif()

set(CXXLENS_COMPILED_TARGETS
    cxxlens_base
    cxxlens_protocol_v2
    cxxlens_kernel
    cxxlens_query
    cxxlens_recipes
    cxxlens_provider_runtime_core_internal
    cxxlens_provider_ng1_runtime_internal
    cxxlens_provider_process_internal
    cxxlens_process_authority_internal
    cxxlens_provider_protocol_v2_internal
    cxxlens_sdk
    cxxlens_cli
    cxxlens_clang22_provider_sdk)
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  list(APPEND CXXLENS_COMPILED_TARGETS cxxlens_clang22_materialization_codecs
       cxxlens_clang22_worker_core cxxlens_clang22_materializer_ingress)
endif()
if(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER)
  list(
    APPEND
    CXXLENS_COMPILED_TARGETS
    cxxlens_detached_run_crypto_internal
    cxxlens_detached_run_signing_file_port_internal
    cxxlens_detached_run_trust_file_port_internal
    cxxlens_clang23_gcc_replay_ingress
    cxxlens_clangcl_worker_command_internal)
endif()

if(CXXLENS_ENABLE_CLANG_TIDY)
  find_program(CXXLENS_CLANG_TIDY NAMES clang-tidy-22 clang-tidy REQUIRED)
  foreach(target IN LISTS CXXLENS_COMPILED_TARGETS)
    set_target_properties(
      ${target} PROPERTIES CXX_CLANG_TIDY
                           "${CXXLENS_CLANG_TIDY};--warnings-as-errors=*")
  endforeach()
endif()

if(CXXLENS_ENABLE_IWYU)
  find_program(CXXLENS_IWYU NAMES include-what-you-use iwyu REQUIRED)
  foreach(target IN LISTS CXXLENS_COMPILED_TARGETS)
    set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE
                                               "${CXXLENS_IWYU}")
  endforeach()
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Developer-only quality registration is never part of a production/install
# configuration.  Documentation remains available independently when requested,
# while BUILD_TESTING=OFF keeps Python/format tooling out of the package build.
if((BUILD_TESTING AND CXXLENS_BUILD_QUALITY_TOOLS) OR CXXLENS_BUILD_DOCS)
  include(CxxlensDeveloperTools)
endif()

# Test-only targets are gated above and are not installed. The package surface
# below remains available with BUILD_TESTING=OFF and contains only product
# libraries, tools, headers, schemas, and documentation.
install(
  TARGETS cxxlens_base
          cxxlens_kernel
          cxxlens_query
          cxxlens_cpp
          cxxlens_recipes
          cxxlens
          cxxlens_sdk
          cxxlens_protocol_v2
  EXPORT cxxlensTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS cxxlens_cli cxxlens-provider-scaffold cxxlens-sdk-doctor
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  install(
    TARGETS cxxlens_clang22_provider_sdk
    EXPORT cxxlensClang22ProviderSDKTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(TARGETS cxxlens-clang-worker-22 cxxlens-clang22-materialize
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(CXXLENS_BUILD_CLANG23_GCC_REPLAY_WORKER)
  install(TARGETS cxxlens-clang-gcc-replay-worker-23
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                  COMPONENT clang23-gcc-replay-worker)
  get_target_property(_cxxlens_clang23_runtime clang-cpp
                      IMPORTED_LOCATION_RELEASE)
  if(NOT _cxxlens_clang23_runtime OR NOT EXISTS "${_cxxlens_clang23_runtime}")
    message(FATAL_ERROR "The exact Clang 23 runtime artifact is unavailable")
  endif()
  find_file(
    _cxxlens_clang23_license
    NAMES LICENSE.TXT
    PATHS ${LLVM_INCLUDE_DIRS}/llvm/Support
    NO_DEFAULT_PATH NO_CACHE REQUIRED)
  install(
    FILES "${_cxxlens_clang23_runtime}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT clang23-gcc-replay-worker)
  install(
    FILES LICENSE NOTICE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens"
    COMPONENT clang23-gcc-replay-worker)
  install(
    FILES "${_cxxlens_clang23_license}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens"
    RENAME LLVM-LICENSE.TXT
    COMPONENT clang23-gcc-replay-worker)
endif()
install(
  PROGRAMS tools/sdk/relation_idl_compiler.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  RENAME cxxlens-relation-idl)

install(FILES include/cxxlens/cxxlens.hpp include/cxxlens/sdk.hpp
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxxlens")
install(
  DIRECTORY include/cxxlens/sdk include/cxxlens/provider
            include/cxxlens/relations
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxxlens"
  PATTERN "testing.hpp" EXCLUDE)
# Install only product authority.  The repository also contains conformance
# vectors, differential corpora, compatibility fixtures, and retired protocol
# schemas; those are development inputs rather than installed product data.
set(CXXLENS_PRODUCT_SCHEMA_FILES
    schemas/cxxlens_application_analysis_support.schema.yaml
    schemas/cxxlens_application_analysis_support.yaml
    schemas/cxxlens_build_capture_bundle.yaml
    schemas/cxxlens_compiler_replay_plan.yaml
    schemas/cxxlens_detached_provider_run.yaml
    schemas/cxxlens_compiler_replay_input.yaml
    schemas/cxxlens_ng_claim_envelope.schema.yaml
    schemas/cxxlens_ng_clang22_materialization_contract.yaml
    schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml
    schemas/cxxlens_ng_clang22_materialization_report.schema.yaml
    schemas/cxxlens_ng_clang22_materialization_request_v2_2.schema.yaml
    schemas/cxxlens_ng_clang22_materializer_occurrence_manifest.schema.yaml
    schemas/cxxlens_ng_compatibility_report.schema.yaml
    schemas/cxxlens_ng_compatibility_request.schema.yaml
    schemas/cxxlens_ng_logical_query_contract.yaml
    schemas/cxxlens_ng_logical_query_contract.schema.yaml
    schemas/cxxlens_ng_logical_query_ir.schema.yaml
    schemas/cxxlens_ng_namespace_registry.yaml
    schemas/cxxlens_ng_namespace_registry.schema.yaml
    schemas/cxxlens_ng_portable_provider_task_contract.yaml
    schemas/cxxlens_ng_portable_provider_task_contract.schema.yaml
    schemas/cxxlens_ng_project_catalog_contract.yaml
    schemas/cxxlens_ng_project_catalog_contract.schema.yaml
    schemas/cxxlens_ng_provider_certification_registry.yaml
    schemas/cxxlens_ng_provider_certification_registry.schema.yaml
    schemas/cxxlens_ng_provider_execution_report.schema.yaml
    schemas/cxxlens_ng_provider_manifest.schema.yaml
    schemas/cxxlens_ng_provider_ng1_hardening.yaml
    schemas/cxxlens_ng_provider_ng1_hardening.schema.yaml
    schemas/cxxlens_ng_provider_protocol_v2.yaml
    schemas/cxxlens_ng_provider_protocol_v2.schema.yaml
    schemas/cxxlens_ng_provider_runtime_contract.yaml
    schemas/cxxlens_ng_provider_runtime_contract.schema.yaml
    schemas/cxxlens_ng_provider_spill_fsync_receipt.schema.yaml
    schemas/cxxlens_ng_provider_support_matrix.yaml
    schemas/cxxlens_ng_provider_support_matrix.schema.yaml
    schemas/cxxlens_ng_provider_task_v4.schema.yaml
    schemas/cxxlens_ng_public_api_catalog.yaml
    schemas/cxxlens_ng_public_api_catalog.schema.yaml
    schemas/cxxlens_ng_query_execution_result.schema.yaml
    schemas/cxxlens_ng_query_runtime_contract.yaml
    schemas/cxxlens_ng_query_runtime_contract.schema.yaml
    schemas/cxxlens_ng_relation_registry.yaml
    schemas/cxxlens_ng_relation_registry.schema.yaml
    schemas/cxxlens_ng_sdk_doctor_catalog.yaml
    schemas/cxxlens_ng_sdk_doctor_catalog.schema.yaml
    schemas/cxxlens_ng_sdk_doctor_project.schema.yaml
    schemas/cxxlens_ng_sdk_doctor_relation_presence.schema.yaml
    schemas/cxxlens_ng_sdk_doctor_resolution.schema.yaml
    schemas/cxxlens_ng_security_profile.yaml
    schemas/cxxlens_ng_security_profile.schema.yaml
    schemas/cxxlens_ng_semantic_guarantee_contract.yaml
    schemas/cxxlens_ng_semantic_guarantee_contract.schema.yaml
    schemas/cxxlens_ng_semantic_metadata.schema.yaml
    schemas/cxxlens_ng_snapshot_manifest.schema.yaml
    schemas/cxxlens_ng_snapshot_store_contract.yaml
    schemas/cxxlens_ng_snapshot_store_contract.schema.yaml
    schemas/cxxlens_ng_source_closure_manifest_v1.schema.yaml
    schemas/cxxlens_ng_source_closure_transport.yaml
    schemas/cxxlens_ng_source_closure_transport.schema.yaml
    schemas/cxxlens_ng_sqlite_store_contract.yaml
    schemas/cxxlens_ng_sqlite_store_contract.schema.yaml
    schemas/cxxlens_support_matrix.yaml
    schemas/cxxlens_support_matrix.schema.yaml)
install(FILES ${CXXLENS_PRODUCT_SCHEMA_FILES}
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxlens/schemas")
install(FILES README.md SECURITY.md LICENSE NOTICE
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens")
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

if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  configure_package_config_file(
    cmake/cxxlensClang22ProviderSDKConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfig.cmake"
    INSTALL_DESTINATION
      "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensClang22ProviderSDK")
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
endif()

# The occurrence manifest inventories the final installed executables, schemas,
# and (for shared packages) runtime libraries.  Generate it only after all
# inventoried install rules have completed so its digests bind the actual
# package, including a relocated install prefix.
if(CXXLENS_BUILD_CLANG22_COMPONENTS)
  install(
    SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/GenerateClang22OccurrenceManifest.cmake"
  )
endif()
