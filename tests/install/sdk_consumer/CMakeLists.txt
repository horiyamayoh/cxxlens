cmake_minimum_required(VERSION 3.25)
project(cxxlens_sdk_consumer LANGUAGES CXX)

find_package(cxxlensProviderSDK 0.1 CONFIG REQUIRED)

add_executable(cxxlens-sdk-consumer main.cpp)
target_link_libraries(cxxlens-sdk-consumer PRIVATE cxxlens::provider_sdk)
target_compile_features(cxxlens-sdk-consumer PRIVATE cxx_std_23)
