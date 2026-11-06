cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaLocaleRegistryTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_locale_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_locale_build_root "${_locale_repo}/build")
cmake_path(IS_PREFIX _locale_build_root "${CMAKE_BINARY_DIR}" NORMALIZE _locale_in_build)
if(NOT _locale_in_build OR CMAKE_BINARY_DIR STREQUAL _locale_build_root)
  message(FATAL_ERROR "Locale registry test artifacts must stay in build/<preset>/")
endif()
if(NOT TARGET epocanva_locale)
  add_subdirectory("${_locale_repo}/engine/locale" "${CMAKE_CURRENT_BINARY_DIR}/locale_core")
endif()

add_executable(epocanva_locale_registry_tests main.cc)
target_link_libraries(epocanva_locale_registry_tests PRIVATE epocanva_locale epocanva_strict)
target_include_directories(epocanva_locale_registry_tests PRIVATE "${_locale_repo}/engine/include")
set_target_properties(epocanva_locale_registry_tests PROPERTIES
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
  add_test(NAME locale_registry.${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_locale_registry_tests>
      -DCASE=${_case} -DPROBE_ARG1=--${_case}
      -DEXPECTED_EXIT=${_expected_exit} -DEXPECT_MARKER=${_expect_marker}
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
      -P "${_locale_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties(locale_registry.${_case} PROPERTIES
    TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "locale;core;unit")
endforeach()

# The committed manifest is generated from BuiltinProfiles(). This gate fails
# when the C++ catalogue changed without regenerating the JSON, when export is
# nondeterministic, or when a mutation would go undetected.
set(_manifest_committed "${_locale_repo}/data/locale/backend-bindings.json")
add_test(NAME locale_registry.manifest
  COMMAND "${CMAKE_COMMAND}"
    -DPROBE=$<TARGET_FILE:epocanva_locale_registry_tests>
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/manifest
    -DCOMMITTED=${_manifest_committed}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_manifest.cmake")
set_tests_properties(locale_registry.manifest PROPERTIES
  TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  LABELS "locale;core;unit;manifest")

add_test(NAME locale_registry.manifest_stale_can_fail
  COMMAND "${CMAKE_COMMAND}"
    -DPROBE=$<TARGET_FILE:epocanva_locale_registry_tests>
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/manifest-stale
    -DEXPECT_STALE=1
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_manifest.cmake")
set_tests_properties(locale_registry.manifest_stale_can_fail PROPERTIES
  TIMEOUT 120 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  LABELS "locale;core;unit;manifest;negative")
