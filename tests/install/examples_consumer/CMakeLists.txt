cmake_minimum_required(VERSION 3.25)
project(cxxlens_installed_examples LANGUAGES CXX)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(cxxlensProviderSDK 0.1 CONFIG REQUIRED)
find_package(cxxlensClang22ProviderSDK 0.1 CONFIG REQUIRED)

if(NOT IS_DIRECTORY "${CXXLENS_EXAMPLE_ROOT}")
  message(FATAL_ERROR "installed SDK example root is missing")
endif()

foreach(example IN ITEMS recipe typed_query dynamic_query portable_provider)
  add_executable("cxxlens-installed-${example}"
                 "${CXXLENS_EXAMPLE_ROOT}/${example}.cpp")
  target_link_libraries("cxxlens-installed-${example}"
                        PRIVATE cxxlens::provider_sdk)
  target_compile_features("cxxlens-installed-${example}" PRIVATE cxx_std_23)
  install(TARGETS "cxxlens-installed-${example}" RUNTIME DESTINATION bin)
endforeach()

add_executable(cxxlens-installed-clang22-native
               "${CXXLENS_EXAMPLE_ROOT}/clang22_native_provider.cpp")
target_link_libraries(cxxlens-installed-clang22-native
                      PRIVATE cxxlens::clang22_provider_sdk)
target_compile_features(cxxlens-installed-clang22-native PRIVATE cxx_std_23)
install(TARGETS cxxlens-installed-clang22-native RUNTIME DESTINATION bin)
