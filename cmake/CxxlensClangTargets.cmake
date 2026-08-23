set(CXXLENS_CLANG_ADAPTER
    "AUTO"
    CACHE STRING "Build the exact Clang 22 adapter: AUTO, ON, or OFF")
set_property(CACHE CXXLENS_CLANG_ADAPTER PROPERTY STRINGS AUTO ON OFF)

# cmake-format: off
function(cxxlens_configure_clang22_worker_static_component target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  target_include_directories(
    ${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  set_target_properties(
    ${target}
    PROPERTIES CXX_EXTENSIONS OFF
               POSITION_INDEPENDENT_CODE ON
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
  cxxlens_set_project_warnings(${target})
  cxxlens_enable_sanitizers(${target})
endfunction()

function(cxxlens_create_clang22_worker_static_closure)
  if(NOT CXXLENS_BUILD_SHARED
     OR NOT TARGET cxxlens-clang-worker-22
     OR TARGET cxxlens_clang22_worker_provider_sdk_internal)
    return()
  endif()

  # A verified provider executable is copied to a sealed memfd and launched
  # with execveat(AT_EMPTY_PATH). Consequently $ORIGIN is the memfd execution
  # image, not the relocated installation prefix. Keep the public shared SDK
  # unchanged, but make the worker's cxxlens-owned implementation a private
  # static closure so the sealed image never needs lib/cxxlens DSOs.
  add_library(cxxlens_clang22_worker_base_internal STATIC EXCLUDE_FROM_ALL
              src/sdk/common.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_base_internal)

  add_library(
    cxxlens_clang22_worker_kernel_internal STATIC EXCLUDE_FROM_ALL
    src/sdk/relation.cpp
    src/sdk/claim.cpp
    src/sdk/store.cpp
    src/runtime/monotonic_clock_port.cpp
    src/sdk/sqlite_connection_lifecycle_internal.cpp
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
    # The sealed shared worker is a production closure.  It must use the same
    # inert dispatcher as the installed kernel; the thread-local fault scope is
    # source-private test support and must not enter the shipped image.
    src/sdk/sqlite_store_fault_injection_noop_internal.cpp
    src/sdk/sqlite_store_terminal_internal.cpp
    src/sdk/sqlite_terminal_reclassifier_internal.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_kernel_internal)
  target_link_libraries(
    cxxlens_clang22_worker_kernel_internal
    PUBLIC cxxlens_clang22_worker_base_internal
    PRIVATE "${CMAKE_DL_LIBS}")

  add_library(
    cxxlens_clang22_worker_query_internal STATIC EXCLUDE_FROM_ALL
    src/sdk/query.cpp src/sdk/query_execution.cpp src/sdk/query_ir_decoder.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_query_internal)
  target_link_libraries(cxxlens_clang22_worker_query_internal
                        PUBLIC cxxlens_clang22_worker_kernel_internal)

  add_library(cxxlens_clang22_worker_recipes_internal STATIC EXCLUDE_FROM_ALL
              src/sdk/recipe.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_recipes_internal)
  target_link_libraries(cxxlens_clang22_worker_recipes_internal
                        PUBLIC cxxlens_clang22_worker_query_internal)

  # provider_runtime.cpp and provider_process_adapter.cpp are already embedded
  # as the hidden cxxlens_provider_runtime_internal object closure by #224.
  # Rebuild only the remaining provider SDK objects against the worker-private
  # static semantic stack; do not duplicate the runtime object definitions.
  add_library(
    cxxlens_clang22_worker_provider_sdk_internal STATIC EXCLUDE_FROM_ALL
    src/sdk/provider.cpp src/sdk/incremental.cpp
    src/sdk/provider_protocol_v2_adapter.cpp
    src/protocol_v2/cbor.cpp src/protocol_v2/codec.cpp
    src/protocol_v2/closure.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_provider_sdk_internal)
  target_link_libraries(
    cxxlens_clang22_worker_provider_sdk_internal
    PUBLIC cxxlens_clang22_worker_query_internal
           cxxlens_clang22_worker_recipes_internal)

  add_library(
    cxxlens_clang22_worker_native_sdk_internal STATIC EXCLUDE_FROM_ALL
    src/llvm/clang22/provider_sdk.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_native_sdk_internal)
  target_link_libraries(cxxlens_clang22_worker_native_sdk_internal
                        PUBLIC cxxlens_clang22_worker_provider_sdk_internal)
  cxxlens_configure_clang22(cxxlens_clang22_worker_native_sdk_internal)

  add_library(
    cxxlens_clang22_worker_codecs_internal STATIC EXCLUDE_FROM_ALL
    src/llvm/clang22/materialization_json.cpp
    src/llvm/clang22/provider_task_v4.cpp
    src/llvm/clang22/observation_v2.cpp
    src/llvm/clang22/source_closure.cpp
    src/llvm/clang22/source_closure_receiver.cpp
    src/llvm/clang22/source_closure_spool.cpp
    src/llvm/clang22/source_closure_task_v4.cpp
    src/llvm/clang22/source_closure_transport.cpp
    src/llvm/clang22/source_closure_invocation.cpp
    src/llvm/clang22/source_closure_vfs.cpp
    src/llvm/clang22/unicode_nfc.cpp)
  cxxlens_configure_clang22_worker_static_component(
    cxxlens_clang22_worker_codecs_internal)
  target_link_libraries(
    cxxlens_clang22_worker_codecs_internal
    PUBLIC cxxlens_clang22_worker_provider_sdk_internal
    PRIVATE cxxlens_icu_static)

  get_target_property(_cxxlens_worker_links cxxlens_clang22_worker_core
                      LINK_LIBRARIES)
  if(_cxxlens_worker_links
     AND NOT _cxxlens_worker_links STREQUAL
         "_cxxlens_worker_links-NOTFOUND")
    list(REMOVE_ITEM _cxxlens_worker_links
         "cxxlens::clang22_provider_sdk"
         "cxxlens_clang22_materialization_codecs")
    set_property(TARGET cxxlens_clang22_worker_core PROPERTY LINK_LIBRARIES
                                                            "${_cxxlens_worker_links}")
  endif()

  get_target_property(_cxxlens_worker_interface_links
                      cxxlens_clang22_worker_core INTERFACE_LINK_LIBRARIES)
  if(_cxxlens_worker_interface_links
     AND NOT _cxxlens_worker_interface_links STREQUAL
         "_cxxlens_worker_interface_links-NOTFOUND")
    list(REMOVE_ITEM _cxxlens_worker_interface_links
         "cxxlens::clang22_provider_sdk"
         "cxxlens_clang22_materialization_codecs"
         "$<LINK_ONLY:cxxlens_clang22_materialization_codecs>")
    set_property(
      TARGET cxxlens_clang22_worker_core PROPERTY INTERFACE_LINK_LIBRARIES
                                                   "${_cxxlens_worker_interface_links}")
  endif()

  target_link_libraries(
    cxxlens_clang22_worker_core
    PUBLIC cxxlens_clang22_worker_native_sdk_internal
    PRIVATE cxxlens_clang22_worker_codecs_internal)
endfunction()

function(cxxlens_apply_clang22_worker_install_rpath)
  if(NOT UNIX
     OR NOT TARGET cxxlens-clang-worker-22
     OR NOT CXXLENS_CLANG22_AVAILABLE
     OR NOT CXXLENS_CLANG22_LIBRARY_DIRS)
    return()
  endif()

  # The sealed worker is self-contained for cxxlens-owned code. Exact external
  # LLVM/Clang runtime directories remain measured toolchain dependencies and
  # must be available without ambient loader variables.
  get_target_property(_cxxlens_worker_install_rpath cxxlens-clang-worker-22
                      INSTALL_RPATH)
  if(NOT _cxxlens_worker_install_rpath
     OR _cxxlens_worker_install_rpath STREQUAL
        "_cxxlens_worker_install_rpath-NOTFOUND")
    set(_cxxlens_worker_install_rpath)
  endif()
  list(APPEND _cxxlens_worker_install_rpath ${CXXLENS_CLANG22_LIBRARY_DIRS})
  list(REMOVE_DUPLICATES _cxxlens_worker_install_rpath)
  set_target_properties(
    cxxlens-clang-worker-22
    PROPERTIES INSTALL_RPATH "${_cxxlens_worker_install_rpath}"
               INSTALL_RPATH_USE_LINK_PATH FALSE)
endfunction()

function(cxxlens_finalize_clang22_worker)
  cxxlens_create_clang22_worker_static_closure()
  if(TARGET cxxlens_clang22_provider_sdk)
    add_dependencies(cxxlens-clang-worker-22 cxxlens_clang22_provider_sdk)
  endif()
  cxxlens_apply_clang22_worker_install_rpath()
endfunction()

function(cxxlens_configure_clang22 target)
  set(CXXLENS_CLANG22_LIBRARY_DIRS
      ""
      CACHE INTERNAL "Exact LLVM/Clang 22 runtime library directories" FORCE)
  set(CXXLENS_CLANG22_UNAVAILABLE_REASON
      ""
      CACHE INTERNAL "Reason the exact Clang 22 adapter is unavailable" FORCE)
  set(CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY
      FALSE
      CACHE INTERNAL
        "Whether the last exact Clang 22 ASan boundary uses the packaged shared clang-cpp target"
        FORCE)
  set_property(TARGET ${target} PROPERTY CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY FALSE)
  if(NOT CXXLENS_CLANG_ADAPTER MATCHES "^(AUTO|ON|OFF)$")
    message(
      FATAL_ERROR
        "CXXLENS_CLANG_ADAPTER must be AUTO, ON, or OFF (got ${CXXLENS_CLANG_ADAPTER})"
    )
  endif()

  if(CXXLENS_CLANG_ADAPTER STREQUAL "OFF")
    target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=0)
    set(CXXLENS_CLANG22_UNAVAILABLE_REASON
        "disabled-by-user"
        CACHE INTERNAL "Reason the exact Clang 22 adapter is unavailable" FORCE)
    set(CXXLENS_CLANG22_AVAILABLE
        FALSE
        CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
    message(
      STATUS
        "Exact LLVM/Clang 22 adapter unavailable: CXXLENS_CLANG_ADAPTER=OFF (structured unavailable build)"
    )
    return()
  endif()

  set(_cxxlens_clang22_components
      LLVMOption
      LLVMSupport
      clangAST
      clangBasic
      clangDriver
      clangFrontend
      clangFrontendTool
      clangIndex
      clangLex
      clangOptions
      clangSerialization
      clangTooling
      clangToolingCore)
  set(_cxxlens_clang22_unavailable_reason "")
  find_package(LLVM 22.1 CONFIG QUIET)
  if(NOT LLVM_FOUND)
    set(_cxxlens_clang22_unavailable_reason "llvm-package-not-found")
  elseif(NOT LLVM_VERSION_MAJOR STREQUAL "22")
    set(_cxxlens_clang22_unavailable_reason "llvm-major-mismatch")
  elseif(NOT LLVM_CMAKE_DIR OR NOT LLVM_LIBRARY_DIRS)
    set(_cxxlens_clang22_unavailable_reason "llvm-package-metadata-incomplete")
  else()
    get_filename_component(_cxxlens_llvm_cmake_dir "${LLVM_CMAKE_DIR}" REALPATH)
    get_filename_component(_cxxlens_llvm_cmake_root
                           "${_cxxlens_llvm_cmake_dir}" DIRECTORY)
    find_library(
      _cxxlens_clang_basic_library
      NAMES clangBasic
      PATHS ${LLVM_LIBRARY_DIRS}
      NO_DEFAULT_PATH NO_CACHE)
    if(NOT _cxxlens_clang_basic_library)
      set(_cxxlens_clang22_unavailable_reason "clang-basic-library-missing")
    else()
      find_package(Clang CONFIG QUIET PATHS "${_cxxlens_llvm_cmake_root}/clang"
                   NO_DEFAULT_PATH)
      if(NOT Clang_FOUND)
        set(_cxxlens_clang22_unavailable_reason "clang-package-not-found")
      endif()
    endif()
  endif()

  if(_cxxlens_clang22_unavailable_reason STREQUAL "")
    set(_cxxlens_clang22_missing_targets "")
    foreach(component IN LISTS _cxxlens_clang22_components)
      if(NOT TARGET ${component})
        list(APPEND _cxxlens_clang22_missing_targets "${component}")
      endif()
    endforeach()
    if(_cxxlens_clang22_missing_targets)
      string(JOIN "," _cxxlens_clang22_missing_target_text
             ${_cxxlens_clang22_missing_targets})
      set(_cxxlens_clang22_unavailable_reason
          "required-targets-missing:${_cxxlens_clang22_missing_target_text}")
    endif()
  endif()

  if(NOT _cxxlens_clang22_unavailable_reason STREQUAL "")
    set(CXXLENS_CLANG22_UNAVAILABLE_REASON
        "${_cxxlens_clang22_unavailable_reason}"
        CACHE INTERNAL "Reason the exact Clang 22 adapter is unavailable" FORCE)
    if(CXXLENS_CLANG_ADAPTER STREQUAL "ON")
      message(
        FATAL_ERROR
          "Exact LLVM/Clang 22 development packages are required when CXXLENS_CLANG_ADAPTER=ON (reason=${_cxxlens_clang22_unavailable_reason}; LLVM_DIR=${LLVM_DIR}; Clang_DIR=${Clang_DIR})"
      )
    endif()
    message(
      STATUS
        "Exact LLVM/Clang 22 unavailable (reason=${_cxxlens_clang22_unavailable_reason}; LLVM_DIR=${LLVM_DIR}; Clang_DIR=${Clang_DIR}); building the structured unavailable adapter"
    )
    target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=0)
    set(CXXLENS_CLANG22_AVAILABLE
        FALSE
        CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
    return()
  endif()

  target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=1)
  target_compile_definitions(
    ${target} PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS
                      __STDC_LIMIT_MACROS)
  # LLVM/Clang 22 release archives are built with RTTI disabled.  Match that ABI
  # at every exact-adapter compilation boundary so provider-owned classes do not
  # introduce unresolved base-class typeinfo into the static worker.
  if(NOT LLVM_ENABLE_RTTI)
    if(MSVC)
      target_compile_options(${target} PRIVATE /GR-)
    else()
      target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
  endif()
  target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
                                                      ${CLANG_INCLUDE_DIRS})
  # The exact LLVM 22 distribution exports both non-PIC component archives and
  # a shared clang-cpp DSO. A shared public SDK cannot embed those archives: the
  # transitive LLVMSupport closure includes a non-PIC zstd archive and the link
  # must fail closed instead of producing a text-relocation DSO. The same
  # boundary is required for UNIX ASan builds, including the private worker
  # native SDK closure: the packaged archives are not sanitizer-instrumented,
  # while LLVM 22's allocator inline definitions are sanitizer-dependent.
  set(_cxxlens_use_shared_clang_cpp FALSE)
  if(UNIX AND CXXLENS_ENABLE_ASAN)
    set(_cxxlens_use_shared_clang_cpp TRUE)
  elseif(CXXLENS_BUILD_SHARED AND UNIX
         AND target STREQUAL "cxxlens_clang22_provider_sdk")
    set(_cxxlens_use_shared_clang_cpp TRUE)
  endif()
  if(_cxxlens_use_shared_clang_cpp)
    if(NOT TARGET clang-cpp)
      message(
        FATAL_ERROR
          "The exact Clang 22 boundary requires the packaged clang-cpp shared target"
      )
    endif()
    get_target_property(_cxxlens_clang_cpp_type clang-cpp TYPE)
    if(NOT _cxxlens_clang_cpp_type STREQUAL "SHARED_LIBRARY")
      message(
        FATAL_ERROR
          "The exact Clang 22 boundary requires clang-cpp to be a shared library target"
      )
    endif()
    # LLVM publishes two supported shared layouts: distribution archives may
    # export LLVM symbols directly from the monolithic clang-cpp DSO, while
    # distribution packages split those symbols into a separate LLVM DSO. In
    # the split layout, link LLVM explicitly because a transitive DT_NEEDED
    # entry from clang-cpp is not sufficient for direct references under GNU
    # ld. In the monolithic layout, clang-cpp is the complete shared boundary.
    target_link_libraries(${target} PRIVATE clang-cpp)
    if(TARGET LLVM)
      get_target_property(_cxxlens_llvm_type LLVM TYPE)
      if(NOT _cxxlens_llvm_type STREQUAL "SHARED_LIBRARY")
        message(
          FATAL_ERROR
            "The exact Clang 22 boundary requires LLVM to be a shared library target when the package exports an LLVM target"
        )
      endif()
      target_link_libraries(${target} PRIVATE LLVM)
    endif()
    if(UNIX AND CXXLENS_ENABLE_ASAN)
      set(CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY
          TRUE
          CACHE INTERNAL
            "Whether the last exact Clang 22 ASan boundary uses the packaged shared clang-cpp target"
            FORCE)
      set_property(TARGET ${target} PROPERTY CXXLENS_CLANG22_ASAN_SHARED_BOUNDARY TRUE)
    endif()
  else()
    target_link_libraries(${target} PRIVATE ${_cxxlens_clang22_components})
  endif()
  set(CXXLENS_CLANG22_EXPLICIT_COMPONENTS
      "${_cxxlens_clang22_components}"
      CACHE INTERNAL "Explicit Clang 22 link closure" FORCE)
  set(CXXLENS_CLANG22_LIBRARY_DIRS
      "${LLVM_LIBRARY_DIRS}"
      CACHE INTERNAL "Exact LLVM/Clang 22 runtime library directories" FORCE)
  if(UNIX AND LLVM_LIBRARY_DIRS)
    get_target_property(_cxxlens_existing_install_rpath ${target} INSTALL_RPATH)
    if(NOT _cxxlens_existing_install_rpath
       OR _cxxlens_existing_install_rpath STREQUAL
          "_cxxlens_existing_install_rpath-NOTFOUND")
      set(_cxxlens_existing_install_rpath)
    endif()
    list(APPEND _cxxlens_existing_install_rpath ${LLVM_LIBRARY_DIRS})
    set_target_properties(
      ${target} PROPERTIES INSTALL_RPATH "${_cxxlens_existing_install_rpath}"
                           INSTALL_RPATH_USE_LINK_PATH FALSE)
  endif()

  get_property(_cxxlens_worker_finalize_scheduled DIRECTORY
               PROPERTY CXXLENS_CLANG22_WORKER_FINALIZE_SCHEDULED)
  if(NOT _cxxlens_worker_finalize_scheduled)
    set_property(DIRECTORY PROPERTY CXXLENS_CLANG22_WORKER_FINALIZE_SCHEDULED TRUE)
    cmake_language(DEFER CALL cxxlens_finalize_clang22_worker)
  endif()

  set(CXXLENS_CLANG22_AVAILABLE
      TRUE
      CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
  message(STATUS "Enabled exact LLVM/Clang ${LLVM_PACKAGE_VERSION} adapter")
endfunction()
# cmake-format: on
