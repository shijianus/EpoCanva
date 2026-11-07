cmake_minimum_required(VERSION 3.25)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Machine-independent validation of data/locale/capability-matrix.json, the E3
# capability matrix. The suite is pure Python and needs no native build or
# external input, so it registers wherever testing is enabled (root project and
# the CI contract gate regex "capability_matrix\.").
add_test(NAME capability_matrix.contract
  COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}" -p "test_capability_matrix.py" -v)
set_tests_properties(capability_matrix.contract PROPERTIES
  TIMEOUT 300 ENVIRONMENT "PYTHONIOENCODING=utf-8"
  LABELS "ci;capability;matrix;negative")
