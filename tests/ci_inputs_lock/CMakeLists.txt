cmake_minimum_required(VERSION 3.25)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Machine-independent validation of the Tier 2 input identity contract: the
# committed lock format plus the real PowerShell verifier's pass/fail/exit-code
# behavior on synthetic fixture trees. The real pinned inputs are verified by
# tools/ci/verify_inputs.ps1 itself, run explicitly on trees that have them.
add_test(NAME ci_inputs.lock_contract
  COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}" -p "test_inputs_lock.py" -v)
set_tests_properties(ci_inputs.lock_contract PROPERTIES
  TIMEOUT 300 ENVIRONMENT "PYTHONIOENCODING=utf-8"
  LABELS "ci;inputs;contract;negative")
