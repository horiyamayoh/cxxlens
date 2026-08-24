find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# SQLite lifecycle and Store fault peers are private test support. Keep their
# declarations visible to deterministic tests while production targets are
# compiled without the corresponding symbols.
add_compile_definitions(CXXLENS_SQLITE_TEST_SUPPORT=1
                        CXXLENS_STORE_FAULT_TEST_SUPPORT=1)

function(cxxlens_add_test_executable target source)
  add_executable(${target} ${source})
  target_compile_features(${target} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target})
  cxxlens_enable_sanitizers(${target})
endfunction()

# Provider transcript fault harnesses are test support, not part of the
# installed author SDK.  Keep the implementation in a test-only object library
# so product targets cannot accidentally export the testing namespace or its
# symbols.
add_library(cxxlens-test-sdk-support OBJECT
            "${PROJECT_SOURCE_DIR}/src/sdk/testing.cpp")
target_compile_features(cxxlens-test-sdk-support PRIVATE cxx_std_23)
target_include_directories(
  cxxlens-test-sdk-support PRIVATE "${PROJECT_SOURCE_DIR}/include"
                                   "${PROJECT_SOURCE_DIR}/src")
cxxlens_set_project_warnings(cxxlens-test-sdk-support)
cxxlens_enable_sanitizers(cxxlens-test-sdk-support)

# Only the three Store suites that exercise typed SQL fault boundaries link the
# private thread-local dispatcher. Production targets and unrelated tests do not
# link a fault entry point.
add_library(
  cxxlens-test-sqlite-store-fault-injection-support OBJECT
  "${PROJECT_SOURCE_DIR}/src/sdk/sqlite_store_fault_injection_internal.cpp")
target_compile_features(cxxlens-test-sqlite-store-fault-injection-support
                        PRIVATE cxx_std_23)
target_include_directories(
  cxxlens-test-sqlite-store-fault-injection-support
  PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
cxxlens_set_project_warnings(cxxlens-test-sqlite-store-fault-injection-support)
cxxlens_enable_sanitizers(cxxlens-test-sqlite-store-fault-injection-support)

cxxlens_add_test_executable(cxxlens-unit-sdk unit/sdk/sdk_test.cpp)
target_sources(cxxlens-unit-sdk
               PRIVATE $<TARGET_OBJECTS:cxxlens-test-sdk-support>)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-unit-sdk
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
  target_sources(
    cxxlens-unit-sdk
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-unit-sdk PRIVATE cxxlens_protocol_v2)
endif()
target_link_libraries(cxxlens-unit-sdk PRIVATE cxxlens::clang22_provider_sdk)
set(cxxlens_sdk_cases
    scalar-value-validation
    digest
    closed-enum-canonical-value
    descriptor-binding
    descriptor-canonical-collections
    claim-batch-digest-framing
    relation-schema-parity
    static-row-view-validation
    static-dynamic-query
    admitted-static-relation-apis
    container-reference-adoption
    snapshot-lifetime
    frame-native-escape
    columnar-wire-codec
    relation-sink-batch-state
    project-catalog-identity
    provider-tooling-faults
    relation-engine-claim-kernel)
foreach(case IN LISTS cxxlens_sdk_cases)
  add_test(NAME "unit.sdk.${case}" COMMAND cxxlens-unit-sdk "${case}")
  set_tests_properties(
    "unit.sdk.${case}" PROPERTIES LABELS "unit;sdk;relation;query;provider"
                                  PROCESSORS 1)
endforeach()

cxxlens_add_test_executable(cxxlens-unit-sdk-store unit/sdk/store_test.cpp)
target_sources(
  cxxlens-unit-sdk-store
  PRIVATE $<TARGET_OBJECTS:cxxlens-test-sqlite-store-fault-injection-support>)
target_link_libraries(cxxlens-unit-sdk-store PRIVATE cxxlens::kernel
                                                     "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-unit-sdk-store
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-store COMMAND cxxlens-unit-sdk-store)
# The store suite deliberately exercises process-global SQLite/VFS lifecycle and
# a set of fixed crash/tamper fixture paths.  Keep that one executable isolated
# from install fixture preparation and other SQLite processes while allowing the
# rest of the deterministic suite to run in parallel.
set_tests_properties(
  unit.sdk-store PROPERTIES LABELS "unit;sdk;store;transaction" RUN_SERIAL TRUE)

cxxlens_add_test_executable(cxxlens-unit-sdk-store-operation-port
                            unit/sdk/store_operation_port_test.cpp)
target_sources(cxxlens-unit-sdk-store-operation-port
               PRIVATE support/store_operation_test_adapter.cpp)
target_link_libraries(cxxlens-unit-sdk-store-operation-port
                      PRIVATE cxxlens::kernel)
target_include_directories(
  cxxlens-unit-sdk-store-operation-port
  PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/support")
add_test(NAME unit.sdk-store-operation-port
         COMMAND cxxlens-unit-sdk-store-operation-port)
set_tests_properties(
  unit.sdk-store-operation-port
  PROPERTIES LABELS "unit;sdk;store;operation-port;fault;determinism")

cxxlens_add_test_executable(cxxlens-unit-sdk-bounded-store-v6-memory-e2e
                            unit/sdk/bounded_store_v6_memory_e2e_test.cpp)
target_sources(cxxlens-unit-sdk-bounded-store-v6-memory-e2e
               PRIVATE support/store_operation_test_adapter.cpp)
target_link_libraries(
  cxxlens-unit-sdk-bounded-store-v6-memory-e2e
  PRIVATE cxxlens_bounded_store_v6_memory_internal cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-bounded-store-v6-memory-e2e
                           PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
add_test(NAME unit.sdk-bounded-store-v6-memory-e2e
         COMMAND cxxlens-unit-sdk-bounded-store-v6-memory-e2e)
set_tests_properties(
  unit.sdk-bounded-store-v6-memory-e2e
  PROPERTIES LABELS
             "unit;sdk;store;bounded;memory;v6;e2e;fault;determinism;resource")

cxxlens_add_test_executable(cxxlens-unit-sdk-snapshot-store-v5-codec
                            unit/sdk/snapshot_store_v5_codec_test.cpp)
target_link_libraries(cxxlens-unit-sdk-snapshot-store-v5-codec
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-snapshot-store-v5-codec
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-snapshot-store-v5-codec
         COMMAND cxxlens-unit-sdk-snapshot-store-v5-codec)
set_tests_properties(
  unit.sdk-snapshot-store-v5-codec
  PROPERTIES LABELS "unit;sdk;store;codec;fault;determinism;resource")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-disposable-qualification
                            unit/sdk/sqlite_disposable_qualification_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-disposable-qualification
                      PRIVATE cxxlens-sqlite-disposable-qualification-internal)
target_include_directories(
  cxxlens-unit-sdk-sqlite-disposable-qualification
  PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-disposable-qualification
         COMMAND cxxlens-unit-sdk-sqlite-disposable-qualification)
set_tests_properties(
  unit.sdk-sqlite-disposable-qualification
  PROPERTIES
    LABELS "unit;sdk;store;sqlite;qualification;capability;filesystem;security")

add_library(cxxlens-sqlite-store-v3-scenario-support STATIC
            support/sqlite_store_v3_scenario.cpp)
target_compile_features(cxxlens-sqlite-store-v3-scenario-support
                        PRIVATE cxx_std_23)
target_link_libraries(
  cxxlens-sqlite-store-v3-scenario-support
  PUBLIC cxxlens::kernel
  PRIVATE "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-sqlite-store-v3-scenario-support
                           PUBLIC "${PROJECT_SOURCE_DIR}/tests/support")
cxxlens_set_project_warnings(cxxlens-sqlite-store-v3-scenario-support)
cxxlens_enable_sanitizers(cxxlens-sqlite-store-v3-scenario-support)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_DL_LIBS}")
  check_cxx_source_compiles(
    [=[
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <features.h>
#ifndef __GLIBC__
#error "The SQLite interposer tests require glibc"
#endif
#include <dlfcn.h>
#include <link.h>
int main() {
  auto* versioned = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
  auto* isolated = dlmopen(LM_ID_NEWLM, nullptr, RTLD_LAZY | RTLD_LOCAL);
  if (isolated != nullptr)
    (void)dlclose(isolated);
  return versioned == nullptr;
}
]=]
    CXXLENS_TEST_HAS_GLIBC_DLMOPEN_DLVSYM)
  check_cxx_source_compiles(
    [=[
#include <fcntl.h>
#ifndef F_OFD_SETLK
#error "The source-SHM availability test requires OFD locks"
#endif
int main() {
  flock lock{};
  return fcntl(-1, F_OFD_SETLK, &lock);
}
]=]
    CXXLENS_TEST_HAS_OFD_LOCKS)
  cmake_pop_check_state()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CXXLENS_TEST_HAS_GLIBC_DLMOPEN_DLVSYM)
  # These helpers interpose the process loader for one dedicated subprocess
  # each. They are test-only DSOs: keep their symbols hidden by default, do not
  # instrument them ahead of the sanitizer runtime, and never install/export
  # them with a product target.
  add_library(cxxlens-test-sqlite-migration-external-interposer SHARED
              support/sqlite_migration_external_interposer.cpp)
  target_compile_features(cxxlens-test-sqlite-migration-external-interposer
                          PRIVATE cxx_std_23)
  target_link_libraries(cxxlens-test-sqlite-migration-external-interposer
                        PRIVATE "${CMAKE_DL_LIBS}")
  set_target_properties(
    cxxlens-test-sqlite-migration-external-interposer
    PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  cxxlens_set_project_warnings(
    cxxlens-test-sqlite-migration-external-interposer)

  add_library(cxxlens-test-store-sqlite-crash-interposer SHARED
              support/sqlite_store_crash_interposer.cpp)
  target_compile_features(cxxlens-test-store-sqlite-crash-interposer
                          PRIVATE cxx_std_23)
  target_link_libraries(cxxlens-test-store-sqlite-crash-interposer
                        PRIVATE "${CMAKE_DL_LIBS}")
  set_target_properties(
    cxxlens-test-store-sqlite-crash-interposer
    PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  cxxlens_set_project_warnings(cxxlens-test-store-sqlite-crash-interposer)

  add_executable(cxxlens-unit-sdk-sqlite-migration-outcomes
                 unit/sdk/sqlite_store_migration_external_interposer_test.cpp)
  target_compile_features(cxxlens-unit-sdk-sqlite-migration-outcomes
                          PRIVATE cxx_std_23)
  target_link_libraries(
    cxxlens-unit-sdk-sqlite-migration-outcomes
    PRIVATE cxxlens-sqlite-store-v3-scenario-support cxxlens::kernel
            "${CMAKE_DL_LIBS}" Threads::Threads)
  cxxlens_set_project_warnings(cxxlens-unit-sdk-sqlite-migration-outcomes)
  cxxlens_enable_sanitizers(cxxlens-unit-sdk-sqlite-migration-outcomes)
  add_dependencies(cxxlens-unit-sdk-sqlite-migration-outcomes
                   cxxlens-test-sqlite-migration-external-interposer)
  add_test(
    NAME unit.sdk-sqlite-migration-outcomes
    COMMAND cxxlens-unit-sdk-sqlite-migration-outcomes
            "$<TARGET_FILE:cxxlens-test-sqlite-migration-external-interposer>")
  set_tests_properties(
    unit.sdk-sqlite-migration-outcomes
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "LD_PRELOAD=unset:;CXXLENS_TEST_SQLITE_CRASH_PAUSE=unset:;CXXLENS_TEST_SQLITE_CRASH_READY_FD=unset:"
      LABELS
      "unit;sdk;store;sqlite;migration;effects;fault;recovery"
      RUN_SERIAL
      TRUE
      TIMEOUT
      60)

  add_executable(cxxlens-unit-sdk-store-sqlite-crash-restart
                 unit/sdk/store_sqlite_crash_recovery_test.cpp)
  target_compile_features(cxxlens-unit-sdk-store-sqlite-crash-restart
                          PRIVATE cxx_std_23)
  target_link_libraries(
    cxxlens-unit-sdk-store-sqlite-crash-restart
    PRIVATE cxxlens-sqlite-store-v3-scenario-support cxxlens::kernel
            "${CMAKE_DL_LIBS}" Threads::Threads)
  cxxlens_set_project_warnings(cxxlens-unit-sdk-store-sqlite-crash-restart)
  cxxlens_enable_sanitizers(cxxlens-unit-sdk-store-sqlite-crash-restart)
  add_dependencies(cxxlens-unit-sdk-store-sqlite-crash-restart
                   cxxlens-test-store-sqlite-crash-interposer)
  if(NOT CXXLENS_ENABLE_TSAN)
    add_test(
      NAME unit.sdk-store-sqlite-crash-restart
      COMMAND cxxlens-unit-sdk-store-sqlite-crash-restart
              "$<TARGET_FILE:cxxlens-test-store-sqlite-crash-interposer>")
    set_tests_properties(
      unit.sdk-store-sqlite-crash-restart
      PROPERTIES
        ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:;CXXLENS_TEST_SQLITE_CRASH_PAUSE=unset:;CXXLENS_TEST_SQLITE_CRASH_READY_FD=unset:"
        LABELS
        "unit;sdk;store;sqlite;normalization;crash;recovery;fault"
        RUN_SERIAL
        TRUE
        TIMEOUT
        30)
  endif()

  if(CXXLENS_TEST_HAS_OFD_LOCKS)
    add_library(cxxlens-test-sqlite-source-shm-missing-symbols-preload SHARED
                support/sqlite_source_shm_missing_symbols_preload.cpp)
    target_compile_features(
      cxxlens-test-sqlite-source-shm-missing-symbols-preload PRIVATE cxx_std_23)
    target_link_libraries(cxxlens-test-sqlite-source-shm-missing-symbols-preload
                          PRIVATE "${CMAKE_DL_LIBS}")
    set_target_properties(
      cxxlens-test-sqlite-source-shm-missing-symbols-preload
      PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    cxxlens_set_project_warnings(
      cxxlens-test-sqlite-source-shm-missing-symbols-preload)

    add_executable(
      cxxlens-unit-sdk-sqlite-source-availability
      unit/sdk/sqlite_source_shm_symbol_resolution_integration_test.cpp)
    target_compile_features(cxxlens-unit-sdk-sqlite-source-availability
                            PRIVATE cxx_std_23)
    target_link_libraries(
      cxxlens-unit-sdk-sqlite-source-availability
      PRIVATE cxxlens-sqlite-store-v3-scenario-support cxxlens::kernel
              "${CMAKE_DL_LIBS}" Threads::Threads)
    cxxlens_set_project_warnings(cxxlens-unit-sdk-sqlite-source-availability)
    cxxlens_enable_sanitizers(cxxlens-unit-sdk-sqlite-source-availability)
    add_dependencies(cxxlens-unit-sdk-sqlite-source-availability
                     cxxlens-test-sqlite-source-shm-missing-symbols-preload)
    add_test(
      NAME unit.sdk-sqlite-source-availability
      COMMAND
        cxxlens-unit-sdk-sqlite-source-availability
        "$<TARGET_FILE:cxxlens-test-sqlite-source-shm-missing-symbols-preload>")
    set_tests_properties(
      unit.sdk-sqlite-source-availability
      PROPERTIES
        ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:;CXXLENS_TEST_SQLITE_CRASH_PAUSE=unset:;CXXLENS_TEST_SQLITE_CRASH_READY_FD=unset:"
        LABELS
        "unit;sdk;store;sqlite;source-shm;availability;fault;security"
        RUN_SERIAL
        TRUE
        TIMEOUT
        30)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(
    cxxlens-unit-sdk-sqlite-202-crash-recovery
    unit/sdk/sqlite_202_crash_recovery_test.cpp
    support/sqlite202_crash_recovery_harness.cpp)
  target_compile_features(cxxlens-unit-sdk-sqlite-202-crash-recovery
                          PRIVATE cxx_std_23)
  target_link_libraries(cxxlens-unit-sdk-sqlite-202-crash-recovery
                        PRIVATE "${CMAKE_DL_LIBS}")
  target_include_directories(
    cxxlens-unit-sdk-sqlite-202-crash-recovery
    PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/support")
  cxxlens_set_project_warnings(cxxlens-unit-sdk-sqlite-202-crash-recovery)
  cxxlens_enable_sanitizers(cxxlens-unit-sdk-sqlite-202-crash-recovery)
  add_test(NAME unit.sdk-sqlite-202-crash-recovery
           COMMAND cxxlens-unit-sdk-sqlite-202-crash-recovery)
  set_tests_properties(
    unit.sdk-sqlite-202-crash-recovery
    PROPERTIES LABELS
               "unit;sdk;store;sqlite;normalization;crash;recovery;fault"
               TIMEOUT 60)
endif()

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-limit-length-precedence
                            unit/sdk/sqlite_limit_length_precedence_test.cpp)
target_link_libraries(
  cxxlens-unit-sdk-sqlite-limit-length-precedence
  PRIVATE cxxlens-sqlite-store-v3-scenario-support cxxlens::kernel
          "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-unit-sdk-sqlite-limit-length-precedence
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-limit-length-precedence
         COMMAND cxxlens-unit-sdk-sqlite-limit-length-precedence)
set_tests_properties(
  unit.sdk-sqlite-limit-length-precedence
  PROPERTIES LABELS "unit;sdk;store;sqlite;limit;precedence;boundary")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-store-wal-routing
                            unit/sdk/sqlite_store_wal_routing_test.cpp)
target_sources(
  cxxlens-unit-sdk-sqlite-store-wal-routing
  PRIVATE $<TARGET_OBJECTS:cxxlens-test-sqlite-store-fault-injection-support>)
target_link_libraries(cxxlens-unit-sdk-sqlite-store-wal-routing
                      PRIVATE cxxlens::kernel "${CMAKE_DL_LIBS}")
add_test(NAME unit.sdk-sqlite-store-wal-routing
         COMMAND cxxlens-unit-sdk-sqlite-store-wal-routing)
set_tests_properties(
  unit.sdk-sqlite-store-wal-routing
  PROPERTIES LABELS "unit;sdk;store;sqlite;wal;routing;recovery")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-payload-streaming
                            unit/sdk/sqlite_payload_streaming_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-payload-streaming
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-payload-streaming
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-payload-streaming
         COMMAND cxxlens-unit-sdk-sqlite-payload-streaming)
set_tests_properties(
  unit.sdk-sqlite-payload-streaming
  PROPERTIES LABELS "unit;sdk;store;sqlite;streaming;bounded")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-wal-receipt
                            unit/sdk/sqlite_wal_receipt_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-wal-receipt
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-wal-receipt
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-wal-receipt
         COMMAND cxxlens-unit-sdk-sqlite-wal-receipt)
