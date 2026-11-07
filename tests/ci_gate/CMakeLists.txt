cmake_minimum_required(VERSION 3.25)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Machine-independent tests for the falsifiable CI gate tooling:
# - test_check_whitespace.py   the event-aware committed-range whitespace gate
# - test_contract_inventory.py the exact CTest inventory verifier
# Both need only Python and git, so they run wherever testing is enabled and
# carry the contract-gate label like every other contract test. The real
# inventory comparison against the live build tree is a CI gate step that
# runs after configure, not here.
add_test(NAME ci_gate.contract
  COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}" -p "test_*.py" -v)
set_tests_properties(ci_gate.contract PROPERTIES
  TIMEOUT 300 ENVIRONMENT "PYTHONIOENCODING=utf-8"
  LABELS "ci;gate;contract;negative;contract-gate")
