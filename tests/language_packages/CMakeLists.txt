cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaLanguagePackageTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_language_package_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_language_package_build_root "${_language_package_repo}/build")
cmake_path(IS_PREFIX _language_package_build_root "${CMAKE_BINARY_DIR}" NORMALIZE
           _language_package_in_build)
if(NOT _language_package_in_build OR CMAKE_BINARY_DIR STREQUAL _language_package_build_root)
  message(FATAL_ERROR "Language package test artifacts must stay in build/<preset>/")
endif()
if(NOT TARGET epocanva_language_packages)
  add_subdirectory("${_language_package_repo}/engine/language_packages"
                   "${CMAKE_CURRENT_BINARY_DIR}/language_packages_core")
endif()
add_executable(epocanva_language_package_tests main.cc)
target_link_libraries(epocanva_language_package_tests PRIVATE epocanva_language_packages epocanva_strict)
set_target_properties(epocanva_language_package_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

foreach(_case unit forcefail unknown)
  if(_case STREQUAL "unit")
    set(_expected_exit 0)
    set(_expect_marker TRUE)
  elseif(_case STREQUAL "forcefail")
    set(_expected_exit 1)
    set(_expect_marker FALSE)
  else()
    set(_expected_exit 2)
    set(_expect_marker FALSE)
  endif()
  add_test(NAME language_packages.${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_language_package_tests>
      -DCASE=${_case} -DPROBE_ARG1=--${_case}
      -DEXPECTED_EXIT=${_expected_exit} -DEXPECT_MARKER=${_expect_marker}
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
      -P "${_language_package_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties(language_packages.${_case} PROPERTIES
    TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "language-packages;core;unit")
endforeach()
