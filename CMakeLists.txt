cmake_minimum_required(VERSION 3.25)

project(
  cxxlens
  VERSION 0.1.0
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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CxxlensWarnings)
include(CxxlensSanitizers)
include(CxxlensClangTargets)

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

add_library(cxxlens_base ${CXXLENS_LIBRARY_TYPE} src/sdk/common.cpp)
add_library(cxxlens::base ALIAS cxxlens_base)
cxxlens_configure_component(cxxlens_base base)

add_library(cxxlens_kernel ${CXXLENS_LIBRARY_TYPE} src/sdk/relation.cpp
                           src/sdk/claim.cpp src/sdk/store.cpp)
add_library(cxxlens::kernel ALIAS cxxlens_kernel)
cxxlens_configure_component(cxxlens_kernel kernel)
target_link_libraries(
  cxxlens_kernel
  PUBLIC cxxlens::base
  PRIVATE "${CMAKE_DL_LIBS}")

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

add_library(
  cxxlens_provider_sdk
  ${CXXLENS_LIBRARY_TYPE} src/sdk/provider.cpp
  src/runtime/provider_process_adapter.cpp src/sdk/provider_runtime.cpp
  src/sdk/testing.cpp)
add_library(cxxlens::provider_sdk ALIAS cxxlens_provider_sdk)
cxxlens_configure_component(cxxlens_provider_sdk provider_sdk)
target_link_libraries(cxxlens_provider_sdk PUBLIC cxxlens::cxxlens
                                                  cxxlens::recipes)

add_library(
  cxxlens_clang22_provider_sdk
  ${CXXLENS_LIBRARY_TYPE} src/llvm/clang22/provider_sdk.cpp
  src/llvm/clang22/provider_worker.cpp)
add_library(cxxlens::clang22_provider_sdk ALIAS cxxlens_clang22_provider_sdk)
cxxlens_configure_component(cxxlens_clang22_provider_sdk clang22_provider_sdk)
target_link_libraries(cxxlens_clang22_provider_sdk PUBLIC cxxlens::provider_sdk)
target_include_directories(cxxlens_clang22_provider_sdk
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
cxxlens_configure_clang22(cxxlens_clang22_provider_sdk)

add_executable(cxxlens-provider-scaffold tools/sdk/provider_scaffold_main.cpp)
target_link_libraries(cxxlens-provider-scaffold PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-provider-scaffold PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-provider-scaffold)
cxxlens_enable_sanitizers(cxxlens-provider-scaffold)
cxxlens_configure_installed_tool(cxxlens-provider-scaffold)

add_executable(cxxlens-sdk-doctor tools/sdk/sdk_doctor_main.cpp)
target_link_libraries(cxxlens-sdk-doctor PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-sdk-doctor PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-sdk-doctor)
cxxlens_enable_sanitizers(cxxlens-sdk-doctor)
cxxlens_configure_installed_tool(cxxlens-sdk-doctor)

add_executable(cxxlens-clang-worker-22
               src/llvm/clang22/provider_worker_main.cpp)
target_link_libraries(cxxlens-clang-worker-22
                      PRIVATE cxxlens::clang22_provider_sdk)
target_include_directories(cxxlens-clang-worker-22
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(cxxlens-clang-worker-22 PRIVATE cxx_std_23)
cxxlens_set_project_warnings(cxxlens-clang-worker-22)
cxxlens_enable_sanitizers(cxxlens-clang-worker-22)
cxxlens_configure_installed_tool(cxxlens-clang-worker-22)

set(CXXLENS_COMPILED_TARGETS
    cxxlens_base cxxlens_kernel cxxlens_query cxxlens_recipes
    cxxlens_provider_sdk cxxlens_clang22_provider_sdk)

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

if(CXXLENS_BUILD_QUALITY_TOOLS OR CXXLENS_BUILD_DOCS)
  include(CxxlensDeveloperTools)
endif()

install(
  TARGETS cxxlens_base cxxlens_kernel cxxlens_query cxxlens_cpp cxxlens_recipes
          cxxlens
  EXPORT cxxlensTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  TARGETS cxxlens_provider_sdk
  EXPORT cxxlensProviderSDKTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  TARGETS cxxlens_clang22_provider_sdk
  EXPORT cxxlensClang22ProviderSDKTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS cxxlens-provider-scaffold cxxlens-sdk-doctor
                cxxlens-clang-worker-22
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  PROGRAMS tools/sdk/relation_idl_compiler.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  RENAME cxxlens-relation-idl)

install(FILES include/cxxlens/cxxlens.hpp include/cxxlens/sdk.hpp
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxxlens")
install(DIRECTORY include/cxxlens/sdk include/cxxlens/provider
                  include/cxxlens/relations
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxxlens")
install(
  DIRECTORY schemas/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxlens/schemas"
  FILES_MATCHING
  PATTERN "cxxlens_ng_*")
# Explicit runtime authority evidence used by the provider-runtime contract
# gate.
install(
  FILES schemas/cxxlens_ng_provider_runtime_contract.yaml
        schemas/cxxlens_ng_provider_runtime_contract.schema.yaml
        schemas/cxxlens_ng_provider_execution_report.schema.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxlens/schemas")
install(FILES README.md SECURITY.md
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

configure_package_config_file(
  cmake/cxxlensProviderSDKConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(
  EXPORT cxxlensProviderSDKTargets
  FILE cxxlensProviderSDKTargets.cmake
  NAMESPACE cxxlens::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cxxlensProviderSDKConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensProviderSDK")

configure_package_config_file(
  cmake/cxxlensClang22ProviderSDKConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cxxlensClang22ProviderSDKConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxxlensClang22ProviderSDK")
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
