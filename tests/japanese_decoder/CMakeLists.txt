cmake_minimum_required(VERSION 3.25)
get_filename_component(_ja_test_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(epocanva_japanese_decoder_tests main.cc)
target_link_libraries(epocanva_japanese_decoder_tests PRIVATE epocanva_japanese_decoder)
target_compile_options(epocanva_japanese_decoder_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
add_test(NAME japanese.decoder.real_dictionary COMMAND epocanva_japanese_decoder_tests
  "${_ja_test_repo}/build/backend-japanese-data/lexicon.bin"
  e0308bb1e09e33915a40ca487ca3c27081d9564876f655a23fb332e2d6b3d011)
set_tests_properties(japanese.decoder.real_dictionary PROPERTIES TIMEOUT 120)
add_test(NAME japanese.decoder.can_fail COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:epocanva_japanese_decoder_tests>"
  -DCASE=ja-negative -DEXPECTED_EXIT=1 -DMARKER=PASS -DMARKER_POLICY=forbid -DPASS_ARGS=--forcefail
  -P "${_ja_test_repo}/tests/english_completion/assert_exit.cmake")
