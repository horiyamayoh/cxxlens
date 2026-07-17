cmake_minimum_required(VERSION 3.25)
project(cxxlens_clang22_sdk_consumer LANGUAGES CXX)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(cxxlensClang22ProviderSDK 0.1 CONFIG REQUIRED)

add_executable(cxxlens-clang22-sdk-consumer main.cpp)
target_link_libraries(cxxlens-clang22-sdk-consumer
                      PRIVATE cxxlens::clang22_provider_sdk)
target_compile_features(cxxlens-clang22-sdk-consumer PRIVATE cxx_std_23)
install(TARGETS cxxlens-clang22-sdk-consumer RUNTIME DESTINATION bin)
