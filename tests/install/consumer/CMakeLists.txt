cmake_minimum_required(VERSION 3.25)
project(cxxlens_consumer LANGUAGES CXX)

find_package(cxxlens 0.1 CONFIG REQUIRED)

add_executable(cxxlens-consumer main.cpp)
target_link_libraries(cxxlens-consumer PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-consumer PRIVATE cxx_std_23)
