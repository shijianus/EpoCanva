# EpoCanva P3.1 -- the TSF lifecycle verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/tsf/CMakeLists.txt (one configure builds the DLL and this program,
# one build directory, one CTest). The guard below keeps it from being configured alone,
# where it would have no DLL to verify and no CRT agreement with it.
#
# The program reaches the DLL ONLY through an absolute path at run time (LoadLibraryW +
# GetProcAddress + the returned COM interfaces); it links neither epocanva_tsf.lib nor
# anything from the DLL. It links:
#   ole32 -- CoInitializeEx / CoCreateInstance for the real-ThreadMgr mode
#   uuid  -- the definition of CLSID_TF_ThreadMgr (msctf.h:783 declares it
#            `EXTERN_C const CLSID`; uuid.lib is the Windows SDK library that defines
#            it). Both are system libraries; nothing else is linked.
#
# identifiers.cc is compiled into this program from the DLL's own source file, so the
# product CLSID has exactly one definition in the repository and the test cannot verify
# against a drifted copy of it.
#
# The three CTest cases judge through the repository's existing wrapper
# tests/english_completion/assert_exit.cmake -- exact integer exit code plus marker
# policy -- which is a generic judgement harness despite its directory name; no
# Windows-specific rule was added to any English source, and no new test framework is
# created here (P3.1 brief, section 11).

if(NOT TARGET epocanva_tsf)
  message(FATAL_ERROR
    "tests/windows/tsf_lifecycle must be included from platforms/windows/tsf "
    "(the top-level project that builds epocanva_tsf); it is not configurable alone.")
endif()

set(EpoCanva_Tsf_Source_Dir "${CMAKE_CURRENT_LIST_DIR}/../../../platforms/windows/tsf")
if(NOT EXISTS "${EpoCanva_Tsf_Source_Dir}/identifiers.h")
  message(FATAL_ERROR
    "tsf_lifecycle: identifiers.h not found at ${EpoCanva_Tsf_Source_Dir}; the "
    "directory layout does not match the inclusion path this file assumes.")
endif()

add_executable(tsf_lifecycle
  main.cc
  "${EpoCanva_Tsf_Source_Dir}/identifiers.cc")
target_compile_definitions(tsf_lifecycle PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(tsf_lifecycle PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_lifecycle PRIVATE "${EpoCanva_Tsf_Source_Dir}")
target_link_libraries(tsf_lifecycle PRIVATE ole32 uuid)
add_dependencies(tsf_lifecycle epocanva_tsf)

enable_testing()

set(_tsf_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")
set(_tsf_dll "$<TARGET_FILE:epocanva_tsf>")

# 1. The DLL/COM lifecycle matrix, mode `lifecycle`: exports present, registration
#    exports absent, unknown CLSID refused, an unsupported IID refused at both levels
#    (DllGetClassObject and QueryInterface) with a null out-pointer and no live object
#    left in the module, a null out-parameter refused, aggregation refused, S_FALSE with
#    the factory alive, S_FALSE with a service alive (factory already gone), S_FALSE with
#    a module lock held and NO object alive, LockServer released through a second factory
#    instance with no counter underflow, S_OK after every release, five clean
#    create/destroy cycles, failed queries that leave the objects usable, and the
#    exact-DLL-path check on the loaded module.
add_test(
  NAME tsf.lifecycle.com_lifecycle
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lifecycle>
    -DCASE=com-lifecycle
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--dll;${_tsf_dll};--mode;lifecycle"
    -DTIMEOUT_SECONDS=120
    -P ${_tsf_assert}
)

# 2. The real-TSF round trip, mode `threadmgr`: COM initialised, a REAL system
#    ITfThreadMgr, real client ids, our service activated/deactivated/reactivated
#    against it, everything released in order, COM unwound. Any failed stage prints its
#    HRESULT and exits 6 -- an unsupported environment reports itself, it is never
#    folded into a pass. Nothing is registered anywhere by this case.
add_test(
  NAME tsf.lifecycle.real_threadmgr_activation
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lifecycle>
    -DCASE=real-threadmgr-activation
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--dll;${_tsf_dll};--mode;threadmgr"
    -DTIMEOUT_SECONDS=120
    -P ${_tsf_assert}
)

# 3. The falsifiability control, mode `self-check-fail`: one deliberately false claim
#    (an unknown CLSID yields a factory) must exit 14 with no RESULT PASS. Without it a
#    lifecycle run whose checks were all tautologies would be indistinguishable from
#    this one.
add_test(
  NAME tsf.lifecycle.suite_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lifecycle>
    -DCASE=self-check-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--dll;${_tsf_dll};--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_tsf_assert}
)

set_tests_properties(tsf.lifecycle.com_lifecycle
                     tsf.lifecycle.real_threadmgr_activation
                     tsf.lifecycle.suite_can_fail
  PROPERTIES TIMEOUT 180
             LABELS "windows;tsf;com;lifecycle")

message(STATUS "  tsf_lifecycle cases  : 3 (com_lifecycle, real_threadmgr_activation, "
               "suite_can_fail)")
