# Full document oracle over the production edit layer and the real TSF manager.
if(NOT WIN32)
  message(FATAL_ERROR "The editor reference protocol requires Windows TSF")
endif()
get_filename_component(_editor_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_editor_host "${_editor_root}/tests/windows/tsf_edit")
set(_editor_layer "${_editor_root}/platforms/windows/tsf_edit")
add_executable(epocanva_editor_reference_tests
  main.cc
  "${_editor_host}/test_host.cc"
  "${_editor_host}/test_text_store.cc"
  "${_editor_host}/test_support.cc"
  "${_editor_host}/inputscope_test_guids.cc")
if(TARGET epocanva_tsf_edit_layer)
  target_link_libraries(epocanva_editor_reference_tests PRIVATE epocanva_tsf_edit_layer)
else()
  target_sources(epocanva_editor_reference_tests PRIVATE
    "${_editor_layer}/edit_plan.cc" "${_editor_layer}/edit_result.cc"
    "${_editor_layer}/edit_session.cc" "${_editor_layer}/composition_writer.cc"
    "${_editor_root}/platforms/windows/text/text_encoding.cc")
endif()
target_include_directories(epocanva_editor_reference_tests PRIVATE
  "${_editor_host}" "${_editor_layer}" "${_editor_root}/platforms/windows/text")
target_compile_features(epocanva_editor_reference_tests PRIVATE cxx_std_17)
target_compile_definitions(epocanva_editor_reference_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(epocanva_editor_reference_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
target_link_libraries(epocanva_editor_reference_tests PRIVATE ole32 oleaut32 uuid user32 psapi)
set_target_properties(epocanva_editor_reference_tests PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME editor.reference_model.stress COMMAND epocanva_editor_reference_tests
  --report "${CMAKE_CURRENT_BINARY_DIR}/editor-report.json")
add_test(NAME editor.reference_model.replay_minimization COMMAND epocanva_editor_reference_tests
  --self-test --report "${CMAKE_CURRENT_BINARY_DIR}/negative-control.json")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME editor.reference_model.evidence_contract
  COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_stress_contract.py" -v)
set_tests_properties(editor.reference_model.evidence_contract PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120
  LABELS "windows;editor;negative;reference-model")
foreach(_editor_case IN ITEMS can_fail report_unwritable)
  if(_editor_case STREQUAL "can_fail")
    set(_editor_args -DPROBE_ARG1=--force-fail -DEXPECTED_EXIT=1)
  else()
    set(_editor_args -DPROBE_ARG1=--report "-DPROBE_ARG2=${CMAKE_CURRENT_BINARY_DIR}" -DEXPECTED_EXIT=2)
  endif()
  add_test(NAME editor.reference_model.${_editor_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_editor_reference_tests> ${_editor_args}
      -DEXPECT_MARKER=FALSE "-DMARKER=RESULT PASS" -DCASE=editor-${_editor_case}
      -DTIMEOUT_SECONDS=60 -P "${_editor_root}/tests/infrastructure/assert_probe.cmake")
endforeach()
set_tests_properties(editor.reference_model.stress PROPERTIES TIMEOUT 1800 LABELS "windows;tsf;editor;stress;reference-model")
set_tests_properties(editor.reference_model.replay_minimization
  editor.reference_model.can_fail editor.reference_model.report_unwritable
  PROPERTIES TIMEOUT 120 LABELS "windows;tsf;editor;negative;reference-model")
