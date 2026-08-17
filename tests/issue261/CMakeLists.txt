cmake_minimum_required(VERSION 3.25)
project(cxxlens_issue_261_clang_vfs LANGUAGES CXX)

if(NOT DEFINED CXXLENS_ROOT)
  message(FATAL_ERROR "CXXLENS_ROOT is required")
endif()

find_package(LLVM 22 REQUIRED CONFIG)
find_package(Clang 22 REQUIRED CONFIG)

add_executable(cxxlens-issue-261-clang-vfs-test
  ${CXXLENS_ROOT}/src/llvm/clang22/source_closure.cpp
  ${CXXLENS_ROOT}/src/llvm/clang22/compiler_vfs.cpp
  ${CXXLENS_ROOT}/src/llvm/clang22/clang_compiler_vfs.cpp
  ${CXXLENS_ROOT}/tests/adapter/clang22/clang_compiler_vfs_test.cpp)

target_compile_features(cxxlens-issue-261-clang-vfs-test PRIVATE cxx_std_23)
target_include_directories(cxxlens-issue-261-clang-vfs-test PRIVATE
  ${CXXLENS_ROOT}/src
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS})
target_compile_options(cxxlens-issue-261-clang-vfs-test PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

if(TARGET clang-cpp)
  target_link_libraries(cxxlens-issue-261-clang-vfs-test PRIVATE clang-cpp LLVM)
else()
  target_link_libraries(cxxlens-issue-261-clang-vfs-test PRIVATE
    clangTooling clangFrontend clangSerialization clangDriver clangParse
    clangSema clangAnalysis clangASTMatchers clangAST clangLex clangBasic LLVM)
endif()
