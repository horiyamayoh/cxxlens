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
