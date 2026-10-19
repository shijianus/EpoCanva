# Test wrapper for the EpoCanva toolchain probe.
#
# Invoked as: cmake -D<vars> -P assert_probe.cmake
# It runs the real compiled probe with execute_process, captures the exit code,
# stdout and stderr separately, and compares against an explicitly expected
# integer exit code plus an explicit success-marker expectation.
#
# Contract (all values arrive as -D options from tests/infrastructure/CMakeLists.txt):
#   PROBE            absolute path to the probe executable          (required)
#   PROBE_ARG1       first argument passed to the probe             (required)
#   PROBE_ARG2       optional second argument (empty/absent = none)
#   EXPECTED_EXIT    exact integer exit code the probe must return  (required)
#   EXPECT_MARKER    TRUE  -> stdout or stderr must contain MARKER
#                    FALSE -> neither may contain MARKER            (required)
#   MARKER           success marker string, default "RESULT PASS"
#   TIMEOUT_SECONDS  per-run timeout for execute_process, default 30
#   CASE             label used in messages only
#
# Nothing here is shell-interpolated: the probe and each argument are separate
# execute_process list items, so paths containing spaces work unchanged.

if(NOT DEFINED PROBE)
  message(FATAL_ERROR "assert_probe: -DPROBE=<path> is required")
endif()

if(PROBE STREQUAL "")
  message(FATAL_ERROR "assert_probe: PROBE is empty; the generator expression for the probe target did not expand")
endif()

if(NOT IS_ABSOLUTE "${PROBE}")
  message(FATAL_ERROR "assert_probe: PROBE must be an absolute path, got '${PROBE}'")
endif()

if(NOT EXISTS "${PROBE}")
  message(FATAL_ERROR "assert_probe: PROBE does not exist: '${PROBE}' (was the probe target built for this configuration?)")
endif()

if(NOT DEFINED EXPECTED_EXIT OR EXPECTED_EXIT STREQUAL "")
  message(FATAL_ERROR "assert_probe: -DEXPECTED_EXIT=<int> is required")
endif()

if(NOT EXPECTED_EXIT MATCHES "^-?[0-9]+$")
  message(FATAL_ERROR "assert_probe: EXPECTED_EXIT must be an integer, got '${EXPECTED_EXIT}'")
endif()

if(NOT DEFINED EXPECT_MARKER OR EXPECT_MARKER STREQUAL "")
  message(FATAL_ERROR "assert_probe: -DEXPECT_MARKER=TRUE|FALSE is required")
endif()

if(NOT EXPECT_MARKER MATCHES "^(TRUE|FALSE)$")
  message(FATAL_ERROR "assert_probe: EXPECT_MARKER must be TRUE or FALSE, got '${EXPECT_MARKER}'")
endif()

if(NOT DEFINED PROBE_ARG1 OR PROBE_ARG1 STREQUAL "")
  message(FATAL_ERROR "assert_probe: -DPROBE_ARG1=<mode> is required")
endif()

if(NOT DEFINED MARKER OR MARKER STREQUAL "")
  set(MARKER "RESULT PASS")
endif()

if(NOT DEFINED TIMEOUT_SECONDS OR TIMEOUT_SECONDS STREQUAL "")
  set(TIMEOUT_SECONDS 30)
endif()

if(NOT TIMEOUT_SECONDS MATCHES "^[0-9]+$")
  message(FATAL_ERROR "assert_probe: TIMEOUT_SECONDS must be a positive integer, got '${TIMEOUT_SECONDS}'")
endif()

if(NOT DEFINED CASE)
  set(CASE "probe")
endif()

# Two explicit argument shapes rather than a ;-list, so a mangled list separator
# cannot silently turn a two-argument case into a one-argument case.
set(command "${PROBE}" "${PROBE_ARG1}")
if(DEFINED PROBE_ARG2 AND NOT PROBE_ARG2 STREQUAL "")
  list(APPEND command "${PROBE_ARG2}")
endif()

execute_process(
  COMMAND ${command}
  OUTPUT_VARIABLE probe_stdout
  ERROR_VARIABLE probe_stderr
  RESULT_VARIABLE probe_result
  TIMEOUT ${TIMEOUT_SECONDS}
)

set(combined "${probe_stdout}${probe_stderr}")

message(STATUS "assert_probe[${CASE}] argv=${command}")
message(STATUS "assert_probe[${CASE}] result=${probe_result} expected=${EXPECTED_EXIT}")
message(STATUS "assert_probe[${CASE}] stdout:\n${probe_stdout}")
message(STATUS "assert_probe[${CASE}] stderr:\n${probe_stderr}")

# 1. The process must actually have run and reported a numeric exit code.
#    Non-numeric means it could not start, was killed, or hit the timeout.
if(NOT probe_result MATCHES "^-?[0-9]+$")
  message(FATAL_ERROR
    "assert_probe[${CASE}]: probe did not terminate normally "
    "(launch failure, timeout after ${TIMEOUT_SECONDS}s, or abnormal end): "
    "result='${probe_result}'")
endif()

# 2. Exact exit code comparison.
if(NOT probe_result EQUAL EXPECTED_EXIT)
  message(FATAL_ERROR
    "assert_probe[${CASE}]: exit code mismatch, got ${probe_result} expected ${EXPECTED_EXIT}. "
    "stderr was: ${probe_stderr}")
endif()

# 3. Success marker policy, using string FIND so the marker is matched literally.
string(FIND "${combined}" "${MARKER}" marker_offset)
set(marker_present FALSE)
if(NOT marker_offset EQUAL -1)
  set(marker_present TRUE)
endif()

if(EXPECT_MARKER STREQUAL "TRUE")
  if(NOT marker_present)
    message(FATAL_ERROR
      "assert_probe[${CASE}]: exit code was ${EXPECTED_EXIT} but the success marker "
      "'${MARKER}' is missing, so the probe may have returned early without completing its checks")
  endif()
else()
  if(marker_present)
    message(FATAL_ERROR
      "assert_probe[${CASE}]: negative case printed the success marker '${MARKER}' "
      "while exiting with ${probe_result}; the probe contradicts itself")
  endif()
endif()

# 4. A non-zero exit combined with the success marker is always a contradiction,
#    even if a future caller forgets to set EXPECT_MARKER=FALSE.
if(NOT EXPECTED_EXIT EQUAL 0 AND marker_present)
  message(FATAL_ERROR
    "assert_probe[${CASE}]: non-zero exit ${EXPECTED_EXIT} with success marker present")
endif()

message(STATUS "assert_probe[${CASE}]: OK (${probe_result} == ${EXPECTED_EXIT}, marker ${marker_present})")
