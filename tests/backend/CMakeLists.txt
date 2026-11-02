cmake_minimum_required(VERSION 3.25)
get_filename_component(_backend_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_backend_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_custom_command(OUTPUT "${_backend_fixtures}/model.tsv" "${_backend_fixtures}/model.sha256"
  COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/fixture_model.py" "${_backend_fixtures}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fixture_model.py" VERBATIM)
add_custom_target(epocanva_backend_fixtures DEPENDS "${_backend_fixtures}/model.tsv" "${_backend_fixtures}/model.sha256")
add_executable(epocanva_backend_c_consumer c_consumer.c)
target_link_libraries(epocanva_backend_c_consumer PRIVATE epocanva_backend)
set_target_properties(epocanva_backend_c_consumer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
add_dependencies(epocanva_backend_c_consumer epocanva_backend_fixtures)
add_test(NAME backend.abi.pure_c_profiles COMMAND epocanva_backend_c_consumer
  "${_backend_fixtures}/model.tsv" "${_backend_fixtures}/model.sha256")
add_test(NAME backend.abi.pure_c_can_fail COMMAND "${CMAKE_COMMAND}"
  "-DEXE=$<TARGET_FILE:epocanva_backend_c_consumer>" -DEXPECTED_EXIT=1
  -DCASE=backend-c-negative -DMARKER=PASS -DMARKER_POLICY=forbid -DPASS_ARGS=--force-failure
  -P "${_backend_repo}/tests/english_completion/assert_exit.cmake")
add_executable(epocanva_backend_context_regressions context_regressions.cc)
target_link_libraries(epocanva_backend_context_regressions PRIVATE
  epocanva_backend epocanva_prediction_core epocanva_backend_session bcrypt)
target_include_directories(epocanva_backend_context_regressions PRIVATE "${_backend_repo}")
target_compile_definitions(epocanva_backend_context_regressions PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_features(epocanva_backend_context_regressions PRIVATE cxx_std_17)
target_compile_options(epocanva_backend_context_regressions PRIVATE /utf-8 /permissive- /W4 /EHsc)
set(_context_args --unit "${CMAKE_CURRENT_BINARY_DIR}/context-regressions-scratch")
if(EPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY)
  list(APPEND _context_args --hunspell)
endif()
add_test(NAME backend.abi.context_regressions COMMAND epocanva_backend_context_regressions ${_context_args})
add_test(NAME backend.abi.context_regressions_can_fail COMMAND "${CMAKE_COMMAND}"
  "-DEXE=$<TARGET_FILE:epocanva_backend_context_regressions>" -DEXPECTED_EXIT=1
  -DCASE=context-regression-negative -DMARKER=PASS -DMARKER_POLICY=forbid -DPASS_ARGS=--forcefail
  -P "${_backend_repo}/tests/english_completion/assert_exit.cmake")
add_executable(epocanva_backend_contract_tests main.cc)
target_link_libraries(epocanva_backend_contract_tests PRIVATE epocanva_backend)
target_compile_options(epocanva_backend_contract_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
add_dependencies(epocanva_backend_contract_tests epocanva_backend_fixtures)
add_test(NAME backend.headless.contracts COMMAND epocanva_backend_contract_tests
  "${_backend_fixtures}/model.tsv" "${_backend_fixtures}/model.sha256")
set_tests_properties(backend.headless.contracts PROPERTIES TIMEOUT 120)
if(EPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY)
  add_executable(epocanva_backend_profiles profiles.cc)
  target_link_libraries(epocanva_backend_profiles PRIVATE epocanva_backend epocanva_prediction_core)
  target_include_directories(epocanva_backend_profiles PRIVATE "${_backend_repo}")
  target_compile_definitions(epocanva_backend_profiles PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_features(epocanva_backend_profiles PRIVATE cxx_std_17)
  target_compile_options(epocanva_backend_profiles PRIVATE /utf-8 /permissive- /W4 /EHsc)
  set_target_properties(epocanva_backend_profiles PROPERTIES CXX_EXTENSIONS OFF)
  set(_profile_rime_dll "${_backend_repo}/build/librime-english/rime/install/lib/rime.dll")
  file(SHA256 "${_profile_rime_dll}" _profile_rime_sha256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_profile_rime_dll}")
  add_test(NAME backend.profiles.real_acceptance COMMAND epocanva_backend_profiles
    "${_backend_repo}" "${CMAKE_CURRENT_BINARY_DIR}/real-profile-sessions" "${_profile_rime_sha256}")
  set_tests_properties(backend.profiles.real_acceptance PROPERTIES TIMEOUT 600)
  add_test(NAME backend.profiles.real_can_fail COMMAND "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:epocanva_backend_profiles>" -DEXPECTED_EXIT=1
    -DCASE=profile-negative -DMARKER=PASS -DMARKER_POLICY=forbid -DPASS_ARGS=--force-failure
    -P "${_backend_repo}/tests/english_completion/assert_exit.cmake")
  add_executable(epocanva_backend_real_languages real_languages.cc)
  target_link_libraries(epocanva_backend_real_languages PRIVATE epocanva_backend psapi)
  target_include_directories(epocanva_backend_real_languages PRIVATE "${_backend_repo}/platforms/windows/product")
  target_compile_options(epocanva_backend_real_languages PRIVATE /utf-8 /permissive- /W4 /EHsc)
  target_compile_definitions(epocanva_backend_real_languages PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_test(NAME backend.headless.real_five_language_api COMMAND epocanva_backend_real_languages "${_backend_repo}")
  add_test(NAME backend.headless.budget_five_language_api COMMAND epocanva_backend_real_languages "${_backend_repo}" build/backend-training-budget)
  set_tests_properties(backend.headless.budget_five_language_api PROPERTIES TIMEOUT 180 DEPENDS backend.headless.chinese_real_engine)
  set_tests_properties(backend.headless.real_five_language_api PROPERTIES TIMEOUT 180
    DEPENDS backend.headless.chinese_real_engine)
endif()
if(EXISTS "${_backend_repo}/build/backend-chinese-ranked/shared/epocanva_backend_pinyin.schema.yaml")
  add_test(NAME backend.headless.chinese_real_engine COMMAND epocanva_backend_contract_tests
    "${_backend_fixtures}/model.tsv" "${_backend_fixtures}/model.sha256"
    "${_backend_repo}/build/librime-english/rime/install/lib/rime.dll"
    "${_backend_repo}/build/backend-chinese-ranked")
  set_tests_properties(backend.headless.chinese_real_engine PROPERTIES TIMEOUT 600)
endif()
add_test(NAME backend.headless.can_fail COMMAND "${CMAKE_COMMAND}"
  "-DEXE=$<TARGET_FILE:epocanva_backend_contract_tests>" -DEXPECTED_EXIT=1
  -DCASE=backend-negative -DMARKER=PASS -DMARKER_POLICY=forbid -DPASS_ARGS=--force-failure
  -P "${_backend_repo}/tests/english_completion/assert_exit.cmake")