set_tests_properties(
  unit.sdk-sqlite-wal-receipt
  PROPERTIES LABELS "unit;sdk;store;sqlite;wal;recovery;bounded")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-wal-source-capture
                            unit/sdk/sqlite_wal_source_capture_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-wal-source-capture
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-wal-source-capture
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-wal-source-capture
         COMMAND cxxlens-unit-sdk-sqlite-wal-source-capture)
set_tests_properties(
  unit.sdk-sqlite-wal-source-capture
  PROPERTIES LABELS "unit;sdk;store;sqlite;wal;recovery;capture;bounded")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-wal-recovery-workspace
                            unit/sdk/sqlite_wal_recovery_workspace_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-wal-recovery-workspace
                      PRIVATE cxxlens::kernel "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-unit-sdk-sqlite-wal-recovery-workspace
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-wal-recovery-workspace
         COMMAND cxxlens-unit-sdk-sqlite-wal-recovery-workspace)
set_tests_properties(
  unit.sdk-sqlite-wal-recovery-workspace
  PROPERTIES LABELS "unit;sdk;store;sqlite;wal;recovery;workspace;bounded")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-default-forwarding-vfs
                            unit/sdk/sqlite_default_forwarding_vfs_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-default-forwarding-vfs
                      PRIVATE cxxlens::kernel "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-unit-sdk-sqlite-default-forwarding-vfs
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-default-forwarding-vfs
         COMMAND cxxlens-unit-sdk-sqlite-default-forwarding-vfs)
set_tests_properties(
  unit.sdk-sqlite-default-forwarding-vfs
  PROPERTIES LABELS "unit;sdk;store;sqlite;vfs;effects;security")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-source-shm-readonly-preflight
  unit/sdk/sqlite_source_shm_readonly_preflight_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-source-shm-readonly-preflight
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-source-shm-readonly-preflight
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-source-shm-readonly-preflight
         COMMAND cxxlens-unit-sdk-sqlite-source-shm-readonly-preflight)
set_tests_properties(
  unit.sdk-sqlite-source-shm-readonly-preflight
  PROPERTIES LABELS "unit;sdk;store;sqlite;vfs;shm;preflight;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-connection-lifecycle
                            unit/sdk/sqlite_connection_lifecycle_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-connection-lifecycle
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-connection-lifecycle
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-connection-lifecycle
         COMMAND cxxlens-unit-sdk-sqlite-connection-lifecycle)
