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
target_link_libraries(cxxlens-unit-sdk-store PRIVATE cxxlens::kernel)
add_test(NAME unit.sdk-store COMMAND cxxlens-unit-sdk-store)
set_tests_properties(unit.sdk-store PROPERTIES LABELS
                                               "unit;sdk;store;transaction")

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
set_tests_properties(unit.sdk-provider-runtime-sealed
                     PROPERTIES LABELS "unit;sdk;provider;sealed;adoption")
set_tests_properties(
  unit.sdk-provider-runtime-host-streaming unit.sdk-provider-runtime-receipt
  PROPERTIES LABELS "unit;sdk;provider;protocol;streaming;sealed;security")

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

cxxlens_add_test_executable(cxxlens-clang22-materialization-store
                            adapter/clang22/materialization_store_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-store
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-store
         COMMAND cxxlens-clang22-materialization-store)
set_tests_properties(
  adapter.clang22-materialization-store
  PROPERTIES LABELS
             "adapter;provider;clang22;materialization;store;transaction;sqlite"
)

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
add_test(NAME adapter.clang22-materialization-rooted-vfs
         COMMAND cxxlens-clang22-materialization-rooted-vfs)
set_tests_properties(
  adapter.clang22-materialization-rooted-vfs
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;store;sqlite;rooted-vfs;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-occurrence
                            adapter/clang22/materialization_occurrence_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-occurrence
                      PRIVATE cxxlens_clang22_materializer_core)
add_test(NAME adapter.clang22-materialization-occurrence
         COMMAND cxxlens-clang22-materialization-occurrence)
set_tests_properties(
  adapter.clang22-materialization-occurrence
  PROPERTIES
    LABELS
    "adapter;provider;clang22;materialization;installation;occurrence;security")

cxxlens_add_test_executable(cxxlens-clang22-materialization-report
                            adapter/clang22/materialization_report_test.cpp)
target_link_libraries(cxxlens-clang22-materialization-report
                      PRIVATE cxxlens_clang22_materializer_core)
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
        provider_protocol
        provider_runtime
        security_contract
        release_contract
        release_qualification
        clang22_materialization
        production_scope_closure
        sdk_contract
        ci_supply_chain
        migration_completion
        foundation_completion
        api_development_readiness
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

cxxlens_configure_sanitizer_tests()

add_custom_target(cxxlens-ng-test-binaries)
add_dependencies(
  cxxlens-ng-test-binaries
  cxxlens-clang-worker-22
  cxxlens-clang22-provider-normalizer
  cxxlens-clang22-provider-worker-protocol
  cxxlens-clang22-materialization-json
  cxxlens-clang22-materialization-identity
  cxxlens-clang22-materialization-request-driver
  cxxlens-clang22-materialization-io
  cxxlens-clang22-materialization-claims
  cxxlens-clang22-materialization-seal
  cxxlens-clang22-materialization-store
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
  cxxlens-public-header-sdk
  cxxlens-unit-sdk
  cxxlens-unit-sdk-provider-runtime
  cxxlens-unit-sdk-query-runtime
  cxxlens-unit-sdk-store
  cxxlens-g5-runtime)
