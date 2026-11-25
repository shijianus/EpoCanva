add_executable(epocanva_tip_profile_policy_tests
  "${EPOCANVA_INSTALLER_ROOT}/tools/product/isolation/tip_profile_policy_tests.cc")
if(MSVC)
  set_property(TARGET epocanva_tip_profile_policy_tests PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(epocanva_tip_profile_policy_tests PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /WX /EHsc)
endif()
add_test(NAME installer.helper.ownership_policy
  COMMAND epocanva_tip_profile_policy_tests)
