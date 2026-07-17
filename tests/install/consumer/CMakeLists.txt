cmake_minimum_required(VERSION 3.25)
project(cxxlens_consumer LANGUAGES CXX)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
include("${CMAKE_CURRENT_LIST_DIR}/../consumer_sanitizers.cmake")

find_package(cxxlens 0.1 CONFIG REQUIRED)

add_executable(cxxlens-consumer main.cpp)
target_link_libraries(cxxlens-consumer PRIVATE cxxlens::cxxlens)
target_compile_features(cxxlens-consumer PRIVATE cxx_std_23)
cxxlens_enable_consumer_sanitizers(cxxlens-consumer)
install(TARGETS cxxlens-consumer RUNTIME DESTINATION bin)
