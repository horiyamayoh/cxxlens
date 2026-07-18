cmake_minimum_required(VERSION 3.25)
project(cxxlens_real_project_consumer LANGUAGES CXX)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
include("${CMAKE_CURRENT_LIST_DIR}/../consumer_sanitizers.cmake")
if(CXXLENS_REQUIRE_INCOMPATIBLE_VERSION)
  find_package(cxxlensProviderSDK 2.0 CONFIG REQUIRED)
  return()
endif()
find_package(cxxlensProviderSDK 1.0 CONFIG REQUIRED)

add_library(real_project_model STATIC model.cpp)
target_link_libraries(real_project_model PUBLIC cxxlens::provider_sdk)
target_include_directories(real_project_model
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(real_project_model PUBLIC cxx_std_23)
cxxlens_enable_consumer_sanitizers(real_project_model)

add_executable(cxxlens-real-project-consumer main.cpp)
target_link_libraries(cxxlens-real-project-consumer PRIVATE real_project_model)
target_compile_features(cxxlens-real-project-consumer PRIVATE cxx_std_23)
cxxlens_enable_consumer_sanitizers(cxxlens-real-project-consumer)
install(TARGETS cxxlens-real-project-consumer RUNTIME DESTINATION bin)
