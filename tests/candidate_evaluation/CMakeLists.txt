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
  # Compile the same backend sources and dependencies into a separate static
  # library for this executable. The observation option never reaches the
  # production backend, callers, or any DLL export set.
  get_target_property(_eval_backend_sources epocanva_backend SOURCES)
  get_target_property(_eval_backend_source_dir epocanva_backend SOURCE_DIR)
  set(_eval_absolute_sources "")
  foreach(_source IN LISTS _eval_backend_sources)
    if(IS_ABSOLUTE "${_source}")
      list(APPEND _eval_absolute_sources "${_source}")
    else()
      list(APPEND _eval_absolute_sources "${_eval_backend_source_dir}/${_source}")
    endif()
  endforeach()
  add_library(epocanva_backend_evaluation STATIC ${_eval_absolute_sources})
  foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                    COMPILE_FEATURES LINK_LIBRARIES)
    get_target_property(_value epocanva_backend ${_property})
    if(_value)
      set_property(TARGET epocanva_backend_evaluation PROPERTY ${_property} "${_value}")
    endif()
  endforeach()
  target_compile_definitions(epocanva_backend_evaluation PRIVATE
    EPOCANVA_BACKEND_TEST_OBSERVATION=1)
  set_target_properties(epocanva_backend_evaluation PROPERTIES CXX_EXTENSIONS OFF)
  add_executable(epocanva_candidate_eval_cli candidate_eval_cli.cc)
  # The evaluation lib mirrors epocanva_backend's link closure explicitly:
  # MSBuild does not propagate a static library's project references into the
  # consuming executable's link, so the CLI lists the same dependency libraries
  # the production backend links. Keep this list in step with
  # engine/backend/CMakeLists.txt.
  target_link_libraries(epocanva_candidate_eval_cli PRIVATE epocanva_backend_evaluation
    epocanva_fusion epocanva_engine_api epocanva_candidates epocanva_backend_session
    epocanva_prediction_core epocanva_prediction_abbreviation epocanva_lexical_service
    epocanva_windows_learning_repository epocanva_adaptive_ranking
    epocanva_ja_composition_core epocanva_ko_composition_core epocanva_zh_pinyin_core
    epocanva_japanese_decoder
    "$<TARGET_NAME_IF_EXISTS:epocanva_french_dictionary>"
    psapi bcrypt)
  target_include_directories(epocanva_candidate_eval_cli PRIVATE "${_eval_repo}"
    "${_eval_repo}/engine/include")
  target_compile_features(epocanva_candidate_eval_cli PRIVATE cxx_std_17)
  target_compile_definitions(epocanva_candidate_eval_cli PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 EPOCANVA_BACKEND_TEST_OBSERVATION=1
    "EPOCANVA_EVAL_REPO_ROOT=\"${_eval_repo}\"")
  target_compile_options(epocanva_candidate_eval_cli PRIVATE /utf-8 /permissive- /W4 /EHsc)
  set_target_properties(epocanva_candidate_eval_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()

add_test(NAME candidate_evaluation.contract
  COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}" -p "test_*.py" -v)
set_tests_properties(candidate_evaluation.contract PROPERTIES
  TIMEOUT 300 ENVIRONMENT "PYTHONIOENCODING=utf-8"
  LABELS "ci;evaluation;contract;negative;contract-gate")
