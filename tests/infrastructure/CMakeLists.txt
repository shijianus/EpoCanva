# EpoCanva infrastructure tests.
#
# Every case executes the real compiled probe through tests/infrastructure/assert_probe.cmake,
# which compares an explicitly expected integer exit code and an explicit
# success-marker expectation. WILL_FAIL and FAIL_REGULAR_EXPRESSION are deliberately
# not used: an inverted exit code and a regex on the output are two indirect ways of
# saying the same thing, and either alone can pass for the wrong reason.

if(NOT TARGET epocanva_toolchain_probe)
  message(FATAL_ERROR
    "epocanva_toolchain_probe target is missing. Enable EPOCANVA_BUILD_TOOLS "
    "(default ON) so the infrastructure tests have a real binary to execute.")
endif()

set(EPOCANVA_ASSERT_PROBE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/assert_probe.cmake")

if(NOT EXISTS "${EPOCANVA_ASSERT_PROBE_SCRIPT}")
  message(FATAL_ERROR "missing test wrapper: ${EPOCANVA_ASSERT_PROBE_SCRIPT}")
endif()

set(EPOCANVA_PROBE_TIMEOUT_SECONDS 30)
set(EPOCANVA_PROBE_MARKER "RESULT PASS")

# 1. Normal self-test: exit code 0 and the success marker must both be present.
add_test(
  NAME infrastructure.toolchain_probe.selftest
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_toolchain_probe>
    -DCASE=selftest
    -DPROBE_ARG1=selftest
    -DEXPECTED_EXIT=0
    -DEXPECT_MARKER=TRUE
    "-DMARKER=${EPOCANVA_PROBE_MARKER}"
    -DTIMEOUT_SECONDS=${EPOCANVA_PROBE_TIMEOUT_SECONDS}
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# 2. Forced failure: exit code 9, and the success marker must not appear.
add_test(
  NAME infrastructure.toolchain_probe.forcefail
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_toolchain_probe>
    -DCASE=forcefail
    -DPROBE_ARG1=forcefail
    -DEXPECTED_EXIT=9
    -DEXPECT_MARKER=FALSE
    "-DMARKER=${EPOCANVA_PROBE_MARKER}"
    -DTIMEOUT_SECONDS=${EPOCANVA_PROBE_TIMEOUT_SECONDS}
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# 3. Unknown mode: rejected with usage exit code 5, no success marker.
add_test(
  NAME infrastructure.toolchain_probe.unknown_mode
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_toolchain_probe>
    -DCASE=unknown_mode
    -DPROBE_ARG1=not-a-real-mode
    -DEXPECTED_EXIT=5
    -DEXPECT_MARKER=FALSE
    "-DMARKER=${EPOCANVA_PROBE_MARKER}"
    -DTIMEOUT_SECONDS=${EPOCANVA_PROBE_TIMEOUT_SECONDS}
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# 4. Too many arguments: also exit code 5. This is the case that detects a mangled
#    argument list, because a lost second argument would make the probe exit 0.
add_test(
  NAME infrastructure.toolchain_probe.extra_args
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_toolchain_probe>
    -DCASE=extra_args
    -DPROBE_ARG1=selftest
    -DPROBE_ARG2=extra
    -DEXPECTED_EXIT=5
    -DEXPECT_MARKER=FALSE
    "-DMARKER=${EPOCANVA_PROBE_MARKER}"
    -DTIMEOUT_SECONDS=${EPOCANVA_PROBE_TIMEOUT_SECONDS}
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

set_property(
  TEST
    infrastructure.toolchain_probe.selftest
    infrastructure.toolchain_probe.forcefail
    infrastructure.toolchain_probe.unknown_mode
    infrastructure.toolchain_probe.extra_args
  APPEND PROPERTY LABELS infrastructure toolchain
)

# The CTest timeout is one notch above the wrapper's own execute_process timeout so a
# hung probe is reported by the wrapper (non-numeric result) before CTest kills the run.
set_tests_properties(
  infrastructure.toolchain_probe.selftest
  infrastructure.toolchain_probe.forcefail
  infrastructure.toolchain_probe.unknown_mode
  infrastructure.toolchain_probe.extra_args
  PROPERTIES TIMEOUT 60
)
