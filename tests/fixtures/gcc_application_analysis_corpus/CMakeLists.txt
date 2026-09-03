cmake_minimum_required(VERSION 3.25)

project(cxxlens_gcc_application_analysis_corpus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(gcc_application_model STATIC src/model.cpp src/pipeline.cpp)
target_include_directories(gcc_application_model
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_source_files_properties(src/model.cpp PROPERTIES COMPILE_OPTIONS
                                                     "-MMD;-MF;model.cxxlens.d")
set_source_files_properties(
  src/pipeline.cpp PROPERTIES COMPILE_OPTIONS "-MMD;-MF;pipeline.cxxlens.d")

add_executable(gcc_application_corpus src/main.cpp)
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS
                                                    "-MMD;-MF;main.cxxlens.d")
target_link_libraries(gcc_application_corpus PRIVATE gcc_application_model)
