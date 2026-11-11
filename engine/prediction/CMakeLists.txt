cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaPredictionCore LANGUAGES CXX)
endif()

# Independent of the existing English, ABI, Rime and platform targets. Do not set
# CMAKE_MSVC_RUNTIME_LIBRARY or /MT /MD: inherit the embedding project's CRT.
add_library(epocanva_prediction_core STATIC model.cc model.h)
target_include_directories(epocanva_prediction_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_features(epocanva_prediction_core PUBLIC cxx_std_17)
set_target_properties(epocanva_prediction_core PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON FOLDER "epocanva/prediction")
if(MSVC)
  target_compile_options(epocanva_prediction_core PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
else()
  target_compile_options(epocanva_prediction_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