set_tests_properties(
  unit.sdk-sqlite-connection-lifecycle
  PROPERTIES LABELS "unit;sdk;store;sqlite;lifecycle;quarantine")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-mapping-lease
  unit/sdk/sqlite_same_process_shm_mapping_lease_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-mapping-lease
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-mapping-lease
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-mapping-lease
         COMMAND cxxlens-unit-sdk-sqlite-shm-mapping-lease)
set_tests_properties(
  unit.sdk-sqlite-shm-mapping-lease
  PROPERTIES LABELS "unit;sdk;store;sqlite;vfs;shm;lease;lifecycle;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-writer-gate-outcome
                            unit/sdk/sqlite_writer_gate_outcome_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-writer-gate-outcome
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-writer-gate-outcome
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-writer-gate-outcome
         COMMAND cxxlens-unit-sdk-sqlite-writer-gate-outcome)
set_tests_properties(
  unit.sdk-sqlite-writer-gate-outcome
  PROPERTIES LABELS "unit;sdk;store;sqlite;vfs;shm;gate;lifecycle;security")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-process-port
  unit/sdk/sqlite_same_process_shm_process_port_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-process-port
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-process-port
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-process-port
         COMMAND cxxlens-unit-sdk-sqlite-shm-process-port)
set_tests_properties(
  unit.sdk-sqlite-shm-process-port
  PROPERTIES LABELS
             "unit;sdk;store;sqlite;vfs;shm;registry;process;fork;security")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-vfs-alias-registration
  unit/sdk/sqlite_same_process_shm_vfs_alias_registration_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-vfs-alias-registration
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-vfs-alias-registration
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-vfs-alias-registration
         COMMAND cxxlens-unit-sdk-sqlite-shm-vfs-alias-registration)
set_tests_properties(
  unit.sdk-sqlite-shm-vfs-alias-registration
  PROPERTIES LABELS
             "unit;sdk;store;sqlite;vfs;shm;registry;registration;fork;security"
)

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-vfs-alias-identity-sealer
  unit/sdk/sqlite_same_process_shm_vfs_alias_identity_sealer_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-vfs-alias-identity-sealer
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-vfs-alias-identity-sealer
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-vfs-alias-identity-sealer
         COMMAND cxxlens-unit-sdk-sqlite-shm-vfs-alias-identity-sealer)
set_tests_properties(
  unit.sdk-sqlite-shm-vfs-alias-identity-sealer
  PROPERTIES LABELS "unit;sdk;store;sqlite;vfs;shm;registry;identity;security")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-mapping-registry
  unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-mapping-registry
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-mapping-registry
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-mapping-registry
         COMMAND cxxlens-unit-sdk-sqlite-shm-mapping-registry)
set_tests_properties(
  unit.sdk-sqlite-shm-mapping-registry
  PROPERTIES LABELS
             "unit;sdk;store;sqlite;vfs;shm;registry;lifecycle;fork;security")
add_test(NAME unit.sdk-sqlite-shm-mapping-registry-qualified-mapped-acceptance
         COMMAND cxxlens-unit-sdk-sqlite-shm-mapping-registry)
set_tests_properties(
  unit.sdk-sqlite-shm-mapping-registry-qualified-mapped-acceptance
  PROPERTIES
    ENVIRONMENT
    "CXXLENS_SQLITE_MAPPED_ACCEPTANCE_ONLY=1"
    LABELS
    "unit;sdk;store;sqlite;vfs;shm;registry;lifecycle;fork;security;mapped;acceptance"
)

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-shm-identity-issuer
  unit/sdk/sqlite_same_process_shm_identity_issuer_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-shm-identity-issuer
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-shm-identity-issuer
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-shm-identity-issuer
         COMMAND cxxlens-unit-sdk-sqlite-shm-identity-issuer)
set_tests_properties(
  unit.sdk-sqlite-shm-identity-issuer
  PROPERTIES
    LABELS
    "unit;sdk;store;sqlite;vfs;shm;identity;issuer;lifecycle;fork;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-writer-shm-mapping-epoch
                            unit/sdk/sqlite_writer_shm_mapping_epoch_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-writer-shm-mapping-epoch
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-writer-shm-mapping-epoch
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-writer-shm-mapping-epoch
         COMMAND cxxlens-unit-sdk-sqlite-writer-shm-mapping-epoch)
set_tests_properties(
  unit.sdk-sqlite-writer-shm-mapping-epoch
  PROPERTIES
    LABELS
    "unit;sdk;store;sqlite;vfs;shm;writer;mapping;epoch;effects;lifecycle;security"
)

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-writer-shm-mapping-semantics
  unit/sdk/sqlite_writer_shm_mapping_semantics_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-writer-shm-mapping-semantics
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-writer-shm-mapping-semantics
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-writer-shm-mapping-semantics
         COMMAND cxxlens-unit-sdk-sqlite-writer-shm-mapping-semantics)
set_tests_properties(
  unit.sdk-sqlite-writer-shm-mapping-semantics
  PROPERTIES
    LABELS
    "unit;sdk;store;sqlite;vfs;shm;writer;mapping;semantics;effects;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-store-terminal
                            unit/sdk/sqlite_store_terminal_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-store-terminal
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-store-terminal
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-store-terminal
         COMMAND cxxlens-unit-sdk-sqlite-store-terminal)
set_tests_properties(
  unit.sdk-sqlite-store-terminal
  PROPERTIES LABELS "unit;sdk;store;sqlite;terminal;poison;reclassification")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-terminal-reclassifier
                            unit/sdk/sqlite_terminal_reclassifier_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-terminal-reclassifier
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-terminal-reclassifier
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-terminal-reclassifier
         COMMAND cxxlens-unit-sdk-sqlite-terminal-reclassifier)
set_tests_properties(
  unit.sdk-sqlite-terminal-reclassifier
  PROPERTIES LABELS
             "unit;sdk;store;sqlite;terminal;reclassification;descendant")

cxxlens_add_test_executable(
  cxxlens-unit-sdk-sqlite-exact-empty-cold-reclassifier
  unit/sdk/sqlite_exact_empty_cold_reclassifier_test.cpp)
target_link_libraries(cxxlens-unit-sdk-sqlite-exact-empty-cold-reclassifier
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-exact-empty-cold-reclassifier
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-exact-empty-cold-reclassifier
         COMMAND cxxlens-unit-sdk-sqlite-exact-empty-cold-reclassifier)
set_tests_properties(
  unit.sdk-sqlite-exact-empty-cold-reclassifier
  PROPERTIES LABELS
             "unit;sdk;store;sqlite;normalization;classifier;cold;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-store-fault-injection
                            unit/sdk/sqlite_store_fault_injection_test.cpp)
target_sources(
  cxxlens-unit-sdk-sqlite-store-fault-injection
  PRIVATE $<TARGET_OBJECTS:cxxlens-test-sqlite-store-fault-injection-support>)
target_link_libraries(cxxlens-unit-sdk-sqlite-store-fault-injection
                      PRIVATE cxxlens::kernel)
target_include_directories(cxxlens-unit-sdk-sqlite-store-fault-injection
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-sqlite-store-fault-injection
         COMMAND cxxlens-unit-sdk-sqlite-store-fault-injection)
set_tests_properties(
  unit.sdk-sqlite-store-fault-injection
  PROPERTIES LABELS "unit;sdk;store;sqlite;fault-injection;terminal")

cxxlens_add_test_executable(cxxlens-unit-sdk-query-runtime
                            unit/sdk/query_runtime_test.cpp)
target_link_libraries(cxxlens-unit-sdk-query-runtime PRIVATE cxxlens::query)
add_test(NAME unit.sdk-query-runtime COMMAND cxxlens-unit-sdk-query-runtime)
set_tests_properties(
  unit.sdk-query-runtime PROPERTIES LABELS
                                    "unit;sdk;query;store;budget;cancellation")

cxxlens_add_test_executable(cxxlens-g5-runtime unit/sdk/g5_runtime_test.cpp)
target_link_libraries(cxxlens-g5-runtime PRIVATE cxxlens::provider_sdk)
add_test(NAME acceptance.g5-runtime COMMAND cxxlens-g5-runtime)
set_tests_properties(
  acceptance.g5-runtime
  PROPERTIES LABELS
             "unit;sdk;g5;closure;incremental;warm-zero;determinism;acceptance")

cxxlens_add_test_executable(cxxlens-provider-process-fixture
                            fixtures/provider_process_fixture.cpp)
target_link_libraries(cxxlens-provider-process-fixture
                      PRIVATE cxxlens::provider_sdk)

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-runtime
                            unit/sdk/provider_runtime_test.cpp)
target_sources(cxxlens-unit-sdk-provider-runtime
               PRIVATE $<TARGET_OBJECTS:cxxlens-test-sdk-support>)
