if(NOT DEFINED CXXLENS_SOURCE_DIR OR NOT DEFINED CXXLENS_BINARY_DIR)
  message(
    FATAL_ERROR
      "dev-core configure regression requires source and binary directories")
endif()

file(REMOVE_RECURSE "${CXXLENS_BINARY_DIR}")
set(_cxxlens_configure_args
    -S
    "${CXXLENS_SOURCE_DIR}"
    -B
    "${CXXLENS_BINARY_DIR}"
    -G
    Ninja
    -DBUILD_TESTING=OFF
    -DCXXLENS_BUILD_QUALITY_TOOLS=OFF
    -DCXXLENS_BUILD_CLANG22_COMPONENTS=OFF
    -DCXXLENS_CLANG_ADAPTER=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE
    -DCMAKE_DISABLE_FIND_PACKAGE_LLVM=TRUE
    -DCMAKE_DISABLE_FIND_PACKAGE_Clang=TRUE)
if(DEFINED CXXLENS_CXX_COMPILER AND NOT CXXLENS_CXX_COMPILER STREQUAL "")
  list(APPEND _cxxlens_configure_args
       "-DCMAKE_CXX_COMPILER=${CXXLENS_CXX_COMPILER}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" ${_cxxlens_configure_args}
  RESULT_VARIABLE _cxxlens_configure_result
  OUTPUT_VARIABLE _cxxlens_configure_output
  ERROR_VARIABLE _cxxlens_configure_error)
if(NOT _cxxlens_configure_result EQUAL 0)
  message(
    FATAL_ERROR
      "dev-core must configure without Git, LLVM, Clang, or ICU discovery\nstdout: ${_cxxlens_configure_output}\nstderr: ${_cxxlens_configure_error}"
  )
endif()
if(NOT _cxxlens_configure_output MATCHES
   "Clang 22 native components disabled by CXXLENS_BUILD_CLANG22_COMPONENTS=OFF"
)
  message(
    FATAL_ERROR "dev-core configure did not report its disabled native profile")
endif()

file(READ "${CXXLENS_BINARY_DIR}/CMakeCache.txt" _cxxlens_cache)
if(NOT _cxxlens_cache MATCHES "CXXLENS_BUILD_CLANG22_COMPONENTS:BOOL=OFF")
  message(
    FATAL_ERROR "dev-core configure regression did not retain the OFF profile")
endif()
if(_cxxlens_cache MATCHES "CXXLENS_SOURCE_(REVISION|TREE):STRING=[0-9a-f]{40}")
  message(
    FATAL_ERROR "dev-core configure unexpectedly embedded source identity")
endif()
if(_cxxlens_cache MATCHES "CXXLENS_ICU_STATIC_(INCLUDE_DIR|ARCHIVE)")
  message(FATAL_ERROR "dev-core configure unexpectedly discovered static ICU")
endif()

file(READ "${CXXLENS_BINARY_DIR}/build.ninja" _cxxlens_build_graph)
if(_cxxlens_build_graph MATCHES "cxxlens_clang22_materialization_codecs")
  message(
    FATAL_ERROR "dev-core build graph contains native materialization targets")
endif()

message(STATUS "dev-core no-dependency configure regression passed")
