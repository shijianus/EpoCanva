cmake_minimum_required(VERSION 3.25)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# E6 candidate-menu evaluation baseline.
#
# The native harness (candidate_eval_cli.cc) is built only where the backend
# library exists -- the tools/backend project. The root project includes this
# directory for the machine-independent Python evaluator tests, which need no
# native build and no external Tier 2 input. The name matches the CI
# contract-gate regex "candidate_evaluation\.".
if(TARGET epocanva_backend)
  get_filename_component(_eval_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  add_executable(epocanva_candidate_eval_cli candidate_eval_cli.cc)
  target_link_libraries(epocanva_candidate_eval_cli PRIVATE epocanva_backend psapi)
  target_include_directories(epocanva_candidate_eval_cli PRIVATE "${_eval_repo}")
  target_compile_features(epocanva_candidate_eval_cli PRIVATE cxx_std_17)
  target_compile_definitions(epocanva_candidate_eval_cli PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(epocanva_candidate_eval_cli PRIVATE /utf-8 /permissive- /W4 /EHsc)
  set_target_properties(epocanva_candidate_eval_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()

add_test(NAME candidate_evaluation.contract
  COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}" -p "test_evaluate_candidates.py" -v)
set_tests_properties(candidate_evaluation.contract PROPERTIES
  TIMEOUT 300 ENVIRONMENT "PYTHONIOENCODING=utf-8"
  LABELS "ci;evaluation;contract;negative")
