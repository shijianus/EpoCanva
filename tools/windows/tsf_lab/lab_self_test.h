// EpoCanva P3.11/P3.13 -- automatic, non-interactive checks for the TSF lab.
//
// The window-lifecycle case creates and destroys the real lab window without a session and
// with no UI shown. The run-root case needs no engine or window: it exercises the guarded
// fresh-directory preparation in an isolated area below the build-local trusted root and
// checks that existing sentinels are never removed. The scenarios case creates a hidden lab
// window with the real TSF host and the real English engine, drives all nine fixed scenarios
// with programmatic ITfThreadMgr::SetFocus (so it never claims a real OnSetFocus
// notification), then tears everything down and asserts the reference/runtime balance.
#ifndef EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SELF_TEST_H_
#define EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SELF_TEST_H_

#include <string>

#include "lab_session.h"

namespace epocanva {
namespace tsf {
namespace lab {

// `case_name`: "window-lifecycle", "run-root", "scenarios", or "all".
int RunLabSelfTest(const LabConfig& config, const std::string& case_name);

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SELF_TEST_H_
