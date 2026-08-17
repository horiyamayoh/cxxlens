find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

function(cxxlens_add_test_executable target source)
  add_executable(${target} ${source})
  target_compile_features(${target} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target})
  cxxlens_enable_sanitizers(${target})
endfunction()

cxxlens_add_test_executable(cxxlens-unit-sdk unit/sdk/sdk_test.cpp)
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
add_test(
  NAME unit.sdk.registration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/sdk/check_case_registration.py"
    "$<TARGET_FILE:cxxlens-unit-sdk>" ${cxxlens_sdk_cases})
set_tests_properties(unit.sdk.registration PROPERTIES LABELS
                                                      "unit;sdk;registration")

cxxlens_add_test_executable(cxxlens-unit-sdk-store unit/sdk/store_test.cpp)
target_link_libraries(cxxlens-unit-sdk-store PRIVATE cxxlens::kernel
                                                     "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-unit-sdk-store
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME unit.sdk-store COMMAND cxxlens-unit-sdk-store)
set_tests_properties(unit.sdk-store PROPERTIES LABELS
                                               "unit;sdk;store;transaction")

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

cxxlens_add_test_executable(
  cxxlens-sqlite-store-v3-qualification-case-runner
  qualification/sqlite_store_v3_qualification_case_runner.cpp)
target_link_libraries(
  cxxlens-sqlite-store-v3-qualification-case-runner
  PRIVATE cxxlens-sqlite-store-v3-scenario-support cxxlens::kernel
          "${CMAKE_DL_LIBS}")
