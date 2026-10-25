// EpoCanva P3.11 -- the fixed, scripted scenarios shown by the TSF integration lab.
//
// WHAT THIS IS. Nine small scripts over one LabSession. Every step submits ONE controlled
// work item through the coordinator and waits for its terminal state before the next step.
// No input queue exists: a second Run while one is active returns kBusy, and the
// coordinator's own single-flight rule is asserted in the late-edit scenario. The scripts
// use fixed fixture text submitted as already-translated KeyText; they are not a keyboard
// sink and are not evidence of natural typing support.
//
// WHAT THE SCRIPTS ASSERT. Each script checks host-visible outcomes (document UTF-16
// lengths/offsets, coordinator terminals, generations, counters) and reports text-free
// details. Document content is never written to a log; the window separately displays the
// fixed fixture text.
#ifndef EPOCANVA_TOOLS_WINDOWS_TSF_LAB_SCENARIO_RUNNER_H_
#define EPOCANVA_TOOLS_WINDOWS_TSF_LAB_SCENARIO_RUNNER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "lab_session.h"

namespace epocanva {
namespace tsf {
namespace lab {

enum class ScenarioId : std::uint8_t {
  kComposeHel = 1,
  kReturnCommit = 2,
  kHelloSpace = 3,
  kBackspaceEscape = 4,
  kAbIsolation = 5,
  kLateAEdit = 6,
  kReadOnly = 7,
  kExternalEdit = 8,
  kUncertain = 9,
};

struct ScenarioResult {
  enum class Status : std::uint8_t {
    kOk = 0,
    kBusy,     // this runner or the coordinator already has an in-flight step
    kRefused,  // no session / invalid prerequisite; nothing was submitted
    kFailed,   // a scripted check failed
    kCancelled,
  };
  Status status = Status::kRefused;
  std::string detail;  // text-free: states, counts, lengths, offsets only
  bool ok() const { return status == Status::kOk; }
};

class ScenarioRunner {
 public:
  ScenarioRunner(LabSession& session, bool use_window_focus);
  ~ScenarioRunner();

  ScenarioRunner(const ScenarioRunner&) = delete;
  ScenarioRunner& operator=(const ScenarioRunner&) = delete;

  void SetProgress(std::function<void(const std::string&)> progress) {
    progress_ = std::move(progress);
  }
  void SetCancel(std::function<bool()> cancel) { cancel_ = std::move(cancel); }

  bool busy() const { return busy_; }
  ScenarioResult Run(ScenarioId id);
  static const char* Name(ScenarioId id);

 private:
  bool PrepareDocument(int index, std::string* detail);
  bool FocusDocument(int index, std::string* detail);
  bool CleanCurrentComposition(std::string* detail);
  bool TypeText(int index, const char* text, std::string* detail);
  bool TypeKey(int index, std::uint32_t virtual_key, const char* label,
               std::string* detail);
  bool SelectHello(int index, std::string* detail);
  bool Refresh(std::string* detail);
  bool DriveToTerminal(const char* label,
                       coordinator::CoordinatorSnapshot* terminal,
                       std::string* detail);
  bool CheckTextEquals(int index, const std::wstring& expected, const char* label,
                       std::string* detail);
  bool CheckTextSuffix(int index, const std::wstring& suffix, const char* label,
                       std::string* detail);
  bool CheckTextUnchanged(int index, const std::wstring& before, const char* label,
                          std::string* detail);
  void Progress(const std::string& line);
  bool Cancelled() const { return cancel_ && cancel_(); }
  DWORD StepTimeout() const;

  bool RunComposeHel(std::string* detail);
  bool RunReturnCommit(std::string* detail);
  bool RunHelloSpace(std::string* detail);
  bool RunBackspaceEscape(std::string* detail);
  bool RunAbIsolation(std::string* detail);
  bool RunLateAEdit(std::string* detail);
  bool RunReadOnly(std::string* detail);
  bool RunExternalEdit(std::string* detail);
  bool RunUncertain(std::string* detail);

  LabSession& session_;
  bool use_window_focus_ = true;
  bool busy_ = false;
  std::function<void(const std::string&)> progress_;
  std::function<bool()> cancel_;
};

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TOOLS_WINDOWS_TSF_LAB_SCENARIO_RUNNER_H_
