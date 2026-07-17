function(cxxlens_validate_sanitizer_configuration)
  if(CXXLENS_ENABLE_TSAN AND (CXXLENS_ENABLE_ASAN OR CXXLENS_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer requires a separate build")
  endif()
  if(MSVC AND (CXXLENS_ENABLE_UBSAN OR CXXLENS_ENABLE_TSAN))
    message(
      FATAL_ERROR "This MSVC configuration supports AddressSanitizer only")
  endif()

  set(sanitizers "")
  if(CXXLENS_ENABLE_ASAN)
    list(APPEND sanitizers address)
  endif()
  if(CXXLENS_ENABLE_UBSAN)
    list(APPEND sanitizers undefined)
  endif()
  if(CXXLENS_ENABLE_TSAN)
    list(APPEND sanitizers thread)
  endif()
  list(JOIN sanitizers "," sanitizer_list)
  set(CXXLENS_SANITIZER_EXPECTED
      "${sanitizer_list}"
      CACHE INTERNAL "Expected first-party compile sanitizer set" FORCE)
endfunction()

function(cxxlens_enable_sanitizers target)
  get_target_property(cxxlens_sanitizers_configured ${target}
                      CXXLENS_SANITIZERS_CONFIGURED)
  if(cxxlens_sanitizers_configured)
    return()
  endif()
  set_property(TARGET ${target} PROPERTY CXXLENS_SANITIZERS_CONFIGURED TRUE)

  if(MSVC)
    if(CXXLENS_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_compile_definitions(${target}
                                 PRIVATE CXXLENS_SANITIZER_INSTRUMENTED=1)
      target_link_options(${target} PUBLIC /fsanitize=address)
    endif()
    return()
  endif()

  set(sanitizers "")
  if(CXXLENS_ENABLE_ASAN)
    list(APPEND sanitizers address)
  endif()
  if(CXXLENS_ENABLE_UBSAN)
    list(APPEND sanitizers undefined)
  endif()
  if(CXXLENS_ENABLE_TSAN)
    list(APPEND sanitizers thread)
  endif()
  if(sanitizers)
    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(${target} PRIVATE "-fsanitize=${sanitizer_list}"
                                             -fno-omit-frame-pointer)
    target_compile_definitions(${target}
                               PRIVATE CXXLENS_SANITIZER_INSTRUMENTED=1)
    target_link_options(${target} PUBLIC "-fsanitize=${sanitizer_list}")
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      target_sources(
        ${target}
        PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CxxlensSanitizerOptions.cpp"
      )
    endif()
  endif()
endfunction()

function(cxxlens_configure_sanitizer_tests)
  set(sanitizer_environment "")
  if(CXXLENS_ENABLE_ASAN)
    list(
      APPEND
      sanitizer_environment
      "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:exitcode=86:handle_segv=0:symbolize=1"
    )
  endif()
  if(CXXLENS_ENABLE_UBSAN)
    list(
      APPEND sanitizer_environment
      "UBSAN_OPTIONS=halt_on_error=1:exitcode=86:print_stacktrace=1:symbolize=1"
    )
  endif()
  if(CXXLENS_ENABLE_TSAN)
    list(
      APPEND
      sanitizer_environment
      "TSAN_OPTIONS=halt_on_error=1:exitcode=86:handle_segv=0:second_deadlock_stack=1:symbolize=1"
    )
  endif()
  if(NOT sanitizer_environment)
    return()
  endif()

  find_program(CXXLENS_LLVM_SYMBOLIZER NAMES llvm-symbolizer-22 llvm-symbolizer)
  if(CXXLENS_LLVM_SYMBOLIZER)
    list(APPEND sanitizer_environment
         "ASAN_SYMBOLIZER_PATH=${CXXLENS_LLVM_SYMBOLIZER}")
  endif()
  get_property(
    cxxlens_tests
    DIRECTORY
    PROPERTY TESTS)
  foreach(test_name IN LISTS cxxlens_tests)
    set_property(
      TEST "${test_name}"
      APPEND
      PROPERTY ENVIRONMENT ${sanitizer_environment})
  endforeach()
endfunction()
