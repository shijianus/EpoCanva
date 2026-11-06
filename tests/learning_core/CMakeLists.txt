cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaLearningTests LANGUAGES CXX)
  enable_testing()
endif()
if(NOT TARGET epocanva_learning_core)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/data_store" learning-core)
endif()
add_executable(epocanva_learning_tests main.cc)
target_link_libraries(epocanva_learning_tests PRIVATE epocanva_learning_core)
target_compile_features(epocanva_learning_tests PRIVATE cxx_std_17)
set_target_properties(epocanva_learning_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_learning_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
endif()
foreach(case privacy identity counts migration malformed bounded feedback decay manual export)
  add_test(NAME learning.core.${case} COMMAND epocanva_learning_tests ${case})
endforeach()
add_test(NAME learning.core.can_fail COMMAND "${CMAKE_COMMAND}"
  "-DEXE=$<TARGET_FILE:epocanva_learning_tests>"
  "-DEXPECTED_EXIT=1" "-DCASE=learning.core.can_fail"
  "-DMARKER=PASS" "-DMARKER_POLICY=forbid" "-DPASS_ARGS=--force-failure"
  -P "${CMAKE_CURRENT_LIST_DIR}/../english_completion/assert_exit.cmake")
