cmake_minimum_required(VERSION 3.25)
project(cxxlens_installed_examples LANGUAGES CXX)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
include("${CMAKE_CURRENT_LIST_DIR}/../consumer_sanitizers.cmake")

find_package(cxxlensProviderSDK 1.0 CONFIG REQUIRED)
find_package(cxxlensClang22ProviderSDK 1.0 CONFIG REQUIRED)

if(NOT IS_DIRECTORY "${CXXLENS_EXAMPLE_ROOT}")
  message(FATAL_ERROR "installed SDK example root is missing")
endif()

foreach(example IN ITEMS recipe typed_query dynamic_query portable_provider)
  add_executable("cxxlens-installed-${example}"
                 "${CXXLENS_EXAMPLE_ROOT}/${example}.cpp")
  target_link_libraries("cxxlens-installed-${example}"
                        PRIVATE cxxlens::provider_sdk)
  target_compile_features("cxxlens-installed-${example}" PRIVATE cxx_std_23)
  cxxlens_enable_consumer_sanitizers("cxxlens-installed-${example}")
  install(TARGETS "cxxlens-installed-${example}" RUNTIME DESTINATION bin)
endforeach()

add_executable(cxxlens-installed-clang22-native
               "${CXXLENS_EXAMPLE_ROOT}/clang22_native_provider.cpp")
target_link_libraries(cxxlens-installed-clang22-native
                      PRIVATE cxxlens::clang22_provider_sdk)
target_compile_features(cxxlens-installed-clang22-native PRIVATE cxx_std_23)
cxxlens_enable_consumer_sanitizers(cxxlens-installed-clang22-native)
install(TARGETS cxxlens-installed-clang22-native RUNTIME DESTINATION bin)
