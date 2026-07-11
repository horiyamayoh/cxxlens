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
