cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaFrenchDictionaryTests LANGUAGES CXX)
  enable_testing()
endif()
get_filename_component(EPOCANVA_FR_TEST_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT TARGET epocanva_french_dictionary)
  add_subdirectory("${EPOCANVA_FR_TEST_REPO}/engine/french_dictionary"
                   "${CMAKE_CURRENT_BINARY_DIR}/dictionary")
endif()
file(READ "${EPOCANVA_FR_TEST_REPO}/data/manifests/backend-hunspell.sources.json" _test_json)
string(JSON _test_aff_path GET "${_test_json}" french_pair aff_path)
string(JSON _test_dic_path GET "${_test_json}" french_pair dic_path)
string(JSON _test_aff_hash GET "${_test_json}" french_pair aff_sha256)
string(JSON _test_dic_hash GET "${_test_json}" french_pair dic_sha256)
string(JSON _test_revision GET "${_test_json}" hunspell revision)
set(EPOCANVA_FRENCH_AFF_PATH "${EPOCANVA_FR_TEST_REPO}/${_test_aff_path}"
  CACHE FILEPATH "Full production French .aff file, verified against pinned hash")
set(EPOCANVA_FRENCH_DIC_PATH "${EPOCANVA_FR_TEST_REPO}/${_test_dic_path}"
  CACHE FILEPATH "Full production French .dic file, verified against pinned hash")
file(TO_CMAKE_PATH "${EPOCANVA_FRENCH_AFF_PATH}" _test_aff_path)
file(TO_CMAKE_PATH "${EPOCANVA_FRENCH_DIC_PATH}" _test_dic_path)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test_config.h" CONTENT
"#ifndef EPOCANVA_FRENCH_TEST_CONFIG_H_\n#define EPOCANVA_FRENCH_TEST_CONFIG_H_\nnamespace test_config {\nconstexpr char kAffPath[] = u8R\"(${_test_aff_path})\";\nconstexpr char kDicPath[] = u8R\"(${_test_dic_path})\";\nconstexpr char kAffHash[] = \"${_test_aff_hash}\";\nconstexpr char kDicHash[] = \"${_test_dic_hash}\";\nconstexpr char kRevision[] = \"${_test_revision}\";\nconstexpr char kScratchPath[] = u8R\"(${CMAKE_CURRENT_BINARY_DIR}/scratch)\";\n}\n#endif\n")
add_executable(epocanva_french_dictionary_tests main.cc)
target_include_directories(epocanva_french_dictionary_tests PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${EPOCANVA_HUNSPELL_ROOT}/src/hunspell")
target_compile_definitions(epocanva_french_dictionary_tests PRIVATE HUNSPELL_STATIC
  NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(epocanva_french_dictionary_tests PRIVATE /utf-8 /permissive-
  /EHsc /Zc:__cplusplus /W4 /WX /UHUNSPELL_WARNING_ON)
target_link_libraries(epocanva_french_dictionary_tests PRIVATE
  epocanva_french_dictionary epocanva_hunspell bcrypt)
set_target_properties(epocanva_french_dictionary_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

foreach(_case production_load accents plurals conjugation native_parity protection
              bounds availability failures complex_rules concurrency static_load
              determinism_scheduled_pause determinism_repeated_query)
  add_test(NAME french.dictionary.${_case}
    COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:epocanva_french_dictionary_tests>"
      "-DMODE=${_case}" -DEXPECTED_EXIT=0
      -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
  set_tests_properties(french.dictionary.${_case} PROPERTIES TIMEOUT 90
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" LABELS "french;dictionary;headless")
endforeach()
add_test(NAME french.dictionary.assertions_can_fail
  COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:epocanva_french_dictionary_tests>"
    -DMODE=force_failure -DEXPECTED_EXIT=1
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
add_test(NAME french.dictionary.rejects_unknown_mode
  COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:epocanva_french_dictionary_tests>"
    -DMODE=unknown_case -DEXPECTED_EXIT=2
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
set_tests_properties(french.dictionary.assertions_can_fail
  french.dictionary.rejects_unknown_mode PROPERTIES TIMEOUT 30
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" LABELS "french;dictionary;negative")
