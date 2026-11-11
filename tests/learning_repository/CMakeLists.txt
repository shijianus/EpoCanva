cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaLearningRepositoryTests LANGUAGES CXX)
  enable_testing()
endif()
if(NOT WIN32)
  message(FATAL_ERROR "The learning repository tests require Windows.")
endif()
if(NOT TARGET epocanva_learning_core)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/data_store" learning-core)
endif()
if(NOT TARGET epocanva_windows_learning_repository)
  add_library(epocanva_windows_learning_repository STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../../engine/data_store/windows_learning_repository.cc")
  target_link_libraries(epocanva_windows_learning_repository PUBLIC
    epocanva_learning_core PRIVATE advapi32)
  target_compile_features(epocanva_windows_learning_repository PUBLIC cxx_std_17)
  target_compile_definitions(epocanva_windows_learning_repository PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  set_target_properties(epocanva_windows_learning_repository PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(epocanva_windows_learning_repository PRIVATE /utf-8 /permissive- /W4 /EHsc)
  endif()
endif()
add_executable(epocanva_learning_repository_tests main.cc)
target_link_libraries(epocanva_learning_repository_tests PRIVATE
  epocanva_windows_learning_repository advapi32)
target_compile_features(epocanva_learning_repository_tests PRIVATE cxx_std_17)
target_compile_definitions(epocanva_learning_repository_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
set_target_properties(epocanva_learning_repository_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_learning_repository_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run")
foreach(case persistence privacy revocation errors corruption feedback management acl paths symlinks concurrency faults)
  add_test(NAME learning.repository.${case}
    COMMAND epocanva_learning_repository_tests ${case} "${CMAKE_CURRENT_BINARY_DIR}/run")
  set_tests_properties(learning.repository.${case} PROPERTIES
    TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run"
    LABELS "learning;repository;windows")
endforeach()
set_tests_properties(learning.repository.symlinks PROPERTIES SKIP_RETURN_CODE 77)
foreach(mode force-failure invalid-mode)
  if(mode STREQUAL "force-failure")
    set(expected_exit 1)
  else()
    set(expected_exit 2)
  endif()
  add_test(NAME learning.repository.${mode}
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_learning_repository_tests>"
      "-DMODE=--${mode}" "-DEXPECTED_EXIT=${expected_exit}"
      -P "${CMAKE_CURRENT_LIST_DIR}/assert_exit.cmake")
endforeach()
