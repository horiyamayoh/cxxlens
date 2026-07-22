# Verify that automatically derived source identities still describe a clean,
# stable checkout when the installed occurrence manifest is generated.

foreach(_cxxlens_required IN
        ITEMS CXXLENS_PROVENANCE_MODE CXXLENS_PROVENANCE_EXPECTED_REVISION
              CXXLENS_PROVENANCE_EXPECTED_TREE)
  if(NOT DEFINED ${_cxxlens_required})
    message(FATAL_ERROR "Missing source provenance input: ${_cxxlens_required}")
  endif()
endforeach()

string(LENGTH "${CXXLENS_PROVENANCE_EXPECTED_REVISION}"
              _cxxlens_revision_length)
string(LENGTH "${CXXLENS_PROVENANCE_EXPECTED_TREE}" _cxxlens_tree_length)
if(NOT CXXLENS_PROVENANCE_EXPECTED_REVISION MATCHES "^[0-9a-f]+$"
   OR NOT CXXLENS_PROVENANCE_EXPECTED_TREE MATCHES "^[0-9a-f]+$"
   OR NOT _cxxlens_revision_length EQUAL 40
   OR NOT _cxxlens_tree_length EQUAL 40)
  message(
    FATAL_ERROR "Source provenance identities must be exact 40-hex values")
endif()

if(CXXLENS_PROVENANCE_MODE STREQUAL "explicit")
  return()
endif()
if(NOT CXXLENS_PROVENANCE_MODE STREQUAL "git-clean")
  message(
    FATAL_ERROR "Unsupported source provenance mode: ${CXXLENS_PROVENANCE_MODE}"
  )
endif()
foreach(_cxxlens_required IN ITEMS CXXLENS_PROVENANCE_SOURCE_DIR
                                   CXXLENS_PROVENANCE_GIT_EXECUTABLE)
  if(NOT DEFINED ${_cxxlens_required} OR "${${_cxxlens_required}}" STREQUAL "")
    message(
      FATAL_ERROR "Missing git-clean provenance input: ${_cxxlens_required}")
  endif()
endforeach()

function(_cxxlens_git_observe suffix)
  execute_process(
    COMMAND "${CXXLENS_PROVENANCE_GIT_EXECUTABLE}" -C
            "${CXXLENS_PROVENANCE_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE _revision_status
    OUTPUT_VARIABLE _revision
    ERROR_VARIABLE _revision_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(
    COMMAND "${CXXLENS_PROVENANCE_GIT_EXECUTABLE}" -C
            "${CXXLENS_PROVENANCE_SOURCE_DIR}" rev-parse "HEAD^{tree}"
    RESULT_VARIABLE _tree_status
    OUTPUT_VARIABLE _tree
    ERROR_VARIABLE _tree_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(
    COMMAND
      "${CXXLENS_PROVENANCE_GIT_EXECUTABLE}" -C
      "${CXXLENS_PROVENANCE_SOURCE_DIR}" status --porcelain=v1
      --untracked-files=all
    RESULT_VARIABLE _status_status
    OUTPUT_VARIABLE _status
    ERROR_VARIABLE _status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _revision_status EQUAL 0
     OR NOT _tree_status EQUAL 0
     OR NOT _status_status EQUAL 0)
    message(
      FATAL_ERROR
        "Cannot observe git-clean source provenance (${suffix}): ${_revision_error}${_tree_error}${_status_error}"
    )
  endif()
  set("_cxxlens_revision_${suffix}"
      "${_revision}"
      PARENT_SCOPE)
  set("_cxxlens_tree_${suffix}"
      "${_tree}"
      PARENT_SCOPE)
  set("_cxxlens_status_${suffix}"
      "${_status}"
      PARENT_SCOPE)
endfunction()

_cxxlens_git_observe(before)
_cxxlens_git_observe(after)
if(NOT "${_cxxlens_revision_before}" STREQUAL
   "${CXXLENS_PROVENANCE_EXPECTED_REVISION}"
   OR NOT "${_cxxlens_revision_after}" STREQUAL
      "${CXXLENS_PROVENANCE_EXPECTED_REVISION}"
   OR NOT "${_cxxlens_tree_before}" STREQUAL
      "${CXXLENS_PROVENANCE_EXPECTED_TREE}"
   OR NOT "${_cxxlens_tree_after}" STREQUAL
      "${CXXLENS_PROVENANCE_EXPECTED_TREE}"
   OR NOT "${_cxxlens_status_before}" STREQUAL ""
   OR NOT "${_cxxlens_status_after}" STREQUAL "")
  message(
    FATAL_ERROR
      "Automatically derived source provenance is dirty, stale, or unstable; provide an exact explicit revision/tree pair only from an independently controlled packaging workflow"
  )
endif()
