set(CXXLENS_CLANG_ADAPTER
    "AUTO"
    CACHE STRING "Build the exact Clang 22 adapter: AUTO, ON, or OFF")
set_property(CACHE CXXLENS_CLANG_ADAPTER PROPERTY STRINGS AUTO ON OFF)

function(cxxlens_configure_clang22 target)
  if(NOT CXXLENS_CLANG_ADAPTER MATCHES "^(AUTO|ON|OFF)$")
    message(
      FATAL_ERROR
        "CXXLENS_CLANG_ADAPTER must be AUTO, ON, or OFF (got ${CXXLENS_CLANG_ADAPTER})"
    )
  endif()

  if(CXXLENS_CLANG_ADAPTER STREQUAL "OFF")
    target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=0)
    set(CXXLENS_CLANG22_AVAILABLE
        FALSE
        CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
    return()
  endif()

  find_package(LLVM 22.1 CONFIG QUIET)
  if(LLVM_FOUND AND LLVM_VERSION_MAJOR EQUAL 22)
    get_filename_component(_cxxlens_llvm_cmake_root "${LLVM_DIR}" DIRECTORY)
    find_package(Clang CONFIG QUIET PATHS "${_cxxlens_llvm_cmake_root}/clang"
                 NO_DEFAULT_PATH)
  endif()

  if(NOT LLVM_FOUND
     OR NOT LLVM_VERSION_MAJOR EQUAL 22
     OR NOT Clang_FOUND)
    if(CXXLENS_CLANG_ADAPTER STREQUAL "ON")
      message(
        FATAL_ERROR
          "Exact LLVM/Clang 22 development packages are required when CXXLENS_CLANG_ADAPTER=ON"
      )
    endif()
    message(
      STATUS
        "Exact LLVM/Clang 22 not found; building the structured unavailable adapter"
    )
    target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=0)
    set(CXXLENS_CLANG22_AVAILABLE
        FALSE
        CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
    return()
  endif()

  set(_cxxlens_clang22_components
      LLVMOption
      LLVMSupport
      clangAST
      clangBasic
      clangDriver
      clangFrontend
      clangFrontendTool
      clangLex
      clangOptions
      clangSerialization
      clangTooling
      clangToolingCore)
  foreach(component IN LISTS _cxxlens_clang22_components)
    if(NOT TARGET ${component})
      message(
        FATAL_ERROR "Required explicit Clang 22 target is missing: ${component}"
      )
    endif()
  endforeach()

  target_compile_definitions(${target} PRIVATE CXXLENS_HAS_CLANG22=1)
  target_compile_definitions(
    ${target} PRIVATE __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS
                      __STDC_LIMIT_MACROS)
  target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
                                                      ${CLANG_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE ${_cxxlens_clang22_components})
  set(CXXLENS_CLANG22_EXPLICIT_COMPONENTS
      "${_cxxlens_clang22_components}"
      CACHE INTERNAL "Explicit Clang 22 link closure" FORCE)
  set(CXXLENS_CLANG22_AVAILABLE
      TRUE
      CACHE INTERNAL "Whether the exact Clang 22 adapter is linked" FORCE)
  message(STATUS "Enabled exact LLVM/Clang ${LLVM_PACKAGE_VERSION} adapter")
endfunction()
