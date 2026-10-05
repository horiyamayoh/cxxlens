# Original LLVM source components are a private native compiler dependency. The
# public provider SDK, including its shared compiler boundary, stays stock.
set(CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/third_party/llvm22-template-events"
    CACHE PATH "Compatible LLVM 22 original template-event source components")
set(_cxxlens_template_events_available TRUE)
foreach(_source IN ITEMS SemaOverload.cpp SemaTemplate.cpp
                         SemaTemplateInstantiate.cpp ExprConstant.cpp)
  if(NOT EXISTS "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/${_source}")
    set(_cxxlens_template_events_available FALSE)
  endif()
endforeach()
if(CXXLENS_ENABLE_ASAN)
  set(_cxxlens_template_events_available FALSE)
endif()
option(
  CXXLENS_CLANG_TEMPLATE_EVENTS
  "Observe original template candidate and legacy constant-evaluation events"
  ${_cxxlens_template_events_available})

function(cxxlens_configure_original_template_events)
  if(NOT TARGET cxxlens_clang22_worker_core)
    return()
  endif()
  if(NOT CXXLENS_CLANG22_AVAILABLE OR NOT CXXLENS_CLANG_TEMPLATE_EVENTS)
    target_compile_definitions(cxxlens_clang22_worker_core
                               PRIVATE CXXLENS_CLANG_TEMPLATE_EVENTS=0)
    return()
  endif()
  if(CXXLENS_ENABLE_ASAN)
    message(
      FATAL_ERROR
        "Original template events require instrumented static compiler components; the packaged ASan parser uses stock clang-cpp"
    )
  endif()
  # The ordinary adapter discovers these packages in its own function scope.
  # Load the same configured packages here for their original compiler flags.
  find_package(LLVM 22.1.0 EXACT CONFIG REQUIRED)
  find_package(Clang CONFIG REQUIRED)
  if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL "22.1.0")
    message(
      FATAL_ERROR
        "Original template source components require their matching LLVM 22.1.0 interface"
    )
  endif()
  foreach(
    _source IN
    ITEMS SemaOverload.cpp
          SemaTemplate.cpp
          SemaTemplateInstantiate.cpp
          ExprConstant.cpp
          TreeTransform.h
          TypeLocBuilder.h
          CoroutineStmtBuilder.h
          ExprConstShared.h
          ByteCode/Context.h)
    if(NOT EXISTS "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/${_source}")
      message(FATAL_ERROR "Missing compiler source dependency: ${_source}")
    endif()
  endforeach()

  # A shared public package already has an existing private static parser for
  # sealed executables. Resolve it before propagating original compiler objects.
  cxxlens_create_clang22_worker_static_closure()
  add_library(
    cxxlens_clang22_template_compiler_objects OBJECT
    "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/SemaOverload.cpp"
    "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/SemaTemplate.cpp"
    "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/SemaTemplateInstantiate.cpp"
    "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/ExprConstant.cpp")
  target_compile_features(cxxlens_clang22_template_compiler_objects
                          PRIVATE cxx_std_23)
  target_include_directories(cxxlens_clang22_template_compiler_objects SYSTEM
                             PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_include_directories(
    cxxlens_clang22_template_compiler_objects
    PRIVATE "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}")
  set_target_properties(cxxlens_clang22_template_compiler_objects
                        PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT LLVM_ENABLE_ASSERTIONS)
    target_compile_definitions(cxxlens_clang22_template_compiler_objects
                               PRIVATE NDEBUG)
  endif()
  if(NOT LLVM_ENABLE_RTTI)
    if(MSVC)
      target_compile_options(cxxlens_clang22_template_compiler_objects
                             PRIVATE /GR-)
    else()
      target_compile_options(cxxlens_clang22_template_compiler_objects
                             PRIVATE -fno-rtti)
    endif()
  endif()
  if(NOT LLVM_ENABLE_EH)
    if(MSVC)
      target_compile_options(cxxlens_clang22_template_compiler_objects
                             PRIVATE /EHs-c-)
    else()
      target_compile_options(cxxlens_clang22_template_compiler_objects
                             PRIVATE -fno-exceptions)
    endif()
  endif()
  # These large original compiler translation units compile serially with Ninja.
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS
                                      cxxlens_template_compiler_pool=1)
  set_property(TARGET cxxlens_clang22_template_compiler_objects
               PROPERTY JOB_POOL_COMPILE cxxlens_template_compiler_pool)

  # TARGET_OBJECTS puts each object before all libraries on every consuming
  # executable's link line. An analyzer-only edge would leave worker/core tests
  # using an instrumented recorder without the matching compiler capability.
  add_library(cxxlens_clang22_template_compiler_closure INTERFACE)
  target_link_libraries(
    cxxlens_clang22_template_compiler_closure
    INTERFACE cxxlens_clang22_template_compiler_objects
              $<TARGET_OBJECTS:cxxlens_clang22_template_compiler_objects>)
  target_link_libraries(cxxlens_clang22_worker_core
                        PUBLIC cxxlens_clang22_template_compiler_closure)
  target_compile_definitions(cxxlens_clang22_worker_core
                             PRIVATE CXXLENS_CLANG_TEMPLATE_EVENTS=1)
  install(
    FILES "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/LICENSE.TXT"
          "${CXXLENS_CLANG_TEMPLATE_EVENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cxxlens/llvm22-template-events")
endfunction()
