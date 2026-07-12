cmake_minimum_required(VERSION 3.25)
project(cxxlens_m2_flagship LANGUAGES CXX)

find_package(cxxlens 0.1 CONFIG REQUIRED)

add_executable(cxxlens-m2-flagship main.cpp)
target_link_libraries(cxxlens-m2-flagship PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-m2-flagship PRIVATE cxx_std_23)