target_include_directories(cxxlens-sqlite-store-v3-qualification-case-runner
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(
  cxxlens-sqlite-store-v3-qualification-case-runner
  PRIVATE
    "CXXLENS_SQLITE_QUALIFICATION_CONFIGURATION=\"${CXXLENS_OCCURRENCE_CONFIGURATION}\""
    "CXXLENS_SQLITE_QUALIFICATION_REVISION=\"${CXXLENS_SOURCE_REVISION}\""
    "CXXLENS_SQLITE_QUALIFICATION_SOURCE_TREE=\"${CXXLENS_SOURCE_TREE}\"")

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

cxxlens_add_test_executable(cxxlens-unit-sdk-sqlite-store-fault-injection
                            unit/sdk/sqlite_store_fault_injection_test.cpp)
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
  target_sources(cxxlens-unit-sdk-provider-runtime
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME unit.sdk-provider-runtime
         COMMAND cxxlens-unit-sdk-provider-runtime
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")
include(CheckCXXSourceCompiles)
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
endif()
add_test(NAME unit.sdk-provider-runtime-sealed
         COMMAND cxxlens-unit-sdk-provider-runtime --sealed-only)
add_test(NAME unit.sdk-provider-runtime-host-streaming
         COMMAND cxxlens-unit-sdk-provider-runtime --host-only
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")
add_test(NAME unit.sdk-provider-runtime-receipt
         COMMAND cxxlens-unit-sdk-provider-runtime --receipt-only
                 "$<TARGET_FILE:cxxlens-provider-process-fixture>")
set_tests_properties(
  unit.sdk-provider-runtime
  PROPERTIES
    LABELS
    "unit;sdk;provider;process;sandbox;security;budget;cancellation;slow"
    PROCESSORS 2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CXXLENS_TEST_HAS_LINUX_GLIBC_PIDFD_WAITID)
  set_tests_properties(
    unit.sdk-provider-runtime-timeout
    PROPERTIES LABELS
               "unit;sdk;provider;process;budget;cancellation;regression"
               TIMEOUT 30 SKIP_RETURN_CODE 77)
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
target_sources(
  cxxlens-unit-sdk-provider-ng1-session
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_session.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_recovery.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_spill_port_internal.cpp")
if(NOT CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-session
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
endif()
target_link_libraries(cxxlens-unit-sdk-provider-ng1-session
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-session
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-unit-sdk-provider-ng1-session
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME unit.sdk-provider-ng1-session
         COMMAND cxxlens-unit-sdk-provider-ng1-session)
set_tests_properties(
  unit.sdk-provider-ng1-session
  PROPERTIES LABELS "unit;sdk;provider;ng1;session;recovery;spill;fail-closed")

cxxlens_add_test_executable(cxxlens-unit-sdk-provider-ng1-live-driver
                            unit/sdk/provider_ng1_live_driver_test.cpp)
target_sources(
  cxxlens-unit-sdk-provider-ng1-live-driver
  PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_live_driver.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_session.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_recovery.cpp"
          "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_spill_port_internal.cpp")
if(NOT CXXLENS_BUILD_SHARED)
  target_sources(
    cxxlens-unit-sdk-provider-ng1-live-driver
    PRIVATE "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/sdk/provider_ng1_validation.cpp")
endif()
target_link_libraries(cxxlens-unit-sdk-provider-ng1-live-driver
                      PRIVATE cxxlens::provider_sdk)
target_include_directories(cxxlens-unit-sdk-provider-ng1-live-driver
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CXXLENS_BUILD_SHARED)
  target_sources(cxxlens-unit-sdk-provider-ng1-live-driver
                 PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME unit.sdk-provider-ng1-live-driver
         COMMAND cxxlens-unit-sdk-provider-ng1-live-driver)
set_tests_properties(
  unit.sdk-provider-ng1-live-driver
  PROPERTIES LABELS "unit;sdk;provider;ng1;live;transport;session;fail-closed")

cxxlens_add_test_executable(cxxlens-clang22-provider-normalizer
                            adapter/clang22/provider_normalizer_test.cpp)
target_link_libraries(cxxlens-clang22-provider-normalizer
                      PRIVATE cxxlens_clang22_worker_core)
target_include_directories(cxxlens-clang22-provider-normalizer
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME adapter.clang22-provider-normalizer
         COMMAND cxxlens-clang22-provider-normalizer)
set_tests_properties(
  adapter.clang22-provider-normalizer
  PROPERTIES LABELS
             "adapter;provider;clang22;normalizer;identity;coverage;unresolved")

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
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-json
         COMMAND cxxlens-clang22-materialization-json)
set_tests_properties(
  adapter.clang22-materialization-json
  PROPERTIES LABELS "adapter;provider;clang22;materialization;json;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-identity
                            adapter/clang22/materialization_identity_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-identity
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-identity
         COMMAND cxxlens-clang22-materialization-identity)
set_tests_properties(
  adapter.clang22-materialization-identity
  PROPERTIES LABELS "adapter;provider;clang22;materialization;identity")

cxxlens_add_test_executable(cxxlens-clang22-materialization-request-driver
                            adapter/clang22/materialization_request_driver.cpp)
target_link_libraries(cxxlens-clang22-materialization-request-driver
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(
  NAME adapter.clang22-materialization-request
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/adapter/clang22/materialization_request_test.py"
    --root "${PROJECT_SOURCE_DIR}" --driver
    "$<TARGET_FILE:cxxlens-clang22-materialization-request-driver>")
set_tests_properties(
  adapter.clang22-materialization-request
  PROPERTIES
    LABELS "adapter;provider;clang22;materialization;request;identity;security")

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
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-io
         COMMAND cxxlens-clang22-materialization-io)
set_tests_properties(
  adapter.clang22-materialization-io
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;streaming;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-seal
                            adapter/clang22/materialization_seal_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-seal
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-seal
         COMMAND cxxlens-clang22-materialization-seal)
set_tests_properties(
  adapter.clang22-materialization-seal
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;seal;identity;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-claims
                            adapter/clang22/materialization_claims_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-claims
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-claims
         COMMAND cxxlens-clang22-materialization-claims "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  adapter.clang22-materialization-claims
  PROPERTIES
    LABELS "adapter;provider;clang22;materialization;claims;partition;identity")

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
  PRIVATE cxxlens_clang22_materializer_core "${CMAKE_DL_LIBS}")
add_test(NAME adapter.clang22-materialization-store
         COMMAND cxxlens-clang22-materialization-store)
set_tests_properties(
  adapter.clang22-materialization-store
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;transaction;sqlite"
)

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-store-outcome
  adapter/clang22/materialization_store_outcome_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-store-outcome
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-store-outcome
         COMMAND cxxlens-clang22-materialization-store-outcome)
set_tests_properties(
  adapter.clang22-materialization-store-outcome
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;outcome;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-request-stream
  adapter/clang22/materialization_request_stream_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-request-stream
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-request-stream
         COMMAND cxxlens-clang22-materialization-request-stream)
set_tests_properties(
  adapter.clang22-materialization-request-stream
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;request;streaming;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-request-identity
  adapter/clang22/materialization_request_identity_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-request-identity
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-request-identity
         COMMAND cxxlens-clang22-materialization-request-identity)
set_tests_properties(
  adapter.clang22-materialization-request-identity
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;request;identity;streaming;security"
)

cxxlens_add_test_executable(cxxlens-clang22-materialization-task-spool
                            adapter/clang22/materialization_task_spool_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-task-spool
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-task-spool
         COMMAND cxxlens-clang22-materialization-task-spool)
set_tests_properties(
  adapter.clang22-materialization-task-spool
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;task;streaming;security")

cxxlens_add_test_executable(
  cxxlens-clang22-materialization-request-v2-1
  adapter/clang22/materialization_request_v2_1_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-request-v2-1
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-request-v2-1
         COMMAND cxxlens-clang22-materialization-request-v2-1)
set_tests_properties(
  adapter.clang22-materialization-request-v2-1
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;request;v2.1;streaming;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-rooted-vfs
                            adapter/clang22/materialization_rooted_vfs_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-rooted-vfs
                      PRIVATE cxxlens_clang22_materializer_core)
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
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-occurrence
         COMMAND cxxlens-clang22-materialization-occurrence)
set_tests_properties(
  adapter.clang22-materialization-occurrence
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;occurrence;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-report
                            adapter/clang22/materialization_report_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-report
                      PRIVATE cxxlens_clang22_materializer_core)
target_compile_definitions(
  cxxlens-clang22-materialization-report
  PRIVATE CXXLENS_CLANG22_MATERIALIZATION_REPORT_TESTING=1)
add_test(NAME adapter.clang22-materialization-report
         COMMAND cxxlens-clang22-materialization-report)
add_test(
  NAME adapter.clang22-materialization-report-schema
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/adapter/clang22/materialization_report_schema_test.py"
    --root "${PROJECT_SOURCE_DIR}" --driver
    "$<TARGET_FILE:cxxlens-clang22-materialization-report>")
set_tests_properties(
  adapter.clang22-materialization-report
  adapter.clang22-materialization-report-schema
  PROPERTIES LABELS "adapter;provider;clang22;materialization;report;security")

cxxlens_add_test_executable(cxxlens-clang22-observation-v2
                            adapter/clang22/observation_v2_test.cpp)
target_link_libraries(cxxlens-clang22-observation-v2
                      PRIVATE cxxlens_clang22_worker_core)
add_test(NAME adapter.clang22-observation-v2
         COMMAND cxxlens-clang22-observation-v2)
set_tests_properties(
  adapter.clang22-observation-v2
  PROPERTIES LABELS "adapter;provider;clang22;materialization;observation")

cxxlens_add_test_executable(cxxlens-clang22-unicode-nfc
                            adapter/clang22/unicode_nfc_test.cpp)
target_link_libraries(cxxlens-clang22-unicode-nfc
                      PRIVATE cxxlens_clang22_worker_core)
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
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;security;identity;contract")

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
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;invocation;security;contract")


cxxlens_add_test_executable(cxxlens-clang22-source-closure-native
                            adapter/clang22/source_closure_native_test.cpp)
target_link_libraries(cxxlens-clang22-source-closure-native
                      PRIVATE cxxlens_clang22_worker_core)
if(CXXLENS_BUILD_SHARED)
  # The worker-private provider SDK is a static closure.  Its provider.cpp
  # references hidden transcript/runtime definitions that cannot be resolved
  # from the public shared SDK.  Place the exact private runtime objects on the
  # final native-fixture link after that closure, matching the sealed worker.
  target_sources(
    cxxlens-clang22-source-closure-native
    PRIVATE $<TARGET_OBJECTS:cxxlens_provider_runtime_internal>)
endif()
add_test(NAME adapter.clang22-source-closure-native
         COMMAND cxxlens-clang22-source-closure-native)
set_tests_properties(
  adapter.clang22-source-closure-native
  PROPERTIES LABELS
             "adapter;provider;clang22;source-closure;vfs;native;security;contract"
             SKIP_RETURN_CODE 77)

cxxlens_add_test_executable(cxxlens-r2-vertical-slice
                            integration/r2_vertical_slice_test.cpp)
target_link_libraries(cxxlens-r2-vertical-slice
                      PRIVATE cxxlens_clang22_worker_core)
target_include_directories(cxxlens-r2-vertical-slice
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
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
        sqlite_store_v3_qualification
        provider_protocol
        provider_ng1_qualification
        provider_runtime
        security_contract
        release_contract
        release_qualification
        clang22_materialization
        clang22_materialization_scale
        production_scope_closure
        sdk_contract
        ci_supply_chain
        migration_completion
        foundation_completion
        api_development_readiness
        use_case_capability_catalog
        agent_context
        design_feedback
        public_callable_inventory
        g5_qualification)
  add_test(NAME "quality.ng-${contract}"
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/quality/test_ng_${contract}.py")
  set_tests_properties("quality.ng-${contract}"
                       PROPERTIES LABELS "quality;ng;contract")
endforeach()

add_test(
  NAME quality.ownership
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/quality/test_quality_ownership.py")
set_tests_properties(quality.ownership PROPERTIES LABELS
                                                  "quality;ng;contract;ci")

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
        clang22-materializer-success
        clang22-materializer-base64
        clang22-materializer-negative
        real-project-consumer
        real-project-incompatible-version
        examples-consumer
        shared-runtime-layout
        legacy-zero)
  add_test(NAME "install.${phase}"
           COMMAND "${CMAKE_COMMAND}" "-DPHASE=${phase}" -P
                   "${cxxlens_install_script}")
  set_tests_properties(
    "install.${phase}"
    PROPERTIES LABELS "install;package;acceptance;consumer" FIXTURES_REQUIRED
               installed_prefix PROCESSORS 2)
endforeach()

add_test(
  NAME install.clang22-materializer-installation-binding
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/install/clang22_materializer_installation_binding_test.py"
    --root "${PROJECT_SOURCE_DIR}" --prefix
    "${CMAKE_CURRENT_BINARY_DIR}/install-consumer/relocated-prefix"
    --executable-suffix "${CMAKE_EXECUTABLE_SUFFIX}" --evidence-dir
    "${CMAKE_CURRENT_BINARY_DIR}/install-consumer/materialization-binding-evidence"
)
set_tests_properties(
  install.clang22-materializer-installation-binding
  PROPERTIES LABELS
             "install;package;acceptance;consumer;materialization;failure;phase"
             FIXTURES_REQUIRED installed_prefix PROCESSORS 2)

cxxlens_configure_sanitizer_tests()
if(CXXLENS_ENABLE_ASAN)
  set_property(
    TEST install.clang22-materializer-success
         install.clang22-materializer-base64
         install.clang22-materializer-negative
    APPEND
    PROPERTY ENVIRONMENT "CXXLENS_ASAN_INSTALLED_QUALIFICATION=1")
endif()

add_custom_target(cxxlens-ng-test-binaries)
add_dependencies(
  cxxlens-ng-test-binaries
  cxxlens-clang-worker-22
  cxxlens-clang22-materialize
  cxxlens-clang22-provider-normalizer
  cxxlens-clang22-provider-worker-protocol
  cxxlens-clang22-materialization-json
  cxxlens-clang22-materialization-identity
  cxxlens-clang22-materialization-request-driver
  cxxlens-clang22-materialization-io
  cxxlens-clang22-materialization-claims
  cxxlens-clang22-materialization-seal
  cxxlens-clang22-materialization-store
  cxxlens-clang22-materialization-store-outcome
  cxxlens-clang22-materialization-request-stream
  cxxlens-clang22-materialization-request-identity
  cxxlens-clang22-materialization-task-spool
  cxxlens-clang22-materialization-request-v2-1
  cxxlens-clang22-materialization-rooted-vfs
  cxxlens-clang22-materialization-occurrence
  cxxlens-clang22-materialization-report
  cxxlens-clang22-observation-v2
  cxxlens-clang22-unicode-nfc
  cxxlens-r2-vertical-slice
  cxxlens-provider-process-fixture
  cxxlens-public-header-cxxlens
  cxxlens-public-header-provider-clang22
  cxxlens-public-api-provider-clang22-translation-unit-input
  cxxlens-public-header-sdk
  cxxlens-unit-sdk
  cxxlens-unit-sdk-provider-runtime
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
  cxxlens-unit-sdk-sqlite-store-wal-routing
  cxxlens-unit-sdk-sqlite-wal-source-capture
  cxxlens-unit-sdk-sqlite-wal-recovery-workspace
  cxxlens-unit-sdk-sqlite-wal-receipt
  cxxlens-unit-sdk-store
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

if(CXXLENS_ENABLE_ASAN OR CXXLENS_ENABLE_UBSAN)
  add_dependencies(cxxlens-ng-test-binaries cxxlens-sanitizer-canary)
endif()
if(CXXLENS_ENABLE_TSAN)
  add_dependencies(cxxlens-ng-test-binaries cxxlens-tsan-test-canary
                   cxxlens-tsan-provider-canary)
endif()
