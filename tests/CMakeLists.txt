add_executable(cxxlens-unit-version unit/version_test.cpp)
target_link_libraries(cxxlens-unit-version PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-unit-version PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-unit-version)
add_test(NAME unit.version COMMAND cxxlens-unit-version)
set_tests_properties(unit.version PROPERTIES LABELS unit)

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
set_tests_properties(unit.runtime-ports PROPERTIES LABELS
                                                   "unit;runtime;security")

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

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
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
