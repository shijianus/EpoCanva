# Test wrapper for the English integration programs.
#
# Invoked as: cmake -D<vars> -P assert_exit.cmake
#
# WHY THIS EXISTS ALONGSIDE tests/infrastructure/assert_probe.cmake rather than
# replacing it: assert_probe.cmake deliberately accepts at most two program
# arguments, and its own comments say that is on purpose so "a lost second
# argument would not silently make the case pass". The English integration program needs
# FOUR (--expect-dll, --fixture-dir, --run-root, --mode), and the pilot modes a fifth,
# so a separate wrapper is honest here; the alternative -- relaxing
# assert_probe.cmake to a ;-list -- would
# weaken the guarantee that file documents for the P0.1 cases that already use it.
#
# Contract:
#   EXE              absolute path to the program                     (required)
#   EXPECTED_EXIT    exact integer exit code the program must return  (required)
#   MARKER_POLICY    require -> MARKER must appear
#                    forbid -> MARKER must not appear                  (required)
#   MARKER           the literal success marker, e.g. "RESULT PASS"     (required)
#   PASS_ARGS        the program's argument list, ';'-separated        (required)
#   TIMEOUT_SECONDS  execute_process timeout, default 180
#   CASE             label used in messages only
#
# Nothing is shell-interpolated: every argument is a separate execute_process list
# item, so this repository's path containing spaces works unchanged.

foreach(_required EXE EXPECTED_EXIT CASE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "assert_exit: -D${_required}=... is required")
  endif()
endforeach()

if(NOT IS_ABSOLUTE "${EXE}")
  message(FATAL_ERROR "assert_exit: EXE must be absolute, got '${EXE}'")
endif()
if(NOT EXISTS "${EXE}")
  message(FATAL_ERROR "assert_exit: EXE does not exist: ${EXE} (was it built for this configuration?)")
endif()
if(NOT EXPECTED_EXIT MATCHES "^-?[0-9]+$")
  message(FATAL_ERROR "assert_exit: EXPECTED_EXIT must be an integer, got '${EXPECTED_EXIT}'")
endif()
if(NOT DEFINED PASS_ARGS OR PASS_ARGS STREQUAL "")
  message(FATAL_ERROR "assert_exit: -DPASS_ARGS=<a;b;c> is required -- an empty argument list would run usage and prove nothing")
endif()
if(NOT DEFINED MARKER OR MARKER STREQUAL "")
  message(FATAL_ERROR "assert_exit: -DMARKER=<literal> is required")
endif()
if(NOT DEFINED MARKER_POLICY OR MARKER_POLICY STREQUAL "")
  message(FATAL_ERROR "assert_exit: -DMARKER_POLICY=require|forbid is required")
endif()
if(NOT MARKER_POLICY MATCHES "^(require|forbid)$")
  message(FATAL_ERROR "assert_exit: MARKER_POLICY must be require or forbid, got '${MARKER_POLICY}'")
endif()
if(NOT DEFINED TIMEOUT_SECONDS OR TIMEOUT_SECONDS STREQUAL "")
  set(TIMEOUT_SECONDS 180)
endif()

# Each ;-list item becomes one argument; quoting is handled by execute_process.
execute_process(
  COMMAND "${EXE}" ${PASS_ARGS}
  OUTPUT_VARIABLE out_stdout
  ERROR_VARIABLE out_stderr
  RESULT_VARIABLE out_result
  TIMEOUT ${TIMEOUT_SECONDS}
)

set(combined "${out_stdout}${out_stderr}")

message(STATUS "assert_exit[${CASE}] result=${out_result} expected=${EXPECTED_EXIT}")
message(STATUS "assert_exit[${CASE}] output:\n${combined}")

if(NOT out_result MATCHES "^-?[0-9]+$")
  message(FATAL_ERROR
    "assert_exit[${CASE}]: program did not terminate normally (launch failure, "
    "timeout after ${TIMEOUT_SECONDS}s, or abnormal end): result='${out_result}'")
endif()
if(NOT out_result EQUAL EXPECTED_EXIT)
  message(FATAL_ERROR
    "assert_exit[${CASE}]: exit code mismatch, got ${out_result} expected ${EXPECTED_EXIT}")
endif()

# Marker policy, matched literally with string FIND. An empty needle is never
# searched for: string(FIND ... "") succeeds at offset 0 for any input, which would
# make every "forbid" case fail unconditionally -- the bug this rewrite removes.
#
# For the negative cases the marker must be ABSENT: a program that prints PASS for
# a run that was supposed to detect a missing capability contradicts itself, and
# that contradiction is exactly the failure mode this phase guards against.
string(FIND "${combined}" "${MARKER}" marker_offset)
set(marker_present FALSE)
if(NOT marker_offset EQUAL -1)
  set(marker_present TRUE)
endif()

if(MARKER_POLICY STREQUAL "forbid")
  if(marker_present)
    message(FATAL_ERROR
      "assert_exit[${CASE}]: expected the marker '${MARKER}' NOT to appear in a "
      "run that must detect a missing capability, but it did")
  endif()
else()
  if(NOT marker_present)
    message(FATAL_ERROR
      "assert_exit[${CASE}]: exit code was ${EXPECTED_EXIT} but the success marker "
      "'${MARKER}' is missing, so the program may have returned early")
  endif()
endif()

message(STATUS "assert_exit[${CASE}]: OK (${out_result} == ${EXPECTED_EXIT}, marker ${marker_present})")
