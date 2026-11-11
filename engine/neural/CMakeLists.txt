cmake_minimum_required(VERSION 3.25)
project(epocanva_neural_reranker LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The optional reranker currently requires Windows local-file verification.")
endif()

set(LLAMA_SOURCE "" CACHE PATH "Pinned llama.cpp b4514 source directory")
if(NOT LLAMA_SOURCE)
  set(LLAMA_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../build/backend-model-acquire/llama.cpp-b4514/source")
endif()
if(NOT EXISTS "${LLAMA_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Missing pinned offline llama.cpp source at ${LLAMA_SOURCE}")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static CPU-only library" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "Disable common helpers" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Disable upstream tests" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Disable all executables and server" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Disable server" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "No model URL downloader" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "No dynamic backend DLL loading" FORCE)
set(GGML_CPU ON CACHE BOOL "CPU backend only" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "Portable baseline CPU instructions" FORCE)
set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "One portable CPU backend" FORCE)
set(GGML_AVX OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_AVX2 OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_AVX512 OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_AVX512_VBMI OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_AVX512_VNNI OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_AVX512_BF16 OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_FMA OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_F16C OFF CACHE BOOL "Portable baseline CPU" FORCE)
set(GGML_LLAMAFILE OFF CACHE BOOL "Avoid bundled assembly extension" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "Avoid optional OpenMP dependency" FORCE)
set(GGML_BLAS OFF CACHE BOOL "No external math library" FORCE)
set(GGML_CUDA OFF CACHE BOOL "No CUDA" FORCE)
set(GGML_CUDA_GRAPHS OFF CACHE BOOL "No CUDA" FORCE)
set(GGML_HIP OFF CACHE BOOL "No HIP" FORCE)
set(GGML_SYCL OFF CACHE BOOL "No SYCL" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "No Vulkan" FORCE)
set(GGML_OPENCL OFF CACHE BOOL "No OpenCL" FORCE)
set(GGML_RPC OFF CACHE BOOL "No network RPC" FORCE)
set(GGML_KOMPUTE OFF CACHE BOOL "No Kompute submodule" FORCE)
set(GGML_MUSA OFF CACHE BOOL "No MUSA" FORCE)
set(GGML_CANN OFF CACHE BOOL "No CANN" FORCE)
set(GGML_METAL OFF CACHE BOOL "No Metal" FORCE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Apply the project-owned strict flags to the pinned source subtree without editing it.
# The upstream CMake files otherwise leave MSVC exception unwinding disabled.
if(MSVC)
  add_compile_options(/utf-8 /permissive- /EHsc)
endif()

set(NEURAL_MODEL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/backend-neural/models/qwen2.5-0.5b-base-q8_0.gguf"
    CACHE FILEPATH "Absolute local Qwen Q8_0 GGUF used by the real-model test")
if(NOT IS_ABSOLUTE "${NEURAL_MODEL_PATH}" OR NOT EXISTS "${NEURAL_MODEL_PATH}")
  message(FATAL_ERROR "Missing absolute local model resource: ${NEURAL_MODEL_PATH}")
endif()

add_subdirectory("${LLAMA_SOURCE}" "llama-pinned")

add_library(epocanva_neural STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/lexical_reranker.cc")
target_include_directories(epocanva_neural PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(epocanva_neural PUBLIC cxx_std_17)
target_link_libraries(epocanva_neural PUBLIC llama bcrypt)
set_target_properties(epocanva_neural PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_neural PRIVATE /utf-8 /permissive- /W4 /EHsc /MD)
endif()

add_executable(neural_reranker_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/neural_reranker/main.cc")
target_link_libraries(neural_reranker_tests PRIVATE epocanva_neural)
target_compile_features(neural_reranker_tests PRIVATE cxx_std_17)
set_target_properties(neural_reranker_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(neural_reranker_tests PRIVATE /utf-8 /permissive- /W4 /EHsc /MD)
endif()

enable_testing()
add_test(NAME neural.reranker.real_model
  COMMAND neural_reranker_tests "${NEURAL_MODEL_PATH}")
set_tests_properties(neural.reranker.real_model PROPERTIES TIMEOUT 300)
add_test(NAME neural.reranker.can_fail
  COMMAND "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:neural_reranker_tests>"
    "-DPASS_ARGS=--force-failure"
    "-DEXPECTED_EXIT=1"
    "-DMARKER_POLICY=require"
    "-DMARKER=FAIL forced_assertion_failure"
    "-DCASE=neural.reranker.can_fail"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_completion/assert_exit.cmake")
set_tests_properties(neural.reranker.can_fail PROPERTIES TIMEOUT 30)