if(NOT CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-runtime
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
endif()
target_link_libraries(cxxlens-unit-sdk-provider-runtime
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-runtime
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-runtime
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-unit-sdk-provider-runtime
                        PRIVATE cxxlens_protocol_v2)
endif()
add_test(NAME unit.sdk-provider-runtime
         COMMAND cxxlens-unit-sdk-provider-runtime
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")
check_cxx_source_compiles(
  [=[
#include <features.h>
#ifndef __GLIBC__
#error "The provider process adapter requires glibc"
#endif
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef SYS_pidfd_open
#error "The timeout regression requires pidfd_open"
#endif
#ifndef SYS_pidfd_send_signal
#error "The timeout regression requires pidfd_send_signal"
#endif
int main() {
  siginfo_t status{};
  return waitid(P_PIDFD, 0, &status, WEXITED | WNOHANG);
}
]=]
  CXXLENS_TEST_HAS_LINUX_GLIBC_PIDFD_WAITID)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CXXLENS_TEST_HAS_LINUX_GLIBC_PIDFD_WAITID)
  add_test(NAME unit.sdk-provider-runtime-timeout
           COMMAND cxxlens-unit-sdk-provider-runtime --timeout-regression
                   "$<TARGET_FILE:cxxlens-provider-process-fixture>")
  add_test(NAME unit.sdk-provider-runtime-timeout-contention
           COMMAND cxxlens-unit-sdk-provider-runtime --timeout-regression
                   "$<TARGET_FILE:cxxlens-provider-process-fixture>")
endif()
add_test(NAME unit.sdk-provider-runtime-sealed
         COMMAND cxxlens-unit-sdk-provider-runtime --sealed-only)
add_test(NAME unit.sdk-provider-runtime-host-streaming
         COMMAND cxxlens-unit-sdk-provider-runtime --host-only
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")
add_test(NAME unit.sdk-provider-runtime-receipt
         COMMAND cxxlens-unit-sdk-provider-runtime --receipt-only
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  cxxlens_add_test_executable(cxxlens-unit-sdk-provider-process-channel
                              unit/sdk/provider_process_channel_test.cpp)
  target_link_libraries(cxxlens-unit-sdk-provider-process-channel
                        PRIVATE cxxlens::provider_sdk)
  target_include_directories(cxxlens-unit-sdk-provider-process-channel
                             PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(CXXLENS_BUILD_SHARED)
    target_sources(
      cxxlens-unit-sdk-provider-process-channel
      PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
              "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
    target_link_libraries(cxxlens-unit-sdk-provider-process-channel
                          PRIVATE cxxlens_protocol_v2)
  endif()
  add_test(NAME unit.sdk-provider-process-channel
           COMMAND cxxlens-unit-sdk-provider-process-channel)
  set_tests_properties(
    unit.sdk-provider-process-channel
    PROPERTIES LABELS
               "unit;sdk;provider;process;source-closure;security;cancellation")
endif()
set_tests_properties(
  unit.sdk-provider-runtime
  PROPERTIES
    LABELS
    "unit;sdk;provider;process;sandbox;security;budget;cancellation;slow"
    PROCESSORS 2)

cxxlens_add_test_executable(
  cxxlens-unit-sdk-provider-expected-output-validator
  unit/sdk/provider_expected_output_validator_test.cpp)
target_link_libraries(
  cxxlens-unit-sdk-provider-expected-output-validator
  PRIVATE cxxlens_provider_expected_output_validator_internal)
target_include_directories(cxxlens-unit-sdk-provider-expected-output-validator
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-provider-expected-output-validator
         COMMAND cxxlens-unit-sdk-provider-expected-output-validator)
set_tests_properties(
  unit.sdk-provider-expected-output-validator
  PROPERTIES
    LABELS
    "unit;sdk;provider;protocol;expected-output;validation;security;bounded")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CXXLENS_TEST_HAS_LINUX_GLIBC_PIDFD_WAITID)
  # The regression exercises several fork/watcher/cleanup sequences whose own
  # internal budgets already sum to several seconds in the best case (see
  # provider_runtime_test.cpp); ASan/UBSan's per-process-spawn instrumentation
  # overhead leaves too little headroom against a flat 30s CTest wall timeout
  # This widens the outer CTest kill timeout for instrumented builds. TSan uses
  # the same sanitizer-only fixture budget as ASan/UBSan, so it must receive the
  # same outer headroom or CTest can kill the test before its internal elapsed
  # bound. The test also derives a sanitizer-only CPU/wall budget for the forked
  # fixture's cold startup; neither allowance changes production provider
  # runtime semantics.
  if(CXXLENS_ENABLE_ASAN
     OR CXXLENS_ENABLE_UBSAN
     OR CXXLENS_ENABLE_TSAN)
    set(CXXLENS_PROVIDER_RUNTIME_TIMEOUT_REGRESSION_TIMEOUT 180)
  else()
    set(CXXLENS_PROVIDER_RUNTIME_TIMEOUT_REGRESSION_TIMEOUT 30)
  endif()
  set_tests_properties(
    unit.sdk-provider-runtime-timeout
    PROPERTIES LABELS
               "unit;sdk;provider;process;budget;cancellation;regression"
               # The fixture asserts a five-second wall-clock readiness bound.
               # Run it without unrelated CTest processes competing for that
               # scheduler interval; its explicit contention variant still
               # exercises the product under CPU pressure.
               RUN_SERIAL
               TRUE
               TIMEOUT
               ${CXXLENS_PROVIDER_RUNTIME_TIMEOUT_REGRESSION_TIMEOUT})
  set_tests_properties(
    unit.sdk-provider-runtime-timeout-contention
    PROPERTIES
      LABELS
      "unit;sdk;provider;process;budget;cancellation;regression;contention"
      ENVIRONMENT
      "CXXLENS_PROVIDER_TIMEOUT_CONTENTION=1"
      RUN_SERIAL
      TRUE
      TIMEOUT
      ${CXXLENS_PROVIDER_RUNTIME_TIMEOUT_REGRESSION_TIMEOUT})
endif()
set_tests_properties(unit.sdk-provider-runtime-sealed
                     PROPERTIES LABELS "unit;sdk;provider;sealed;adoption")
set_tests_properties(
  unit.sdk-provider-runtime-host-streaming unit.sdk-provider-runtime-receipt
  PROPERTIES LABELS "unit;sdk;provider;protocol;streaming;sealed;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-validation
                            unit/sdk/provider_ng1_hardening_test.cpp)
target_sources(
  cxxlens-unit-sdk-provider-ng1-validation
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
target_link_libraries(cxxlens-unit-sdk-provider-ng1-validation
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-validation
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-provider-ng1-validation
         COMMAND cxxlens-unit-sdk-provider-ng1-validation)
set_tests_properties(
  unit.sdk-provider-ng1-validation
  PROPERTIES LABELS "unit;sdk;provider;ng1;validation;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-spill-port
                            unit/sdk/provider_ng1_spill_port_test.cpp)
target_sources(
  cxxlens-unit-sdk-provider-ng1-spill-port
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_spill_port_internal.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
target_link_libraries(cxxlens-unit-sdk-provider-ng1-spill-port
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-spill-port
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-provider-ng1-spill-port
         COMMAND cxxlens-unit-sdk-provider-ng1-spill-port)
set_tests_properties(
  unit.sdk-provider-ng1-spill-port
  PROPERTIES LABELS "unit;sdk;provider;ng1;spill;fsync;recovery;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-transport
                            unit/sdk/provider_ng1_transport_test.cpp)
target_sources(
  cxxlens-unit-sdk-provider-ng1-transport
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
target_link_libraries(cxxlens-unit-sdk-provider-ng1-transport
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-transport
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-provider-ng1-transport
         COMMAND cxxlens-unit-sdk-provider-ng1-transport)
set_tests_properties(
  unit.sdk-provider-ng1-transport
  PROPERTIES LABELS
             "unit;sdk;provider;ng1;transport;deterministic-cbor;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-recovery-adapter
                            unit/sdk/provider_ng1_recovery_adapter_test.cpp)
target_sources(
  cxxlens-unit-sdk-provider-ng1-recovery-adapter
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_recovery.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
target_link_libraries(cxxlens-unit-sdk-provider-ng1-recovery-adapter
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-recovery-adapter
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-provider-ng1-recovery-adapter
         COMMAND cxxlens-unit-sdk-provider-ng1-recovery-adapter)
set_tests_properties(
  unit.sdk-provider-ng1-recovery-adapter
  PROPERTIES LABELS "unit;sdk;provider;ng1;recovery;resume;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-session
                            unit/sdk/provider_ng1_session_test.cpp)
if(NOT CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-session
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_session.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_recovery.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_spill_port_internal.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
endif()
target_link_libraries(cxxlens-unit-sdk-provider-ng1-session
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-session
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-session
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-unit-sdk-provider-ng1-session
                        PRIVATE cxxlens_protocol_v2)
endif()
add_test(NAME unit.sdk-provider-ng1-session
         COMMAND cxxlens-unit-sdk-provider-ng1-session)
set_tests_properties(
  unit.sdk-provider-ng1-session
  PROPERTIES LABELS "unit;sdk;provider;ng1;session;recovery;spill;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-live-driver
                            unit/sdk/provider_ng1_live_driver_test.cpp)
if(NOT CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-live-driver
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_live_driver.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_session.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_recovery.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_spill_port_internal.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
endif()
target_link_libraries(cxxlens-unit-sdk-provider-ng1-live-driver
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-live-driver
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-live-driver
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-unit-sdk-provider-ng1-live-driver
                        PRIVATE cxxlens_protocol_v2)
endif()
add_test(NAME unit.sdk-provider-ng1-live-driver
         COMMAND cxxlens-unit-sdk-provider-ng1-live-driver)
set_tests_properties(
  unit.sdk-provider-ng1-live-driver
  PROPERTIES LABELS "unit;sdk;provider;ng1;live;transport;session;fail-closed")

cxxlens_add_test_executable(cxxlens-clang22-provider-worker-protocol
                            adapter/clang22/provider_worker_protocol_test.cpp)
target_link_libraries(cxxlens-clang22-provider-worker-protocol
                      PRIVATE cxxlens::provider_sdk)
add_dependencies(cxxlens-clang22-provider-worker-protocol
                 cxxlens-clang-worker-22)
add_test(NAME adapter.clang22-provider-worker-protocol
         COMMAND cxxlens-clang22-provider-worker-protocol
                 "$<TARGET_FILE:cxxlens-clang-worker-22>")
set_tests_properties(
  adapter.clang22-provider-worker-protocol
  PROPERTIES LABELS
             "adapter;provider;clang22;process;protocol;sandbox;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-json
                            adapter/clang22/materialization_json_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-json
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-materialization-json
         COMMAND cxxlens-clang22-materialization-json)
set_tests_properties(
  adapter.clang22-materialization-json
  PROPERTIES LABELS "adapter;provider;clang22;materialization;json;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-identity
                            adapter/clang22/materialization_identity_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-identity
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-materialization-identity
         COMMAND cxxlens-clang22-materialization-identity)
set_tests_properties(
  adapter.clang22-materialization-identity
  PROPERTIES LABELS "adapter;provider;clang22;materialization;identity")

add_test(
  NAME adapter.clang22-materialization-cli-phase
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/adapter/clang22/materialization_cli_phase_test.py"
    --driver "$<TARGET_FILE:cxxlens-clang22-materialize>")
set_tests_properties(
  adapter.clang22-materialization-cli-phase
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;request;failure;phase")

cxxlens_add_test_executable(cxxlens-clang22-materialization-io
                            adapter/clang22/materialization_io_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-io
                      PRIVATE cxxlens_clang22_materialization_io_support)
add_test(NAME adapter.clang22-materialization-io
         COMMAND cxxlens-clang22-materialization-io)
set_tests_properties(
  adapter.clang22-materialization-io
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;streaming;security")

cxxlens_add_test_executable(cxxlens-df0200-claim-batch-corpus
                            adapter/clang22/df_0200_claim_batch_corpus_test.cpp)
target_link_libraries(cxxlens-df0200-claim-batch-corpus
                      PRIVATE cxxlens::clang22_provider_sdk)
add_test(
  NAME qualification.df0200-claim-batch-corpus
  COMMAND
    cxxlens-df0200-claim-batch-corpus
    "${PROJECT_SOURCE_DIR}/schemas/cxxlens_ng_df_0200_claim_batch_differential_corpus.tsv"
)
set_tests_properties(
  qualification.df0200-claim-batch-corpus
  PROPERTIES LABELS "qualification;sdk;claim;differential;df0200")

cxxlens_add_test_executable(cxxlens-clang22-materialization-store
                            adapter/clang22/materialization_store_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-store
  PRIVATE cxxlens_clang22_materializer_safety_support "${CMAKE_DL_LIBS}")
add_test(NAME adapter.clang22-materialization-store
         COMMAND cxxlens-clang22-materialization-store)
set_tests_properties(
  adapter.clang22-materialization-store
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;transaction;sqlite"
)

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-candidate
  adapter/clang22/materialization_store_candidate_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-store-candidate
                      PRIVATE cxxlens_clang22_materializer_bounded_support)
add_test(NAME adapter.clang22-materialization-store-candidate
         COMMAND cxxlens-clang22-materialization-store-candidate)
set_tests_properties(
  adapter.clang22-materialization-store-candidate
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;store;candidate;bounded;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-projection
  adapter/clang22/materialization_store_projection_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-store-projection
                      PRIVATE cxxlens_clang22_materializer_bounded_support)
add_test(NAME adapter.clang22-materialization-store-projection
         COMMAND cxxlens-clang22-materialization-store-projection)
set_tests_properties(
  adapter.clang22-materialization-store-projection
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;store;projection;bounded;security"
)

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-candidate-bridge
  adapter/clang22/materialization_store_candidate_bridge_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-store-candidate-bridge
                      PRIVATE cxxlens_clang22_materializer_bounded_support)
add_test(NAME adapter.clang22-materialization-store-candidate-bridge
         COMMAND cxxlens-clang22-materialization-store-candidate-bridge)
set_tests_properties(
  adapter.clang22-materialization-store-candidate-bridge
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;store;candidate;bridge;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-outcome
  adapter/clang22/materialization_store_outcome_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-store-outcome
  PRIVATE cxxlens_clang22_materializer_store_outcome_support
          cxxlens_clang22_materializer_v4_reuse_support)
add_test(NAME adapter.clang22-materialization-store-outcome
         COMMAND cxxlens-clang22-materialization-store-outcome)
set_tests_properties(
  adapter.clang22-materialization-store-outcome
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;outcome;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-request-v2-2
  adapter/clang22/materialization_request_v2_2_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-request-v2-2
  PRIVATE cxxlens_clang22_materialization_request_v2_2_support)
add_test(NAME adapter.clang22-materialization-request-v2-2
         COMMAND cxxlens-clang22-materialization-request-v2-2)
set_tests_properties(
  adapter.clang22-materialization-request-v2-2
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;request;v2.2;streaming;security")

cxxlens_add_test_executable(cxxlens-clang22-provider-task-v4
                            adapter/clang22/provider_task_v4_test.cpp)
target_link_libraries(cxxlens-clang22-provider-task-v4
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-provider-task-v4
         COMMAND cxxlens-clang22-provider-task-v4)
set_tests_properties(
  adapter.clang22-provider-task-v4
  PROPERTIES LABELS "adapter;provider;clang22;materialization;task;v4;security")

cxxlens_add_test_executable(cxxlens-clang22-provider-task-v4-authority
                            adapter/clang22/provider_task_v4_authority_test.cpp)
target_link_libraries(
  cxxlens-clang22-provider-task-v4-authority
  PRIVATE cxxlens_clang22_provider_task_v4_authority_internal)
add_test(NAME adapter.clang22-provider-task-v4-authority
         COMMAND cxxlens-clang22-provider-task-v4-authority)
set_tests_properties(
  adapter.clang22-provider-task-v4-authority
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;task;v4;authority;security;bounded;downgrade;replay"
)

cxxlens_add_test_executable(
  cxxlens-clang22-provider-worker-ingress-decoder
  adapter/clang22/provider_worker_ingress_decoder_test.cpp)
target_link_libraries(cxxlens-clang22-provider-worker-ingress-decoder
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-provider-worker-ingress-decoder
         COMMAND cxxlens-clang22-provider-worker-ingress-decoder)
set_tests_properties(
  adapter.clang22-provider-worker-ingress-decoder
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;task;v4;ingress;security;bounded")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-v4-claim-binding
  adapter/clang22/materialization_v4_claim_binding_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-v4-claim-binding
  PRIVATE cxxlens_clang22_materializer_v4_claims
          cxxlens_clang22_materializer_v4_ingress)
add_test(NAME adapter.clang22-materialization-v4-claim-binding
         COMMAND cxxlens-clang22-materialization-v4-claim-binding)
set_tests_properties(
  adapter.clang22-materialization-v4-claim-binding
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;claim;task;v4;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-v4-store-positive
  adapter/clang22/materialization_v4_store_positive_min_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-v4-store-positive
  PRIVATE cxxlens_clang22_materializer_v4_claims
          cxxlens_clang22_materializer_v4_ingress
          cxxlens_clang22_materializer_v4_store_source)
add_test(NAME adapter.clang22-materialization-v4-store-positive
         COMMAND cxxlens-clang22-materialization-v4-store-positive)
set_tests_properties(
  adapter.clang22-materialization-v4-store-positive
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;claim;task;v4;store;positive;security"
)

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-v6-typed-ingress
  adapter/clang22/materialization_store_v6_typed_ingress_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-store-v6-typed-ingress
  PRIVATE cxxlens_clang22_materializer_store_v6_typed_ingress)
target_include_directories(
  cxxlens-clang22-materialization-store-v6-typed-ingress
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME adapter.clang22-materialization-store-v6-typed-ingress
         COMMAND cxxlens-clang22-materialization-store-v6-typed-ingress)
set_tests_properties(
  adapter.clang22-materialization-store-v6-typed-ingress
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;v6;typed;ingress;bounded;security")

cxxlens_add_test_executable(cxxlens-protocol-v2
                            protocol_v2/protocol_v2_direct_test.cpp)
target_link_libraries(cxxlens-protocol-v2 PRIVATE cxxlens_protocol_v2)
add_test(NAME sdk.protocol-v2 COMMAND cxxlens-protocol-v2)
set_tests_properties(sdk.protocol-v2 PROPERTIES LABELS
                                                "sdk;protocol;v2;security")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-protocol-v2-adapter
                            unit/sdk/provider_protocol_v2_adapter_test.cpp)
target_link_libraries(cxxlens-unit-sdk-provider-protocol-v2-adapter
                      PRIVATE cxxlens::provider_sdk)
if(CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-protocol-v2-adapter
    PRIVATE
      $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
      "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp"
      "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_heartbeat_adapter.cpp"
  )
  target_link_libraries(cxxlens-unit-sdk-provider-protocol-v2-adapter
                        PRIVATE cxxlens_protocol_v2)
endif()
add_test(NAME unit.sdk-provider-protocol-v2-adapter
         COMMAND cxxlens-unit-sdk-provider-protocol-v2-adapter)
set_tests_properties(
  unit.sdk-provider-protocol-v2-adapter
  PROPERTIES LABELS "unit;sdk;protocol;v2;source-closure;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-pipeline-v2-2
  adapter/clang22/materialization_pipeline_v2_2_test.cpp)
target_link_libraries(
  cxxlens-clang22-materialization-pipeline-v2-2
  PRIVATE cxxlens_clang22_materializer_v2_2_pipeline_support)
add_test(NAME adapter.clang22-materialization-pipeline-v2-2
         COMMAND cxxlens-clang22-materialization-pipeline-v2-2)
set_tests_properties(
  adapter.clang22-materialization-pipeline-v2-2
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;pipeline;v2.2;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-rooted-vfs
                            adapter/clang22/materialization_rooted_vfs_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-rooted-vfs
                      PRIVATE cxxlens_clang22_materializer_rooted_vfs_support)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(cxxlens-clang22-materialization-rooted-vfs
                             PRIVATE CXXLENS_TEST_WRAP_ROOTED_VFS_EFFECTS=1)
  target_link_options(cxxlens-clang22-materialization-rooted-vfs PRIVATE
                      "LINKER:--wrap=unlinkat")
endif()
add_test(NAME adapter.clang22-materialization-rooted-vfs
         COMMAND cxxlens-clang22-materialization-rooted-vfs)
set_tests_properties(
  adapter.clang22-materialization-rooted-vfs
  PROPERTIES
    TIMEOUT 60 LABELS
    "adapter;provider;clang22;materialization;store;sqlite;rooted-vfs;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-occurrence
                            adapter/clang22/materialization_occurrence_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-occurrence
                      PRIVATE cxxlens_clang22_materializer_occurrence_support)
add_test(NAME adapter.clang22-materialization-occurrence
         COMMAND cxxlens-clang22-materialization-occurrence)
set_tests_properties(
  adapter.clang22-materialization-occurrence
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;occurrence;security")

cxxlens_add_test_executable(cxxlens-clang22-observation-v2
                            adapter/clang22/observation_v2_test.cpp)
target_link_libraries(cxxlens-clang22-observation-v2
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-observation-v2
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME adapter.clang22-observation-v2
         COMMAND cxxlens-clang22-observation-v2)
set_tests_properties(
  adapter.clang22-observation-v2
  PROPERTIES LABELS "adapter;provider;clang22;materialization;observation")

cxxlens_add_test_executable(cxxlens-clang22-unicode-nfc
                            adapter/clang22/unicode_nfc_test.cpp)
target_link_libraries(cxxlens-clang22-unicode-nfc
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-unicode-nfc
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME adapter.clang22-unicode-nfc COMMAND cxxlens-clang22-unicode-nfc)
set_tests_properties(
  adapter.clang22-unicode-nfc
  PROPERTIES LABELS "adapter;provider;clang22;materialization;unicode;security")

cxxlens_add_test_executable(cxxlens-clang22-source-closure
                            adapter/clang22/source_closure_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure
         COMMAND cxxlens-clang22-source-closure)
set_tests_properties(
  adapter.clang22-source-closure
  PROPERTIES
    LABELS "adapter;provider;clang22;source-closure;security;identity;contract")

cxxlens_add_test_executable(cxxlens-clang22-source-closure-vfs
                            adapter/clang22/source_closure_vfs_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-vfs
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-vfs
         COMMAND cxxlens-clang22-source-closure-vfs)
set_tests_properties(
  adapter.clang22-source-closure-vfs
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;vfs;security;contract")

cxxlens_add_test_executable(cxxlens-clang22-source-closure-invocation
                            adapter/clang22/source_closure_invocation_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-invocation
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-invocation
         COMMAND cxxlens-clang22-source-closure-invocation)
set_tests_properties(
  adapter.clang22-source-closure-invocation
  PROPERTIES
    LABELS
    "adapter;provider;clang22;source-closure;invocation;security;contract")

cxxlens_add_test_executable(cxxlens-clang22-source-closure-transport
                            adapter/clang22/source_closure_transport_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-transport
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-transport
         COMMAND cxxlens-clang22-source-closure-transport)
set_tests_properties(
  adapter.clang22-source-closure-transport
  PROPERTIES
    LABELS
    "adapter;provider;clang22;source-closure;transport;protocol;security")

cxxlens_add_test_executable(cxxlens-clang22-source-closure-spool
                            adapter/clang22/source_closure_spool_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-spool
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-spool
         COMMAND cxxlens-clang22-source-closure-spool)
set_tests_properties(
  adapter.clang22-source-closure-spool
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;spool;bounded;security")

cxxlens_add_test_executable(cxxlens-clang22-source-closure-receiver
                            adapter/clang22/source_closure_receiver_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-receiver
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-receiver
         COMMAND cxxlens-clang22-source-closure-receiver)
set_tests_properties(
  adapter.clang22-source-closure-receiver
  PROPERTIES
    LABELS
    "adapter;provider;clang22;source-closure;receiver;protocol;security;bounded"
)

if(UNIX)
  cxxlens_add_test_executable(cxxlens-clang22-source-closure-fd
                              adapter/clang22/source_closure_fd_test.cpp)
  target_link_libraries(cxxlens-clang22-source-closure-fd
                        PRIVATE cxxlens_clang22_materialization_codecs)
  add_test(NAME adapter.clang22-source-closure-fd
           COMMAND cxxlens-clang22-source-closure-fd)
  set_tests_properties(
    adapter.clang22-source-closure-fd
    PROPERTIES
      LABELS
      "adapter;provider;clang22;source-closure;fd;transport;security;bounded")

  cxxlens_add_test_executable(
    cxxlens-clang22-installed-materializer-source-closure
    adapter/clang22/installed_materializer_source_closure_test.cpp)
  target_link_libraries(
    cxxlens-clang22-installed-materializer-source-closure
    PRIVATE cxxlens_clang22_materializer_ingress
            cxxlens_clang22_materialization_codecs)
  add_test(NAME adapter.clang22-installed-materializer-source-closure
           COMMAND cxxlens-clang22-installed-materializer-source-closure)
  set_tests_properties(
    adapter.clang22-installed-materializer-source-closure
    PROPERTIES
      LABELS
      "adapter;provider;clang22;source-closure;fd;materializer;security;bounded"
  )
endif()

cxxlens_add_test_executable(cxxlens-clang22-source-closure-task-v4
                            adapter/clang22/source_closure_task_v4_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-task-v4
                      PRIVATE cxxlens_clang22_materialization_codecs)
add_test(NAME adapter.clang22-source-closure-task-v4
         COMMAND cxxlens-clang22-source-closure-task-v4)
set_tests_properties(
  adapter.clang22-source-closure-task-v4
  PROPERTIES LABELS "adapter;provider;clang22;source-closure;task;v4;security")

cxxlens_add_test_executable(
  cxxlens-clang22-source-closure-task-v4-worker
  adapter/clang22/source_closure_task_v4_worker_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-task-v4-worker
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-source-closure-task-v4-worker
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  # The positive worker path must invoke the exact packaged compiler selected by
  # the adapter, with the same admitted root and resource directory as the
  # native source-closure fixture. Keep the test bounded to that package; it
  # must never silently fall back to a host clang++.
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_worker_test_lib_dir)
  get_filename_component(_cxxlens_worker_test_llvm_root
                         "${_cxxlens_worker_test_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_worker_test_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_worker_test_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_worker_test_clangxx)
    execute_process(
      COMMAND "${_cxxlens_worker_test_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_worker_test_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_include_directories(
      cxxlens-clang22-source-closure-task-v4-worker SYSTEM
      PRIVATE "${_cxxlens_worker_test_llvm_root}/include")
    target_compile_definitions(
      cxxlens-clang22-source-closure-task-v4-worker
      PRIVATE
        CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_worker_test_llvm_root}"
        CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_worker_test_clangxx}"
        CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_worker_test_resource_dir}"
    )
  endif()
  unset(_cxxlens_worker_test_lib_dir)
  unset(_cxxlens_worker_test_llvm_root)
  unset(_cxxlens_worker_test_clangxx)
  unset(_cxxlens_worker_test_resource_dir)
endif()
add_test(NAME adapter.clang22-source-closure-task-v4-worker
         COMMAND cxxlens-clang22-source-closure-task-v4-worker)
set_tests_properties(
  adapter.clang22-source-closure-task-v4-worker
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;task;v4;worker;security")

cxxlens_add_test_executable(cxxlens-clang22-provider-worker-v4
                            adapter/clang22/provider_worker_v4_test.cpp)
target_link_libraries(cxxlens-clang22-provider-worker-v4
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-provider-worker-v4
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_worker_v4_test_lib_dir)
  get_filename_component(_cxxlens_worker_v4_test_llvm_root
                         "${_cxxlens_worker_v4_test_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_worker_v4_test_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_worker_v4_test_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_worker_v4_test_clangxx)
    execute_process(
      COMMAND "${_cxxlens_worker_v4_test_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_worker_v4_test_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(
      cxxlens-clang22-provider-worker-v4
      PRIVATE
        CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_worker_v4_test_llvm_root}"
        CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_worker_v4_test_clangxx}"
        CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_worker_v4_test_resource_dir}"
    )
  endif()
  unset(_cxxlens_worker_v4_test_lib_dir)
  unset(_cxxlens_worker_v4_test_llvm_root)
  unset(_cxxlens_worker_v4_test_clangxx)
  unset(_cxxlens_worker_v4_test_resource_dir)
endif()
add_test(NAME adapter.clang22-provider-worker-v4
         COMMAND cxxlens-clang22-provider-worker-v4)
set_tests_properties(
  adapter.clang22-provider-worker-v4
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;task;v4;worker;security")

cxxlens_add_test_executable(
  cxxlens-clang22-provider-worker-v4-ast-observer
  adapter/clang22/provider_worker_v4_ast_observer_test.cpp)
target_link_libraries(cxxlens-clang22-provider-worker-v4-ast-observer
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-provider-worker-v4-ast-observer
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_ast_observer_lib_dir)
  get_filename_component(_cxxlens_ast_observer_llvm_root
                         "${_cxxlens_ast_observer_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_ast_observer_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_ast_observer_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_ast_observer_clangxx)
    execute_process(
      COMMAND "${_cxxlens_ast_observer_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_ast_observer_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(
      cxxlens-clang22-provider-worker-v4-ast-observer
      PRIVATE
        CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_ast_observer_llvm_root}"
        CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_ast_observer_clangxx}"
        CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_ast_observer_resource_dir}"
    )
  endif()
  unset(_cxxlens_ast_observer_lib_dir)
  unset(_cxxlens_ast_observer_llvm_root)
  unset(_cxxlens_ast_observer_clangxx)
  unset(_cxxlens_ast_observer_resource_dir)
endif()
add_test(NAME adapter.clang22-provider-worker-v4-ast-observer
         COMMAND cxxlens-clang22-provider-worker-v4-ast-observer)
set_tests_properties(
  adapter.clang22-provider-worker-v4-ast-observer
  PROPERTIES
    LABELS
    "adapter;provider;clang22;source-closure;task;v4;worker;ast;observation;security"
)

cxxlens_add_test_executable(
  cxxlens-clang22-provider-worker-v4-output-normalizer
  adapter/clang22/provider_worker_v4_output_normalizer_test.cpp)
target_link_libraries(cxxlens-clang22-provider-worker-v4-output-normalizer
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-clang22-provider-worker-v4-output-normalizer
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_v4_normalizer_lib_dir)
  get_filename_component(_cxxlens_v4_normalizer_llvm_root
                         "${_cxxlens_v4_normalizer_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_v4_normalizer_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_v4_normalizer_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_v4_normalizer_clangxx)
    execute_process(
      COMMAND "${_cxxlens_v4_normalizer_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_v4_normalizer_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(
      cxxlens-clang22-provider-worker-v4-output-normalizer
      PRIVATE
        CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_v4_normalizer_llvm_root}"
        CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_v4_normalizer_clangxx}"
        CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_v4_normalizer_resource_dir}"
    )
  endif()
  unset(_cxxlens_v4_normalizer_lib_dir)
  unset(_cxxlens_v4_normalizer_llvm_root)
  unset(_cxxlens_v4_normalizer_clangxx)
  unset(_cxxlens_v4_normalizer_resource_dir)
endif()
add_test(NAME adapter.clang22-provider-worker-v4-output-normalizer
         COMMAND cxxlens-clang22-provider-worker-v4-output-normalizer)
set_tests_properties(
  adapter.clang22-provider-worker-v4-output-normalizer
  PROPERTIES
    LABELS
    "adapter;provider;clang22;task;v4;worker;output;normalization;security;bounded"
)

cxxlens_add_test_executable(
  cxxlens-clang22-provider-worker-v4-claim-bridge
  adapter/clang22/provider_worker_v4_claim_bridge_test.cpp)
target_link_libraries(
  cxxlens-clang22-provider-worker-v4-claim-bridge
  PRIVATE cxxlens_clang22_worker_v4_claim_bridge
          cxxlens_clang22_materializer_v4_claims
          cxxlens_clang22_materializer_v4_ingress)
add_test(NAME adapter.clang22-provider-worker-v4-claim-bridge
         COMMAND cxxlens-clang22-provider-worker-v4-claim-bridge)
set_tests_properties(
  adapter.clang22-provider-worker-v4-claim-bridge
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;worker;v4;claim;security"
)

cxxlens_add_test_executable(cxxlens-clang22-provider-worker-channel
                            adapter/clang22/provider_worker_channel_test.cpp)
target_link_libraries(
  cxxlens-clang22-provider-worker-channel
  PRIVATE cxxlens_clang22_materialization_codecs cxxlens::provider_sdk)
if(CXXLENS_BUILD_SHARED)
  # The channel launch helper is source-private and intentionally hidden from
  # the installed provider SDK. Embed its runtime object closure for this
  # process-boundary fixture, matching the worker executable.
  target_sources(
    cxxlens-clang22-provider-worker-channel
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-clang22-provider-worker-channel
                        PRIVATE cxxlens_protocol_v2)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_worker_channel_lib_dir)
  get_filename_component(_cxxlens_worker_channel_llvm_root
                         "${_cxxlens_worker_channel_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_worker_channel_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_worker_channel_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_worker_channel_clangxx)
    execute_process(
      COMMAND "${_cxxlens_worker_channel_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_worker_channel_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(
      cxxlens-clang22-provider-worker-channel
      PRIVATE
        CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_worker_channel_llvm_root}"
        CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_worker_channel_clangxx}"
        CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_worker_channel_resource_dir}"
    )
  endif()
  unset(_cxxlens_worker_channel_lib_dir)
  unset(_cxxlens_worker_channel_llvm_root)
  unset(_cxxlens_worker_channel_clangxx)
  unset(_cxxlens_worker_channel_resource_dir)
endif()
add_dependencies(cxxlens-clang22-provider-worker-channel
                 cxxlens-clang-worker-22)
add_test(NAME adapter.clang22-provider-worker-channel
         COMMAND cxxlens-clang22-provider-worker-channel
                 "$<TARGET_FILE:cxxlens-clang-worker-22>")
set_tests_properties(
  adapter.clang22-provider-worker-channel
  PROPERTIES
    LABELS
    "adapter;provider;clang22;source-closure;task;v4;worker;process;security")

# The native bridge test needs a real compiler binary/resource-dir/root,
# resolved from the exact LLVM/Clang 22 package `find_package(LLVM 22.1 CONFIG)`
# locates for the real adapter (CXXLENS_CLANG22_LIBRARY_DIRS, set by
# cxxlens_configure_clang22 in cmake/CxxlensClangTargets.cmake) rather than an
# assumed host path such as /usr/bin/clang++-22. This is the same "admitted
# toolchain root" the native bridge itself pins through qualified_read_roots --
# one source of truth, not a second ad hoc guess for the test.
cxxlens_add_test_executable(cxxlens-clang22-source-closure-native
                            adapter/clang22/source_closure_native_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-native
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  # The worker-private provider SDK is a static closure.  Its provider.cpp
  # references hidden transcript/runtime definitions that cannot be resolved
  # from the public shared SDK.  Place the exact private runtime objects on the
  # final native-fixture link after that closure, matching the sealed worker.
  target_sources(cxxlens-clang22-source-closure-native
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
if(CXXLENS_CLANG22_AVAILABLE AND CXXLENS_CLANG22_LIBRARY_DIRS)
  list(GET CXXLENS_CLANG22_LIBRARY_DIRS 0 _cxxlens_native_test_lib_dir)
  get_filename_component(_cxxlens_native_test_llvm_root
                         "${_cxxlens_native_test_lib_dir}" DIRECTORY)
  find_program(
    _cxxlens_native_test_clangxx
    NAMES clang++-22 clang++
    PATHS "${_cxxlens_native_test_llvm_root}/bin"
    NO_DEFAULT_PATH NO_CACHE)
  if(_cxxlens_native_test_clangxx)
    execute_process(
      COMMAND "${_cxxlens_native_test_clangxx}" -print-resource-dir
      OUTPUT_VARIABLE _cxxlens_native_test_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _cxxlens_native_test_resource_result)
    if(_cxxlens_native_test_resource_result EQUAL 0
       AND IS_DIRECTORY "${_cxxlens_native_test_resource_dir}")
      target_compile_definitions(
        cxxlens-clang22-source-closure-native
        PRIVATE
          CXXLENS_TEST_CLANG22_ROOT="${_cxxlens_native_test_llvm_root}"
          CXXLENS_TEST_CLANGXX22_PATH="${_cxxlens_native_test_clangxx}"
          CXXLENS_TEST_CLANG22_RESOURCE_DIR="${_cxxlens_native_test_resource_dir}"
      )
      add_test(NAME adapter.clang22-source-closure-native
               COMMAND cxxlens-clang22-source-closure-native)
      set_tests_properties(
        adapter.clang22-source-closure-native
        PROPERTIES
          LABELS
          "adapter;provider;clang22;source-closure;vfs;native;security;contract"
      )
    endif()
  endif()
  unset(_cxxlens_native_test_lib_dir)
  unset(_cxxlens_native_test_llvm_root)
  unset(_cxxlens_native_test_clangxx)
  unset(_cxxlens_native_test_resource_dir)
  unset(_cxxlens_native_test_resource_result)
endif()

cxxlens_add_test_executable(cxxlens-r2-vertical-slice
                            integration/r2_vertical_slice_test.cpp)
target_sources(cxxlens-r2-vertical-slice
               PRIVATE $<TARGET_OBJECTS:cxxlens-test-sdk-support>)
target_link_libraries(cxxlens-r2-vertical-slice PRIVATE cxxlens::provider_sdk)
if(CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-r2-vertical-slice
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_protocol_v2_adapter.cpp")
  target_link_libraries(cxxlens-r2-vertical-slice PRIVATE cxxlens_protocol_v2)
endif()
add_test(NAME integration.r2-vertical-slice COMMAND cxxlens-r2-vertical-slice)
set_tests_properties(
  integration.r2-vertical-slice
  PROPERTIES
    LABELS
    "integration;acceptance;r2;recipe;query;provider;clang22;store;determinism"
    PROCESSORS 2 TIMEOUT 60)

add_test(
  NAME link.provider-sdk-llvm-free
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/link/provider_sdk_link_closure.py"
    "$<TARGET_FILE:cxxlens-unit-sdk-provider-runtime>")
set_tests_properties(link.provider-sdk-llvm-free
                     PROPERTIES LABELS "link;sdk;provider;llvm-free;acceptance")

cxxlens_add_test_executable(cxxlens-public-header-cxxlens
                            public_headers/cxxlens_header_test.cpp)
target_link_libraries(cxxlens-public-header-cxxlens PRIVATE cxxlens::cxxlens)
add_test(NAME public-api.cxxlens-header COMMAND cxxlens-public-header-cxxlens)
set_tests_properties(public-api.cxxlens-header
                     PROPERTIES LABELS "public-api;llvm-free")

cxxlens_add_test_executable(cxxlens-public-header-sdk
                            public_headers/sdk_header_test.cpp)
target_link_libraries(cxxlens-public-header-sdk PRIVATE cxxlens::provider_sdk)
add_test(NAME public-api.sdk-header COMMAND cxxlens-public-header-sdk)
set_tests_properties(public-api.sdk-header
                     PROPERTIES LABELS "public-api;sdk;llvm-free")

cxxlens_add_test_executable(cxxlens-public-header-provider-clang22
                            public_headers/provider_clang22_header_test.cpp)
target_link_libraries(cxxlens-public-header-provider-clang22
                      PRIVATE cxxlens::clang22_provider_sdk)
add_test(NAME public-api.provider-clang22-header
         COMMAND cxxlens-public-header-provider-clang22)
set_tests_properties(public-api.provider-clang22-header
                     PROPERTIES LABELS "public-api;native-sdk;clang22")

cxxlens_add_test_executable(
  cxxlens-public-api-provider-clang22-translation-unit-input
  public_headers/provider_clang22_translation_unit_input_test.cpp)
target_link_libraries(cxxlens-public-api-provider-clang22-translation-unit-input
                      PRIVATE cxxlens::clang22_provider_sdk)
add_test(NAME acceptance.public-api.provider-clang22-translation-unit-input
         COMMAND cxxlens-public-api-provider-clang22-translation-unit-input)
set_tests_properties(
  acceptance.public-api.provider-clang22-translation-unit-input
  PROPERTIES LABELS
             "acceptance;public-api;native-sdk;clang22;validation;security")

foreach(
  example IN
  ITEMS typed_query
        dynamic_query
        portable_provider
        recipe
        relation_claim_batch
        snapshot_store
        query_execution
        incremental_materialization)
  set(target_name "cxxlens-sdk-example-${example}")
  cxxlens_add_test_executable(
    ${target_name} "${PROJECT_SOURCE_DIR}/examples/sdk/${example}.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::provider_sdk)
  add_test(NAME "sdk.example-${example}" COMMAND ${target_name})
  set_tests_properties("sdk.example-${example}"
                       PROPERTIES LABELS "sdk;example;acceptance")
endforeach()

cxxlens_add_test_executable(
  cxxlens-sdk-example-clang22-native
  "${PROJECT_SOURCE_DIR}/examples/sdk/clang22_native_provider.cpp")
target_link_libraries(cxxlens-sdk-example-clang22-native
                      PRIVATE cxxlens::clang22_provider_sdk)
add_test(NAME sdk.example-clang22-native
         COMMAND cxxlens-sdk-example-clang22-native)
set_tests_properties(sdk.example-clang22-native
                     PROPERTIES LABELS "sdk;example;native-sdk;acceptance")

foreach(example IN ITEMS dynamic_literal_type_mismatch unordered_limit
                         invalid_recipe provider_no_credit)
  set(target_name "cxxlens-sdk-negative-${example}")
  cxxlens_add_test_executable(
    ${target_name} "${PROJECT_SOURCE_DIR}/examples/sdk/negative/${example}.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::provider_sdk)
  add_test(NAME "sdk.negative-${example}" COMMAND ${target_name})
  set_tests_properties("sdk.negative-${example}"
                       PROPERTIES LABELS "sdk;negative;acceptance")
endforeach()

if(CXXLENS_ENABLE_ASAN OR CXXLENS_ENABLE_UBSAN)
  cxxlens_add_test_executable(cxxlens-sanitizer-canary
                              canary/sanitizer_canary.cpp)
  if(CXXLENS_ENABLE_ASAN)
    add_test(
      NAME sanitizer.canary-address
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/canary/expect_failure.py"
        "$<TARGET_FILE:cxxlens-sanitizer-canary>" address)
    set_tests_properties(sanitizer.canary-address
                         PROPERTIES LABELS "sanitizer;canary;asan")
  endif()
  if(CXXLENS_ENABLE_UBSAN)
    add_test(
      NAME sanitizer.canary-undefined
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/canary/expect_failure.py"
        "$<TARGET_FILE:cxxlens-sanitizer-canary>" undefined)
    set_tests_properties(sanitizer.canary-undefined
                         PROPERTIES LABELS "sanitizer;canary;ubsan")
  endif()
endif()

if(CXXLENS_ENABLE_TSAN)
  foreach(role IN ITEMS test provider)
    set(target_name "cxxlens-tsan-${role}-canary")
    cxxlens_add_test_executable(${target_name} canary/sanitizer_canary.cpp)
    add_test(
      NAME "sanitizer.canary-thread-${role}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/canary/expect_failure.py"
        "$<TARGET_FILE:${target_name}>" thread)
    set_tests_properties("sanitizer.canary-thread-${role}"
                         PROPERTIES LABELS "sanitizer;canary;tsan;${role}")
  endforeach()
endif()

foreach(
  contract IN
  ITEMS relation_contract
        query_contract
        semantic_guarantee
        snapshot_store_contract
        sqlite_store_contract
        provider_protocol
        provider_runtime
        security_contract
        source_closure_transport
        release_contract
        clang22_materialization
        sdk_contract)
  add_test(NAME "quality.ng-${contract}"
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/quality/test_ng_${contract}.py")
  set_tests_properties("quality.ng-${contract}"
                       PROPERTIES LABELS "quality;ng;contract")
endforeach()

add_test(
  NAME sdk.doctor-product
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/sdk/check_sdk_doctor_product.py"
    "$<TARGET_FILE:cxxlens-sdk-doctor>")
set_tests_properties(sdk.doctor-product PROPERTIES LABELS "sdk;doctor;product")

configure_file(install/run_install_test.cmake.in
               "${CMAKE_CURRENT_BINARY_DIR}/run_install_test.cmake" @ONLY)
set(cxxlens_install_script "${CMAKE_CURRENT_BINARY_DIR}/run_install_test.cmake")
add_test(NAME install.prepare COMMAND "${CMAKE_COMMAND}" -DPHASE=prepare -P
                                      "${cxxlens_install_script}")
set_tests_properties(
  install.prepare PROPERTIES LABELS "install;package;fixture" FIXTURES_SETUP
                             installed_staging PROCESSORS 2)
add_test(NAME install.relocation COMMAND "${CMAKE_COMMAND}" -DPHASE=relocation
                                         -P "${cxxlens_install_script}")
set_tests_properties(
  install.relocation
  PROPERTIES LABELS
             "install;package;fixture;acceptance"
             FIXTURES_REQUIRED
             installed_staging
             FIXTURES_SETUP
             installed_prefix
             PROCESSORS
             2)
foreach(
  phase IN
  ITEMS core-consumer
        sdk-consumer
        clang22-sdk-consumer
        real-project-consumer
        real-project-incompatible-version
        examples-consumer
        shared-runtime-layout)
  add_test(NAME "install.${phase}"
           COMMAND "${CMAKE_COMMAND}" "-DPHASE=${phase}" -P
                   "${cxxlens_install_script}")
  set_tests_properties(
    "install.${phase}"
    PROPERTIES LABELS "install;package;acceptance;consumer" FIXTURES_REQUIRED
               installed_prefix PROCESSORS 2)
endforeach()

if(CXXLENS_CLANG22_AVAILABLE)
  add_test(NAME install.clang22-materializer-v2-2
           COMMAND "${CMAKE_COMMAND}" -DPHASE=clang22-materializer-v2-2 -P
                   "${cxxlens_install_script}")
  set_tests_properties(
    install.clang22-materializer-v2-2
    PROPERTIES LABELS
               "install;package;acceptance;consumer;materialization;v2.2"
               FIXTURES_REQUIRED installed_prefix PROCESSORS 2)
endif()

cxxlens_configure_sanitizer_tests()
if(CXXLENS_ENABLE_ASAN)
  set_property(
    TEST install.clang22-materializer-v2-2
    APPEND
    PROPERTY ENVIRONMENT "CXXLENS_ASAN_INSTALLED_TEST_PROFILE=1")
endif()

add_custom_target(cxxlens-ng-test-binaries)
add_dependencies(
  cxxlens-ng-test-binaries
  cxxlens-sdk-doctor
  cxxlens-clang-worker-22
  cxxlens-clang22-materialize
  cxxlens-clang22-provider-worker-protocol
  cxxlens-clang22-materialization-json
  cxxlens-clang22-materialization-identity
  cxxlens-clang22-materialization-io
  cxxlens-clang22-materialization-store
  cxxlens-clang22-materialization-store-candidate
  cxxlens-clang22-materialization-store-projection
  cxxlens-clang22-materialization-store-candidate-bridge
  cxxlens-clang22-materialization-store-outcome
  cxxlens-clang22-materialization-request-v2-2
  cxxlens-clang22-provider-task-v4
  cxxlens-clang22-provider-task-v4-authority
  cxxlens-clang22-provider-worker-ingress-decoder
  cxxlens-clang22-materialization-v4-claim-binding
  cxxlens-clang22-materialization-v4-store-positive
  cxxlens-clang22-materialization-store-v6-typed-ingress
  cxxlens-protocol-v2
  cxxlens-unit-sdk-provider-protocol-v2-adapter
  cxxlens-clang22-materialization-pipeline-v2-2
  cxxlens-clang22-materialization-rooted-vfs
  cxxlens-clang22-materialization-occurrence
  cxxlens-clang22-observation-v2
  cxxlens-clang22-unicode-nfc
  cxxlens-clang22-source-closure
  cxxlens-clang22-source-closure-vfs
  cxxlens-clang22-source-closure-invocation
  cxxlens-clang22-source-closure-transport
  cxxlens-clang22-source-closure-spool
  cxxlens-clang22-source-closure-receiver
  cxxlens-clang22-source-closure-task-v4
  cxxlens-clang22-source-closure-task-v4-worker
  cxxlens-clang22-source-closure-native
  cxxlens-clang22-provider-worker-v4
  cxxlens-clang22-provider-worker-v4-ast-observer
  cxxlens-clang22-provider-worker-v4-output-normalizer
  cxxlens-clang22-provider-worker-v4-claim-bridge
  cxxlens-clang22-provider-worker-channel
  cxxlens-r2-vertical-slice
  cxxlens-provider-process-fixture
  cxxlens-public-header-cxxlens
  cxxlens-public-header-provider-clang22
  cxxlens-public-api-provider-clang22-translation-unit-input
  cxxlens-public-header-sdk
  cxxlens-unit-sdk
  cxxlens-unit-sdk-provider-runtime
  cxxlens-unit-sdk-provider-expected-output-validator
  cxxlens-unit-sdk-provider-ng1-validation
  cxxlens-unit-sdk-provider-ng1-spill-port
  cxxlens-unit-sdk-provider-ng1-transport
  cxxlens-unit-sdk-provider-ng1-recovery-adapter
  cxxlens-unit-sdk-provider-ng1-session
  cxxlens-unit-sdk-provider-ng1-live-driver
  cxxlens-unit-sdk-query-runtime
  cxxlens-unit-sdk-sqlite-connection-lifecycle
  cxxlens-unit-sdk-sqlite-default-forwarding-vfs
  cxxlens-unit-sdk-sqlite-disposable-qualification
  cxxlens-unit-sdk-sqlite-limit-length-precedence
  cxxlens-unit-sdk-sqlite-payload-streaming
  cxxlens-unit-sdk-sqlite-shm-process-port
  cxxlens-unit-sdk-sqlite-shm-vfs-alias-registration
  cxxlens-unit-sdk-sqlite-shm-vfs-alias-identity-sealer
  cxxlens-unit-sdk-sqlite-shm-identity-issuer
  cxxlens-unit-sdk-sqlite-shm-mapping-lease
  cxxlens-unit-sdk-sqlite-shm-mapping-registry
  cxxlens-unit-sdk-sqlite-writer-shm-mapping-epoch
  cxxlens-unit-sdk-sqlite-writer-shm-mapping-semantics
  cxxlens-unit-sdk-sqlite-source-shm-readonly-preflight
  cxxlens-unit-sdk-sqlite-store-fault-injection
  cxxlens-unit-sdk-sqlite-store-terminal
  cxxlens-unit-sdk-sqlite-terminal-reclassifier
  cxxlens-unit-sdk-sqlite-exact-empty-cold-reclassifier
  cxxlens-unit-sdk-sqlite-store-wal-routing
  cxxlens-unit-sdk-sqlite-wal-source-capture
  cxxlens-unit-sdk-sqlite-wal-recovery-workspace
  cxxlens-unit-sdk-sqlite-wal-receipt
  cxxlens-unit-sdk-store
  cxxlens-unit-sdk-store-operation-port
  cxxlens-unit-sdk-bounded-store-v6-memory-e2e
  cxxlens-unit-sdk-snapshot-store-v5-codec
  cxxlens-unit-sdk-sqlite-writer-gate-outcome
  cxxlens-df0200-claim-batch-corpus
  cxxlens-sdk-example-typed_query
  cxxlens-sdk-example-dynamic_query
  cxxlens-sdk-example-portable_provider
  cxxlens-sdk-example-recipe
  cxxlens-sdk-example-relation_claim_batch
  cxxlens-sdk-example-snapshot_store
  cxxlens-sdk-example-query_execution
  cxxlens-sdk-example-incremental_materialization
  cxxlens-sdk-example-clang22-native
  cxxlens-sdk-negative-dynamic_literal_type_mismatch
  cxxlens-sdk-negative-unordered_limit
  cxxlens-sdk-negative-invalid_recipe
  cxxlens-sdk-negative-provider_no_credit
  cxxlens-g5-runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(cxxlens-ng-test-binaries
                   cxxlens-unit-sdk-provider-process-channel)
  add_dependencies(cxxlens-ng-test-binaries cxxlens-clang22-source-closure-fd
                   cxxlens-clang22-installed-materializer-source-closure)
  add_dependencies(cxxlens-ng-test-binaries
                   cxxlens-unit-sdk-sqlite-202-crash-recovery)
  if(CXXLENS_TEST_HAS_GLIBC_DLMOPEN_DLVSYM)
    add_dependencies(
      cxxlens-ng-test-binaries cxxlens-unit-sdk-sqlite-migration-outcomes
      cxxlens-unit-sdk-store-sqlite-crash-restart)
    if(CXXLENS_TEST_HAS_OFD_LOCKS)
      add_dependencies(cxxlens-ng-test-binaries
                       cxxlens-unit-sdk-sqlite-source-availability)
    endif()
  endif()
endif()

if(CXXLENS_ENABLE_ASAN OR CXXLENS_ENABLE_UBSAN)
  add_dependencies(cxxlens-ng-test-binaries cxxlens-sanitizer-canary)
endif()
if(CXXLENS_ENABLE_TSAN)
  add_dependencies(cxxlens-ng-test-binaries cxxlens-tsan-test-canary
                   cxxlens-tsan-provider-canary)
endif()
