find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME agent.task-packet
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/agent/task_packet/test_task_packet.py")
set_tests_properties(
  agent.task-packet PROPERTIES LABELS
                               "agent;catalog;schema;determinism;property")

add_test(
  NAME agent.ownership
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/agent/ownership/test_ownership.py")
set_tests_properties(
  agent.ownership PROPERTIES LABELS
                             "agent;ownership;schema;determinism;security")

add_test(NAME agent.runner
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/agent/runner/test_runner.py")
set_tests_properties(
  agent.runner PROPERTIES LABELS "agent;dag;prompt;schema;determinism;security")

add_test(
  NAME agent.readiness
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/agent/readiness/test_readiness_audit.py")
set_tests_properties(
  agent.readiness
  PROPERTIES LABELS "agent;readiness;schema;determinism;security;acceptance")

add_test(
  NAME contract.high-risk-validation
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/contract_spikes/test_high_risk_contract_validation.py"
)
set_tests_properties(
  contract.high-risk-validation
  PROPERTIES LABELS "contract;spike;schema;determinism;security;acceptance")

add_executable(cxxlens-unit-version unit/version_test.cpp)
target_link_libraries(cxxlens-unit-version PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-version PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-version)
add_test(NAME unit.version COMMAND cxxlens-unit-version)
set_tests_properties(unit.version PROPERTIES LABELS unit)

add_executable(cxxlens-unit-sdk unit/sdk/sdk_test.cpp)
target_link_libraries(cxxlens-unit-sdk PRIVATE cxxlens::provider_sdk
                                               cxxlens::cxxlens)
target_compile_features(cxxlens-unit-sdk PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-sdk)
add_test(NAME unit.sdk COMMAND cxxlens-unit-sdk)
set_tests_properties(
  unit.sdk PROPERTIES LABELS "unit;sdk;relation;query;provider;native")

add_executable(cxxlens-public-header-sdk public_headers/sdk_header_test.cpp)
target_link_libraries(cxxlens-public-header-sdk PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-public-header-sdk PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-sdk)
add_test(NAME public-api.sdk-header COMMAND cxxlens-public-header-sdk)
set_tests_properties(public-api.sdk-header
                     PROPERTIES LABELS "public-api;sdk;llvm-free")

add_executable(cxxlens-public-header-provider-clang22
               public_headers/provider_clang22_header_test.cpp)
target_link_libraries(cxxlens-public-header-provider-clang22
                      PRIVATE cxxlens::clang22_provider_sdk)
target_compile_features(cxxlens-public-header-provider-clang22
                        PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-provider-clang22)
add_test(NAME public-api.provider-clang22-header
         COMMAND cxxlens-public-header-provider-clang22)
set_tests_properties(public-api.provider-clang22-header
                     PROPERTIES LABELS "public-api;native-sdk;clang22")

foreach(example IN ITEMS typed_query dynamic_query portable_provider recipe)
  set(target_name "cxxlens-sdk-example-${example}")
  add_executable(${target_name}
                 "${PROJECT_SOURCE_DIR}/examples/sdk/${example}.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::provider_sdk)
  target_compile_features(${target_name} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target_name})
  add_test(NAME "sdk.example-${example}" COMMAND ${target_name})
  set_tests_properties("sdk.example-${example}"
                       PROPERTIES LABELS "sdk;example;acceptance")
endforeach()

add_executable(cxxlens-sdk-example-clang22-native
               "${PROJECT_SOURCE_DIR}/examples/sdk/clang22_native_provider.cpp")
target_link_libraries(cxxlens-sdk-example-clang22-native
                      PRIVATE cxxlens::clang22_provider_sdk)
target_compile_features(cxxlens-sdk-example-clang22-native PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-sdk-example-clang22-native)
add_test(NAME sdk.example-clang22-native
         COMMAND cxxlens-sdk-example-clang22-native)
set_tests_properties(sdk.example-clang22-native
                     PROPERTIES LABELS "sdk;example;native-sdk;acceptance")

foreach(example IN ITEMS dynamic_literal_type_mismatch unordered_limit
                         invalid_recipe provider_no_credit)
  set(target_name "cxxlens-sdk-negative-${example}")
  add_executable(${target_name}
                 "${PROJECT_SOURCE_DIR}/examples/sdk/negative/${example}.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::provider_sdk)
  target_compile_features(${target_name} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target_name})
  add_test(NAME "sdk.negative-${example}" COMMAND ${target_name})
  set_tests_properties("sdk.negative-${example}"
                       PROPERTIES LABELS "sdk;negative;acceptance")
endforeach()

add_executable(cxxlens-unit-selectors unit/select/selector_test.cpp)
target_link_libraries(cxxlens-unit-selectors PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-selectors
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-selectors PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-selectors)
add_test(NAME unit.selectors COMMAND cxxlens-unit-selectors)
set_tests_properties(
  unit.selectors PROPERTIES LABELS "unit;select;property;schema;determinism")

add_executable(cxxlens-selector-process unit/select/selector_process.cpp)
target_link_libraries(cxxlens-selector-process PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-selector-process PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-selector-process)
add_test(
  NAME schema.selector-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/select/test_selector_process.py"
    "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:cxxlens-selector-process>")
set_tests_properties(schema.selector-process
                     PROPERTIES LABELS "select;schema;determinism;golden")

add_executable(cxxlens-public-header-select
               public_headers/select_header_test.cpp)
target_link_libraries(cxxlens-public-header-select PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-select PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-select)
add_test(NAME public-header.select COMMAND cxxlens-public-header-select)
set_tests_properties(public-header.select PROPERTIES LABELS
                                                     "public-header;select")

foreach(header IN ITEMS search explain)
  set(target_name "cxxlens-public-header-${header}")
  add_executable(${target_name} "public_headers/${header}_header_test.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::cxxlens)
  target_compile_features(${target_name} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target_name})
  add_test(NAME "public-api.${header}-header" COMMAND ${target_name})
  set_tests_properties("public-api.${header}-header"
                       PROPERTIES LABELS "public-api;search;explain")
endforeach()

foreach(header IN ITEMS core source cxxlens)
  set(target_name "cxxlens-public-header-${header}")
  add_executable(${target_name} "public_headers/${header}_header_test.cpp")
  target_link_libraries(${target_name} PRIVATE cxxlens::cxxlens)
  target_compile_features(${target_name} PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(${target_name})
  add_test(NAME "public-api.${header}-header" COMMAND ${target_name})
  set_tests_properties("public-api.${header}-header" PROPERTIES LABELS
                                                                public-api)
endforeach()

add_executable(cxxlens-public-header-identity
               public_headers/identity_header_test.cpp)
target_link_libraries(cxxlens-public-header-identity PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-identity PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-identity)
add_test(NAME public-api.identity-header COMMAND cxxlens-public-header-identity)
set_tests_properties(public-api.identity-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-failure
               public_headers/failure_header_test.cpp)
target_link_libraries(cxxlens-public-header-failure PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-failure PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-failure)
add_test(NAME public-api.failure-header COMMAND cxxlens-public-header-failure)
set_tests_properties(public-api.failure-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-evidence
               public_headers/evidence_header_test.cpp)
target_link_libraries(cxxlens-public-header-evidence PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-evidence PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-evidence)
add_test(NAME public-api.evidence-header COMMAND cxxlens-public-header-evidence)
set_tests_properties(public-api.evidence-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-finding
               public_headers/finding_header_test.cpp)
target_link_libraries(cxxlens-public-header-finding PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-finding PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-finding)
add_test(NAME public-api.finding-header COMMAND cxxlens-public-header-finding)
set_tests_properties(public-api.finding-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-schema
               public_headers/schema_header_test.cpp)
target_link_libraries(cxxlens-public-header-schema PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-schema PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-schema)
add_test(NAME public-api.schema-header COMMAND cxxlens-public-header-schema)
set_tests_properties(public-api.schema-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-configuration
               public_headers/configuration_header_test.cpp)
target_link_libraries(cxxlens-public-header-configuration
                      PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-configuration PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-configuration)
add_test(NAME public-api.configuration-header
         COMMAND cxxlens-public-header-configuration)
set_tests_properties(public-api.configuration-header PROPERTIES LABELS
                                                                public-api)

add_executable(cxxlens-public-header-testing
               public_headers/testing_header_test.cpp)
target_link_libraries(cxxlens-public-header-testing PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-testing PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-testing)
add_test(NAME public-api.testing-header COMMAND cxxlens-public-header-testing)
set_tests_properties(public-api.testing-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-workspace
               public_headers/workspace_header_test.cpp)
target_link_libraries(cxxlens-public-header-workspace PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-workspace PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-workspace)
add_test(NAME public-api.workspace-header
         COMMAND cxxlens-public-header-workspace)
set_tests_properties(public-api.workspace-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-facts public_headers/facts_header_test.cpp)
target_link_libraries(cxxlens-public-header-facts PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-facts PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-facts)
add_test(NAME public-api.facts-header COMMAND cxxlens-public-header-facts)
set_tests_properties(public-api.facts-header PROPERTIES LABELS public-api)

add_executable(cxxlens-public-header-interop-clang
               public_headers/interop_clang_header_test.cpp)
target_link_libraries(cxxlens-public-header-interop-clang
                      PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-public-header-interop-clang PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-public-header-interop-clang)
add_test(NAME public-api.interop-clang-header
         COMMAND cxxlens-public-header-interop-clang)
set_tests_properties(public-api.interop-clang-header PROPERTIES LABELS
                                                                public-api)

configure_file(install/run_install_test.cmake.in
               "${CMAKE_CURRENT_BINARY_DIR}/run_install_test.cmake" @ONLY)
add_test(NAME install.consumer
         COMMAND "${CMAKE_COMMAND}" -P
                 "${CMAKE_CURRENT_BINARY_DIR}/run_install_test.cmake")
set_tests_properties(install.consumer PROPERTIES LABELS install RUN_SERIAL TRUE)

add_executable(cxxlens-unit-runtime-ports unit/runtime/runtime_ports_test.cpp)
target_link_libraries(cxxlens-unit-runtime-ports PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-runtime-ports
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-runtime-ports PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-runtime-ports)
add_test(NAME unit.runtime-ports COMMAND cxxlens-unit-runtime-ports)
set_tests_properties(unit.runtime-ports
                     PROPERTIES LABELS "unit;runtime;security;concurrency")

add_executable(cxxlens-unit-source-span unit/source/source_span_test.cpp)
target_link_libraries(cxxlens-unit-source-span PRIVATE cxxlens::cxxlens)
target_compile_definitions(
  cxxlens-unit-source-span
  PRIVATE
    CXXLENS_NESTED_SOURCE_GOLDEN="${PROJECT_SOURCE_DIR}/tests/golden/source/nested_macro.json"
)
target_compile_features(cxxlens-unit-source-span PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-source-span)
add_test(NAME unit.source-span COMMAND cxxlens-unit-source-span)
set_tests_properties(unit.source-span PROPERTIES LABELS "unit;source;schema")

add_test(
  NAME schema.source-span
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/schema/test_source_schema.py"
    "${PROJECT_SOURCE_DIR}")
set_tests_properties(schema.source-span PROPERTIES LABELS "source;schema")

add_executable(cxxlens-unit-canonical-identity
               unit/identity/canonical_identity_test.cpp)
target_link_libraries(cxxlens-unit-canonical-identity PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-canonical-identity
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-canonical-identity PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-canonical-identity)
add_test(NAME unit.canonical-identity COMMAND cxxlens-unit-canonical-identity)
set_tests_properties(unit.canonical-identity PROPERTIES LABELS "unit;identity")
add_test(
  NAME determinism.identity-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/identity/test_identity_process.py"
    "$<TARGET_FILE:cxxlens-unit-canonical-identity>"
    "${PROJECT_SOURCE_DIR}/tests/golden/identity/canonical_v1.txt"
    "${PROJECT_SOURCE_DIR}/schemas/cxxlens_identity_contract.yaml")
set_tests_properties(determinism.identity-process
                     PROPERTIES LABELS "identity;determinism;schema")

add_executable(cxxlens-unit-failure-contract
               unit/core/failure_contract_test.cpp)
target_link_libraries(cxxlens-unit-failure-contract PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-failure-contract PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-failure-contract)
add_test(NAME unit.failure-contract COMMAND cxxlens-unit-failure-contract)
set_tests_properties(unit.failure-contract
                     PROPERTIES LABELS "unit;failure;schema;conformance")

add_executable(cxxlens-unit-evidence-coverage
               unit/core/evidence_coverage_test.cpp)
target_link_libraries(cxxlens-unit-evidence-coverage PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-evidence-coverage PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-evidence-coverage)
add_test(NAME unit.evidence-coverage COMMAND cxxlens-unit-evidence-coverage)
set_tests_properties(
  unit.evidence-coverage
  PROPERTIES LABELS "unit;evidence;coverage;schema;determinism;conformance")
add_test(
  NAME determinism.evidence-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/evidence/test_evidence_process.py"
    "$<TARGET_FILE:cxxlens-unit-evidence-coverage>"
    "${PROJECT_SOURCE_DIR}/tests/golden/evidence_coverage/canonical_v1.txt")
set_tests_properties(determinism.evidence-process
                     PROPERTIES LABELS "evidence;coverage;determinism;golden")

add_executable(cxxlens-unit-finding-contract
               unit/core/finding_contract_test.cpp)
target_link_libraries(cxxlens-unit-finding-contract PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-finding-contract
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-finding-contract PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-finding-contract)
add_test(NAME unit.finding-contract COMMAND cxxlens-unit-finding-contract)
set_tests_properties(unit.finding-contract
                     PROPERTIES LABELS "unit;finding;determinism;conformance")
add_test(
  NAME determinism.finding-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/finding/test_finding_process.py"
    "$<TARGET_FILE:cxxlens-unit-finding-contract>"
    "${PROJECT_SOURCE_DIR}/tests/golden/finding/canonical_v1.txt")
set_tests_properties(determinism.finding-process
                     PROPERTIES LABELS "finding;determinism;golden")

add_executable(cxxlens-unit-canonical-json unit/core/canonical_json_test.cpp)
target_link_libraries(cxxlens-unit-canonical-json PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-canonical-json
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-canonical-json PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-canonical-json)
add_test(NAME unit.canonical-json COMMAND cxxlens-unit-canonical-json)
set_tests_properties(unit.canonical-json
                     PROPERTIES LABELS "unit;json;schema;security;determinism")

add_executable(cxxlens-unit-schema-registry unit/core/schema_registry_test.cpp)
target_link_libraries(cxxlens-unit-schema-registry PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-schema-registry PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-schema-registry)
add_test(NAME unit.schema-registry COMMAND cxxlens-unit-schema-registry)
set_tests_properties(unit.schema-registry
                     PROPERTIES LABELS "unit;schema;compatibility")

add_executable(cxxlens-unit-m0-serialization
               unit/core/m0_serialization_test.cpp)
target_link_libraries(cxxlens-unit-m0-serialization PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-m0-serialization PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-m0-serialization)
add_test(
  NAME schema.m0-serialization
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/schema/test_m0_serialization.py"
    "$<TARGET_FILE:cxxlens-unit-m0-serialization>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.m0-serialization PROPERTIES LABELS
                                     "schema;json;golden;determinism;security")

add_executable(cxxlens-unit-configuration unit/config/configuration_test.cpp)
target_link_libraries(cxxlens-unit-configuration PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-configuration
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-configuration PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-configuration)
add_test(NAME unit.configuration COMMAND cxxlens-unit-configuration)
set_tests_properties(unit.configuration
                     PROPERTIES LABELS "unit;config;security;determinism")

add_executable(cxxlens-unit-configuration-process
               unit/config/configuration_process_test.cpp)
target_link_libraries(cxxlens-unit-configuration-process
                      PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-configuration-process PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-configuration-process)
add_test(
  NAME schema.configuration
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/config/test_configuration_process.py"
    "$<TARGET_FILE:cxxlens-unit-configuration-process>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.configuration PROPERTIES LABELS
                                  "config;schema;golden;determinism;security")

add_executable(cxxlens-unit-testing-substrate
               unit/testing/testing_substrate_test.cpp)
target_link_libraries(cxxlens-unit-testing-substrate PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-testing-substrate
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-testing-substrate PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-testing-substrate)
add_test(NAME unit.testing-substrate COMMAND cxxlens-unit-testing-substrate)
set_tests_properties(
  unit.testing-substrate
  PROPERTIES LABELS "unit;testing;property;schema;golden;determinism;security")

add_executable(cxxlens-unit-testing-fixture-process
               unit/testing/testing_fixture_process_test.cpp)
target_link_libraries(cxxlens-unit-testing-fixture-process
                      PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-testing-fixture-process PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-testing-fixture-process)
add_test(
  NAME schema.testing-fixture
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/testing/test_testing_fixture_process.py"
    "$<TARGET_FILE:cxxlens-unit-testing-fixture-process>"
    "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.testing-fixture
  PROPERTIES LABELS "testing;fixture;schema;golden;determinism;security")

add_test(
  NAME fixtures.m0-discovery
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/support/discover_fixtures.py"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/m0")
set_tests_properties(fixtures.m0-discovery
                     PROPERTIES LABELS "testing;fixture;discovery")

add_executable(cxxlens-unit-workspace-catalog
               unit/workspace/workspace_catalog_test.cpp)
target_link_libraries(cxxlens-unit-workspace-catalog PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-workspace-catalog PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-workspace-catalog)
add_test(NAME unit.workspace-catalog COMMAND cxxlens-unit-workspace-catalog)
set_tests_properties(
  unit.workspace-catalog
  PROPERTIES LABELS "unit;workspace;schema;determinism;security;conformance")
add_test(
  NAME schema.workspace-catalog
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/workspace/test_workspace_process.py"
    "$<TARGET_FILE:cxxlens-unit-workspace-catalog>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.workspace-catalog
  PROPERTIES LABELS "workspace;schema;golden;determinism;security")

add_executable(cxxlens-unit-fact-contract unit/facts/fact_contract_test.cpp)
target_link_libraries(cxxlens-unit-fact-contract PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-fact-contract
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-fact-contract PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-fact-contract)
add_test(NAME unit.fact-contract COMMAND cxxlens-unit-fact-contract)
set_tests_properties(
  unit.fact-contract
  PROPERTIES LABELS "unit;facts;schema;identity;determinism;conformance")
add_test(
  NAME schema.fact-contract
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/facts/test_fact_schemas.py"
    "$<TARGET_FILE:cxxlens-unit-fact-contract>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.fact-contract
  PROPERTIES LABELS "facts;schema;golden;determinism;security;conformance")

add_executable(cxxlens-unit-fact-reducer unit/facts/reducer/reducer_test.cpp)
target_link_libraries(cxxlens-unit-fact-reducer PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-fact-reducer
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-fact-reducer PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-fact-reducer)
add_test(NAME unit.fact-reducer COMMAND cxxlens-unit-fact-reducer)
set_tests_properties(
  unit.fact-reducer
  PROPERTIES
    LABELS
    "unit;facts;reducer;identity;variant;coverage;determinism;conformance")

add_executable(cxxlens-unit-fact-store unit/store/fact_store_test.cpp)
target_link_libraries(cxxlens-unit-fact-store PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-fact-store
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(
  cxxlens-unit-fact-store
  PRIVATE CXXLENS_STORE_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(cxxlens-unit-fact-store PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-fact-store)
add_test(NAME unit.fact-store COMMAND cxxlens-unit-fact-store)
set_tests_properties(
  unit.fact-store
  PROPERTIES
    LABELS
    "unit;facts;store;sqlite;transaction;coverage;determinism;conformance;concurrency"
)
add_test(
  NAME determinism.fact-store-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/store/test_fact_store_process.py"
    "$<TARGET_FILE:cxxlens-unit-fact-store>"
    "${PROJECT_SOURCE_DIR}/tests/golden/store/snapshot_binary_v1.txt")
set_tests_properties(
  determinism.fact-store-process
  PROPERTIES LABELS "facts;store;sqlite;binary;golden;determinism;conformance")
add_test(
  NAME determinism.fact-reducer-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/facts/reducer/test_reducer_process.py"
    "$<TARGET_FILE:cxxlens-unit-fact-reducer>"
    "${PROJECT_SOURCE_DIR}/tests/golden/facts/reducer_v1.json"
    "${PROJECT_SOURCE_DIR}/schemas/cxxlens_reduction_trace.schema.yaml")
set_tests_properties(
  determinism.fact-reducer-process
  PROPERTIES LABELS "facts;reducer;schema;golden;determinism;coverage")

add_executable(cxxlens-unit-frontend-job unit/llvm/frontend_job_test.cpp)
target_link_libraries(cxxlens-unit-frontend-job PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-frontend-job
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-frontend-job PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-frontend-job)
add_test(NAME unit.frontend-job COMMAND cxxlens-unit-frontend-job)
set_tests_properties(
  unit.frontend-job
  PROPERTIES LABELS
             "unit;frontend;interop;lifetime;determinism;security;conformance")
add_test(
  NAME schema.frontend-batch
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/frontend/test_frontend_schema.py"
    "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.frontend-batch
  PROPERTIES LABELS "frontend;interop;schema;security;conformance")

add_executable(cxxlens-frontend-worker-fixture
               fixtures/frontend_worker_fixture.cpp)
target_link_libraries(cxxlens-frontend-worker-fixture PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-frontend-worker-fixture
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-frontend-worker-fixture PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-frontend-worker-fixture)

add_executable(cxxlens-unit-scheduler unit/workspace/scheduler_test.cpp)
target_link_libraries(cxxlens-unit-scheduler PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-scheduler
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-scheduler PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-scheduler)
add_dependencies(cxxlens-unit-scheduler cxxlens-frontend-worker
                 cxxlens-frontend-worker-fixture)
add_test(NAME unit.scheduler
         COMMAND cxxlens-unit-scheduler
                 "$<TARGET_FILE:cxxlens-frontend-worker-fixture>")
set_tests_properties(
  unit.scheduler
  PROPERTIES LABELS
             "unit;scheduler;determinism;concurrency;cancellation;coverage")
add_test(
  NAME determinism.scheduler-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/scheduler/test_scheduler_process.py"
    "$<TARGET_FILE:cxxlens-unit-scheduler>" "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests/golden/scheduler/canonical_v1.json")
set_tests_properties(
  determinism.scheduler-process
  PROPERTIES LABELS "scheduler;determinism;schema;golden;concurrency;coverage")

add_executable(cxxlens-unit-provisioning unit/workspace/provisioning_test.cpp)
target_link_libraries(cxxlens-unit-provisioning PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-provisioning
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-provisioning PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-provisioning)
add_test(NAME unit.provisioning COMMAND cxxlens-unit-provisioning)
set_tests_properties(
  unit.provisioning
  PROPERTIES
    LABELS
    "unit;workspace;facts;coverage;incremental;sqlite;determinism;cancellation;capability;doctor;conformance;concurrency"
)
add_test(
  NAME schema.provisioning
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/workspace/test_provisioning_contract.py"
    "$<TARGET_FILE:cxxlens-unit-provisioning>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(
  schema.provisioning
  PROPERTIES
    LABELS
    "schema;workspace;facts;coverage;incremental;capability;doctor;conformance")

add_executable(cxxlens-unit-query-engine unit/query/query_engine_test.cpp)
target_link_libraries(cxxlens-unit-query-engine PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-query-engine
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(
  cxxlens-unit-query-engine
  PRIVATE CXXLENS_QUERY_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(cxxlens-unit-query-engine PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-query-engine)
add_test(NAME unit.query-engine COMMAND cxxlens-unit-query-engine)
set_tests_properties(
  unit.query-engine
  PROPERTIES
    LABELS
    "unit;query;planner;executor;graph;variant;coverage;budget;cancellation;determinism;conformance"
)

add_executable(cxxlens-query-process unit/query/query_process.cpp)
target_link_libraries(cxxlens-query-process PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-query-process
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-query-process PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-query-process)
add_test(
  NAME schema.query-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/query/test_query_process.py"
    "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:cxxlens-query-process>")
set_tests_properties(
  schema.query-process
  PROPERTIES LABELS
             "query;planner;executor;graph;schema;golden;determinism;coverage")

add_executable(cxxlens-unit-search unit/search/search_test.cpp)
target_link_libraries(cxxlens-unit-search PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-unit-search
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-unit-search PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-search)
add_test(NAME unit.search COMMAND cxxlens-unit-search)
set_tests_properties(
  unit.search
  PROPERTIES
    LABELS
    "unit;search;explain;report;schema;coverage;variant;determinism;conformance"
)

add_executable(cxxlens-search-process unit/search/search_process.cpp)
target_link_libraries(cxxlens-search-process PRIVATE cxxlens::cxxlens)
target_include_directories(cxxlens-search-process
                           PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(cxxlens-search-process PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-search-process)
add_test(
  NAME schema.search-process
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/search/test_search_process.py"
    "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:cxxlens-search-process>")
set_tests_properties(
  schema.search-process
  PROPERTIES LABELS
             "search;explain;report;schema;golden;determinism;coverage;variant")

if(CXXLENS_CLANG22_AVAILABLE)
  add_executable(cxxlens-m2-search-conformance
                 integration/m2_search_conformance_test.cpp)
  target_link_libraries(cxxlens-m2-search-conformance PRIVATE cxxlens::cxxlens)
  target_include_directories(cxxlens-m2-search-conformance
                             PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_features(cxxlens-m2-search-conformance PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-m2-search-conformance)
  add_test(NAME integration.m2-search-conformance
           COMMAND cxxlens-m2-search-conformance)
  set_tests_properties(
    integration.m2-search-conformance
    PROPERTIES
      LABELS
      "m2;integration;search;explain;frontend;facts;macro;variant;coverage;determinism;conformance"
  )

  add_executable(cxxlens-m1-conformance integration/m1_conformance_test.cpp)
  target_link_libraries(cxxlens-m1-conformance PRIVATE cxxlens::cxxlens)
  target_compile_features(cxxlens-m1-conformance PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-m1-conformance)
  add_test(NAME integration.m1-conformance COMMAND cxxlens-m1-conformance)
  set_tests_properties(
    integration.m1-conformance
    PROPERTIES
      LABELS
      "m1;integration;workspace;frontend;facts;store;interop;macro;variant;determinism;coverage;conformance"
  )

  add_executable(cxxlens-clang22-semantic
                 adapter/clang22/semantic_extractor_test.cpp)
  target_link_libraries(cxxlens-clang22-semantic PRIVATE cxxlens::cxxlens)
  target_include_directories(cxxlens-clang22-semantic
                             PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_features(cxxlens-clang22-semantic PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang22-semantic)
  add_test(NAME adapter.clang22-semantic COMMAND cxxlens-clang22-semantic)
  set_tests_properties(
    adapter.clang22-semantic
    PROPERTIES
      LABELS
      "adapter;frontend;symbol;type;call;inheritance;override;schema;determinism;coverage"
  )
  add_test(
    NAME determinism.clang22-semantic-process
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/adapter/clang22/test_semantic_process.py"
      "$<TARGET_FILE:cxxlens-clang22-semantic>"
      "${PROJECT_SOURCE_DIR}/tests/golden/clang22/semantic_v1.txt")
  set_tests_properties(
    determinism.clang22-semantic-process
    PROPERTIES
      LABELS
      "adapter;symbol;type;call;inheritance;override;schema;golden;determinism")

  add_executable(cxxlens-clang22-preprocessor
                 adapter/clang22/preprocessor_extractor_test.cpp)
  target_link_libraries(cxxlens-clang22-preprocessor PRIVATE cxxlens::cxxlens)
  target_include_directories(cxxlens-clang22-preprocessor
                             PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_features(cxxlens-clang22-preprocessor PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang22-preprocessor)
  add_test(NAME adapter.clang22-preprocessor
           COMMAND cxxlens-clang22-preprocessor)
  set_tests_properties(
    adapter.clang22-preprocessor
    PROPERTIES
      LABELS
      "adapter;frontend;source;macro;include;schema;determinism;coverage")
  add_test(
    NAME determinism.clang22-preprocessor-process
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/adapter/clang22/test_preprocessor_process.py"
      "$<TARGET_FILE:cxxlens-clang22-preprocessor>" "${PROJECT_SOURCE_DIR}"
      "${PROJECT_SOURCE_DIR}/tests/golden/clang22/preprocessor_v1.txt")
  set_tests_properties(
    determinism.clang22-preprocessor-process
    PROPERTIES LABELS "adapter;source;macro;include;schema;golden;determinism")

  add_executable(cxxlens-clang22-link-closure
                 link/clang22_link_closure_test.cpp)
  target_link_libraries(cxxlens-clang22-link-closure PRIVATE cxxlens::cxxlens)
  target_compile_features(cxxlens-clang22-link-closure PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-clang22-link-closure)
  add_test(NAME link.clang22-closure COMMAND cxxlens-clang22-link-closure)
  set_tests_properties(link.clang22-closure PROPERTIES LABELS
                                                       "frontend;interop;link")

  add_custom_target(cxxlens-m1-test-binaries)
  add_dependencies(
    cxxlens-m1-test-binaries
    cxxlens-clang22-link-closure
    cxxlens-clang22-preprocessor
    cxxlens-clang22-semantic
    cxxlens-m1-conformance
    cxxlens-public-header-cxxlens
    cxxlens-public-header-facts
    cxxlens-public-header-interop-clang
    cxxlens-public-header-testing
    cxxlens-public-header-workspace
    cxxlens-unit-fact-contract
    cxxlens-unit-fact-reducer
    cxxlens-unit-fact-store
    cxxlens-unit-frontend-job
    cxxlens-unit-provisioning
    cxxlens-unit-scheduler
    cxxlens-unit-testing-substrate
    cxxlens-unit-workspace-catalog)

  add_custom_target(cxxlens-m2-test-binaries)
  add_dependencies(
    cxxlens-m2-test-binaries
    cxxlens-m2-search-conformance
    cxxlens-public-header-cxxlens
    cxxlens-public-header-explain
    cxxlens-public-header-search
    cxxlens-query-process
    cxxlens-search-process
    cxxlens-unit-provisioning
    cxxlens-unit-query-engine
    cxxlens-unit-scheduler
    cxxlens-unit-search)
endif()

add_custom_target(cxxlens-m0-test-binaries)
add_dependencies(
  cxxlens-m0-test-binaries
  cxxlens-frontend-worker
  cxxlens-public-header-configuration
  cxxlens-public-header-core
  cxxlens-public-header-cxxlens
  cxxlens-public-header-evidence
  cxxlens-public-header-explain
  cxxlens-public-header-facts
  cxxlens-public-header-failure
  cxxlens-public-header-finding
  cxxlens-public-header-identity
  cxxlens-public-header-interop-clang
  cxxlens-public-header-schema
  cxxlens-public-header-search
  cxxlens-public-header-select
  cxxlens-public-header-source
  cxxlens-public-header-testing
  cxxlens-public-header-workspace
  cxxlens-query-process
  cxxlens-search-process
  cxxlens-selector-process
  cxxlens-unit-canonical-identity
  cxxlens-unit-canonical-json
  cxxlens-unit-configuration
  cxxlens-unit-configuration-process
  cxxlens-unit-evidence-coverage
  cxxlens-unit-fact-contract
  cxxlens-unit-fact-reducer
  cxxlens-unit-fact-store
  cxxlens-unit-failure-contract
  cxxlens-unit-finding-contract
  cxxlens-unit-frontend-job
  cxxlens-unit-m0-serialization
  cxxlens-unit-provisioning
  cxxlens-unit-query-engine
  cxxlens-unit-runtime-ports
  cxxlens-unit-scheduler
  cxxlens-unit-schema-registry
  cxxlens-unit-search
  cxxlens-unit-selectors
  cxxlens-unit-source-span
  cxxlens-unit-testing-fixture-process
  cxxlens-unit-testing-substrate
  cxxlens-unit-version)
add_dependencies(cxxlens-m0-test-binaries cxxlens-unit-workspace-catalog)

if(TARGET cxxlens-m1-test-binaries)
  add_dependencies(cxxlens-m1-test-binaries cxxlens-m0-test-binaries)
endif()
if(TARGET cxxlens-m2-test-binaries)
  add_dependencies(cxxlens-m2-test-binaries cxxlens-m1-test-binaries)
endif()

if(CXXLENS_ENABLE_TSAN)
  add_executable(cxxlens-tsan-race-fixture fixtures/tsan_race_fixture.cpp)
  target_compile_features(cxxlens-tsan-race-fixture PRIVATE cxx_std_23)
  cxxlens_set_project_warnings(cxxlens-tsan-race-fixture)
  add_test(
    NAME sanitizer.tsan-detection
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/tsan/verify_tsan_detection.py"
      "$<TARGET_FILE:cxxlens-tsan-race-fixture>"
      "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/tsan-detection.log")
  set_tests_properties(
    sanitizer.tsan-detection
    PROPERTIES
      LABELS
      "sanitizer;tsan;tsan-self-test"
      ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/tsan/tsan.supp:halt_on_error=1:exitcode=66:handle_segv=0"
  )
endif()

get_property(
  cxxlens_test_targets
  DIRECTORY
  PROPERTY BUILDSYSTEM_TARGETS)
foreach(cxxlens_test_target IN LISTS cxxlens_test_targets)
  get_target_property(cxxlens_test_target_type ${cxxlens_test_target} TYPE)
  if(cxxlens_test_target_type STREQUAL "EXECUTABLE")
    cxxlens_enable_sanitizers(${cxxlens_test_target})
  endif()
endforeach()

get_property(
  cxxlens_registered_tests
  DIRECTORY
  PROPERTY TESTS)
set_tests_properties(
  ${cxxlens_registered_tests}
  PROPERTIES ENVIRONMENT_MODIFICATION
             "PATH=path_list_prepend:${PROJECT_BINARY_DIR}")
add_test(
  NAME contract.phase-b-integration-metadata
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/quality/check_phase_b_contract_integration.py"
    --root "${PROJECT_SOURCE_DIR}" --metadata-only)
set_tests_properties(contract.phase-b-integration-metadata
                     PROPERTIES LABELS "contract;phase-b;integration")
