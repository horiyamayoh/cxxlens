# Original LLVM source components are a private native compiler dependency. The
# public provider SDK, including its shared compiler boundary, stays stock.
set(CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/third_party/llvm22-exceptional-events"
    CACHE PATH
          "Compatible LLVM 22 original exceptional-occurrence source components"
)
set(_cxxlens_exceptional_events_available TRUE)
foreach(
  _source IN
  ITEMS CodeGenFunction.cpp
        CGExpr.cpp
        CGExprCXX.cpp
        CGCall.cpp
        CGException.cpp
        CGBuiltin.cpp
        CGDecl.cpp
        CGClass.cpp)
  if(NOT EXISTS "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/${_source}")
    set(_cxxlens_exceptional_events_available FALSE)
  endif()
endforeach()
if(CXXLENS_ENABLE_ASAN)
  set(_cxxlens_exceptional_events_available FALSE)
endif()
option(CXXLENS_CLANG_EXCEPTIONAL_EVENTS
       "Observe original exceptional exit and lowering occurrences"
       ${_cxxlens_exceptional_events_available})

function(cxxlens_configure_original_exceptional_events)
  if(NOT TARGET cxxlens_clang22_worker_core)
    return()
  endif()
  if(NOT CXXLENS_CLANG22_AVAILABLE OR NOT CXXLENS_CLANG_EXCEPTIONAL_EVENTS)
    target_compile_definitions(cxxlens_clang22_worker_core
                               PRIVATE CXXLENS_CLANG_EXCEPTIONAL_EVENTS=0)
    return()
  endif()
  if(CXXLENS_ENABLE_ASAN)
    message(
      FATAL_ERROR
        "Original exceptional occurrences require instrumented static compiler components; the packaged ASan parser uses stock clang-cpp"
    )
  endif()
  # The ordinary adapter discovers these packages in its own function scope.
  # Load the same configured packages here for their original compiler flags.
  find_package(LLVM 22.1.0 EXACT CONFIG REQUIRED)
  find_package(Clang CONFIG REQUIRED)
  if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL "22.1.0")
    message(
      FATAL_ERROR
        "Original exceptional source components require their matching LLVM 22.1.0 interface"
    )
  endif()
  foreach(
    _source IN
    ITEMS CodeGenFunction.cpp
          CGExpr.cpp
          CGExprCXX.cpp
          CGCall.cpp
          CGException.cpp
          CGBuiltin.cpp
          CGDecl.cpp
          CGClass.cpp
          CodeGenFunction.h
          CodeGenModule.h
          CGCall.h
          original_eh_hooks.hpp)
    if(NOT EXISTS "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/${_source}")
      message(FATAL_ERROR "Missing compiler source dependency: ${_source}")
    endif()
  endforeach()

  # A shared public package already has an existing private static parser for
  # sealed executables. Resolve it before propagating original compiler objects.
  cxxlens_create_clang22_worker_static_closure()
  add_library(
    cxxlens_clang22_exceptional_compiler_objects OBJECT
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CodeGenFunction.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGExpr.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGExprCXX.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGCall.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGException.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGBuiltin.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGDecl.cpp"
    "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/CGClass.cpp")
  target_compile_features(cxxlens_clang22_exceptional_compiler_objects
                          PRIVATE cxx_std_23)
  target_include_directories(cxxlens_clang22_exceptional_compiler_objects SYSTEM
                             PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_include_directories(
    cxxlens_clang22_exceptional_compiler_objects
    PRIVATE "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}")
  set_target_properties(cxxlens_clang22_exceptional_compiler_objects
                        PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT LLVM_ENABLE_ASSERTIONS)
    target_compile_definitions(cxxlens_clang22_exceptional_compiler_objects
                               PRIVATE NDEBUG)
  endif()
  if(NOT LLVM_ENABLE_RTTI)
    if(MSVC)
      target_compile_options(cxxlens_clang22_exceptional_compiler_objects
                             PRIVATE /GR-)
    else()
      target_compile_options(cxxlens_clang22_exceptional_compiler_objects
                             PRIVATE -fno-rtti)
    endif()
  endif()
  if(NOT LLVM_ENABLE_EH)
    if(MSVC)
      target_compile_options(cxxlens_clang22_exceptional_compiler_objects
                             PRIVATE /EHs-c-)
    else()
      target_compile_options(cxxlens_clang22_exceptional_compiler_objects
                             PRIVATE -fno-exceptions)
    endif()
  endif()
  # These large original compiler translation units compile serially with Ninja.
  set_property(TARGET cxxlens_clang22_exceptional_compiler_objects
               PROPERTY JOB_POOL_COMPILE cxxlens_original_compiler_pool)

  # TARGET_OBJECTS puts each object before all libraries on every consuming
  # executable's link line. An analyzer-only edge would leave worker/core tests
  # using an instrumented recorder without the matching compiler capability.
  add_library(cxxlens_clang22_exceptional_compiler_closure INTERFACE)
  target_link_libraries(
    cxxlens_clang22_exceptional_compiler_closure
    INTERFACE cxxlens_clang22_exceptional_compiler_objects
              $<TARGET_OBJECTS:cxxlens_clang22_exceptional_compiler_objects>)
  target_link_libraries(cxxlens_clang22_worker_core
                        PUBLIC cxxlens_clang22_exceptional_compiler_closure)
  target_compile_definitions(cxxlens_clang22_worker_core
                             PRIVATE CXXLENS_CLANG_EXCEPTIONAL_EVENTS=1)
  install(
    FILES "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/LICENSE.TXT"
          "${CXXLENS_CLANG_EXCEPTIONAL_EVENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens/llvm22-exceptional-events"
  )
endfunction()
