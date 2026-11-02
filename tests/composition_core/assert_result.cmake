# Exact exit plus anchored summary checks; deliberately not CTest WILL_FAIL.
# A missing executable, timeout, crash, or lost argument must not pass a control.
if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
  message(FATAL_ERROR "Composition test executable is missing")
endif()
if(NOT DEFINED PROBE_ARG OR NOT DEFINED EXPECTED_EXIT OR NOT DEFINED CONTROL)
  message(FATAL_ERROR "Composition test wrapper arguments are incomplete")
endif()
execute_process(
  COMMAND "${PROBE}" "${PROBE_ARG}"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE stdout_text
  ERROR_VARIABLE stderr_text
  TIMEOUT 120
)
if(NOT "${actual_exit}" STREQUAL "${EXPECTED_EXIT}")
  message(FATAL_ERROR
    "Composition test exit: expected=${EXPECTED_EXIT} actual=${actual_exit}\n${stdout_text}${stderr_text}")
endif()
if(CONTROL STREQUAL "normal")
  if(NOT stdout_text MATCHES "^RESULT PASS suite=[a-z_]+ assertion_count=[1-9][0-9]* failure_count=0[\r\n]+$")
    message(FATAL_ERROR "Composition suite did not produce its exact passing summary")
  endif()
  if(NOT stderr_text STREQUAL "")
    message(FATAL_ERROR "Composition passing suite unexpectedly wrote to stderr")
  endif()
elseif(CONTROL STREQUAL "intentional")
  if(NOT stdout_text MATCHES "^RESULT FAIL suite=intentional_control assertion_count=3 failure_count=1[\r\n]+$")
    message(FATAL_ERROR "Intentional control did not execute the one false assertion")
  endif()
  if(NOT stderr_text MATCHES "^CHECK FAIL case=intentional.wrong_expected_kana[\r\n]+$")
    message(FATAL_ERROR "Intentional control did not identify the false assertion")
  endif()
elseif(CONTROL STREQUAL "arguments")
  if(NOT stdout_text STREQUAL "" OR NOT stderr_text MATCHES "^ARGUMENT ERROR unknown_suite_argument[\r\n]+$")
    message(FATAL_ERROR "Unknown argument was not explicitly rejected")
  endif()
else()
  message(FATAL_ERROR "Unknown composition wrapper control")
endif()
string(STRIP "${stdout_text}" summary_text)
message(STATUS "Composition control=${CONTROL} observed_exit=${actual_exit} ${summary_text}")
