# Verifies that the committed backend binding manifest is a deterministic export
# of the C++ locale registry.
#
# Normal mode:
#   1. run the probe twice and require byte-identical output;
#   2. require the committed data/locale/backend-bindings.json to match it;
#   3. prove the comparison is meaningful by showing a one-byte mutation differs.
#
# EXPECT_STALE mode:
#   export the manifest, mutate a copy, then invoke MODE=compare in a child
#   CMake process and require that comparison to REJECT it. This makes the
#   freshness gate falsifiable in a permanent test, not just by inspection.
#
# MODE=compare is the child entry point used by EXPECT_STALE:
#   read EXPORT and COMMITTED and fail when they differ.
if(DEFINED MODE AND MODE STREQUAL "compare")
  if(NOT DEFINED EXPORT OR NOT DEFINED COMMITTED)
    message(FATAL_ERROR "EXPORT and COMMITTED are required for MODE=compare")
  endif()
  if(NOT EXISTS "${EXPORT}" OR NOT EXISTS "${COMMITTED}")
    message(FATAL_ERROR "compare inputs are missing")
  endif()
  file(READ "${EXPORT}" export_bytes)
  file(READ "${COMMITTED}" committed_bytes)
  if(NOT export_bytes STREQUAL committed_bytes)
    message(FATAL_ERROR "committed manifest differs from the export")
  endif()
  message(STATUS "committed manifest matches the export")
  return()
endif()
if(NOT DEFINED PROBE OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "PROBE and OUTPUT_DIR are required")
endif()
if(NOT EXISTS "${PROBE}")
  message(FATAL_ERROR "probe does not exist: ${PROBE}")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(run_probe destination)
  execute_process(
    COMMAND "${PROBE}" --manifest "${destination}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "manifest export failed (${result}): ${output}${error}")
  endif()
  if(NOT EXISTS "${destination}")
    message(FATAL_ERROR "manifest export did not create ${destination}")
  endif()
endfunction()

set(first_path "${OUTPUT_DIR}/backend-bindings-a.json")
set(second_path "${OUTPUT_DIR}/backend-bindings-b.json")
run_probe("${first_path}")
run_probe("${second_path}")
file(READ "${first_path}" first)
file(READ "${second_path}" second)
if(first STREQUAL "")
  message(FATAL_ERROR "manifest export is empty")
endif()
if(NOT first STREQUAL second)
  message(FATAL_ERROR "manifest export is not deterministic across two runs")
endif()

string(REGEX REPLACE "\"format\": 1" "\"format\": 2" mutated "${first}")
if(mutated STREQUAL first)
  message(FATAL_ERROR "self-check mutation did not change the manifest")
endif()

if(DEFINED EXPECT_STALE AND EXPECT_STALE)
  set(stale_path "${OUTPUT_DIR}/backend-bindings-stale.json")
  file(WRITE "${stale_path}" "${mutated}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -DMODE=compare
      -DEXPORT=${first_path}
      -DCOMMITTED=${stale_path}
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_output
    ERROR_VARIABLE compare_error)
  if(compare_result EQUAL 0)
    message(FATAL_ERROR
      "a hand-modified manifest was accepted by the freshness comparison")
  endif()
  message(STATUS "stale manifest correctly rejected (child exit ${compare_result})")
  return()
endif()

if(NOT DEFINED COMMITTED OR NOT EXISTS "${COMMITTED}")
  message(FATAL_ERROR "committed manifest is missing: ${COMMITTED}")
endif()
file(READ "${COMMITTED}" committed)
if(NOT first STREQUAL committed)
  message(FATAL_ERROR
    "data/locale/backend-bindings.json is stale. Regenerate it with:\n"
    "  <locale probe> --manifest data/locale/backend-bindings.json\n"
    "The C++ locale registry is the only authored source; do not edit the JSON by hand.")
endif()
if(mutated STREQUAL committed)
  message(FATAL_ERROR "mutation comparison unexpectedly matched the committed manifest")
endif()
message(STATUS "backend binding manifest is deterministic and matches the committed export")
