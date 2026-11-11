cmake_minimum_required(VERSION 3.25)
project(EpoCanvaBackendResourceStagingProbe LANGUAGES CXX)
enable_testing()

set(EPOCANVA_STAGED_PRODUCT_ROOT "" CACHE PATH "Exact product root prepared by stage_resources.py")
if(NOT IS_ABSOLUTE "${EPOCANVA_STAGED_PRODUCT_ROOT}")
  message(FATAL_ERROR "Supply the absolute EPOCANVA_STAGED_PRODUCT_ROOT; no data search or download is performed")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction"
                 "${CMAKE_CURRENT_BINARY_DIR}/prediction-core")
add_executable(epocanva_staged_abbreviations_probe
  main.cc "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction/abbreviation.cc")
target_link_libraries(epocanva_staged_abbreviations_probe PRIVATE epocanva_prediction_core)
set_target_properties(epocanva_staged_abbreviations_probe PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_staged_abbreviations_probe PRIVATE /utf-8 /permissive- /W4 /WX /EHsc)
else()
  target_compile_options(epocanva_staged_abbreviations_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME backend-resource-staging.native-decoder
  COMMAND epocanva_staged_abbreviations_probe "${EPOCANVA_STAGED_PRODUCT_ROOT}")
set_tests_properties(backend-resource-staging.native-decoder PROPERTIES TIMEOUT 60)
