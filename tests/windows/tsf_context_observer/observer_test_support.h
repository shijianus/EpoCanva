// EpoCanva P3.10 -- shared scaffolding for the context-observer verification program.
//
// TEST CODE ONLY. It reuses the P3.7 real TSF host (tests/windows/tsf_edit) and adds three
// test-owned pieces, none of which exists in the product:
//   * a recording ContextObserverSink (values only; it stores no COM pointer);
//   * a real external edit session driven with a DIFFERENT TfClientId, so the observer's
//     InWriteSession-based own/external classification is exercised against a real other
//     client rather than against "the same thread";
//   * real focus helpers (SetFocus / AssociateFocus / GetFocus) and a visible-window
//     helper for the interactive focus mode.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TEST_SUPPORT_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TEST_SUPPORT_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <string>
#include <vector>

#include "context_observer.h"
#include "context_registry.h"
#include "selection_reader.h"
#include "test_host.h"

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

using epocanva::tsf::edit::test::TestDocument;
using epocanva::tsf::edit::test::TestDocumentOptions;
using epocanva::tsf::edit::test::TestHost;
using epocanva::tsf::edit::test::TestTextStore;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;
// A real-focus case that the environment cannot exercise: reported as SKIPPED by CTest,
// never as PASS.
constexpr int kExitEnvironmentLimited = 77;

extern int g_total;
extern int g_failed;

void Check(bool condition, const std::string& what, const std::string& got = "");
// Compares document text without echoing it: a failure reports the two lengths and the
// first differing UTF-16 code-unit offset only.
void CheckDocText(const std::wstring& actual, const std::wstring& expected,
                  const std::string& what);
std::string DescribeObserverSnapshot(const ContextObserverSnapshot& snapshot);
std::string DescribeSelection(const SelectionSnapshot& selection);
int Finish();

// --- recording sink (values only) -------------------------------------------------------

struct SinkEvent {
  enum class Kind { kBound = 0, kLost, kOwnEdit };
  Kind kind = Kind::kBound;
  ContextDropReason reason = ContextDropReason::kNone;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  LONG caret_utf16 = 0;
  SelectionSnapshot selection;
};

class RecordingSink final : public ContextObserverSink {
 public:
  void OnTargetLost(ContextDropReason reason) override;
  void OnTargetBound(ITfContext* context, const ObservedTarget& target) override;
  void OnOwnEditCompleted(ITfContext* context, const ObservedTarget& target,
                          const SelectionSnapshot& selection) override;

  const std::vector<SinkEvent>& events() const { return events_; }
  std::size_t Count(SinkEvent::Kind kind) const;
  bool has_target() const { return has_target_; }
  ObservedTarget last_target() const { return last_target_; }
  bool last_context_non_null() const { return last_context_non_null_; }
  SelectionSnapshot last_own_selection() const { return last_own_selection_; }

 private:
  std::vector<SinkEvent> events_;
  bool has_target_ = false;
  ObservedTarget last_target_{};
  // True when the last callback received a non-null context; no pointer is stored.
  bool last_context_non_null_ = false;
  SelectionSnapshot last_own_selection_{};
};

// --- external edits with a separate real client -----------------------------------------

// Real TSF composition enumeration: ITfContext::QueryInterface(IID_ITfContextComposition)
// -> EnumCompositions(). Returns the number of live composition views on this context. This
// is the state evidence that distinguishes "no underline is drawn" from "the composition
// really ended"; it is a read-only query and needs no edit cookie.
HRESULT CountCompositions(ITfContext* context, ULONG* count);

enum class ExternalEditMode {
  kSetCollapsedSelection = 0,
  kInsertText,
  kSetRangeSelection,
};

struct ExternalEditResult {
  HRESULT request_hr = E_FAIL;
  HRESULT session_hr = E_FAIL;  // RequestEditSession's phrSession answer
  HRESULT edit_hr = E_FAIL;     // DoEditSession's own answer
  bool completed = false;
  bool async = false;
};

// A different real TfClientId from ITfClientId::GetClientId(TEST_CLSID). Tests assert it
// differs from the host's Activate() id; the observer must classify its writes external.
TfClientId ExternalClientId(ITfThreadMgr* thread_manager, std::string* error);

// Runs one real read/write edit session with `client`. In kInsertText mode `text` is
// inserted at [start_utf16, start_utf16); in selection modes the ACP extent is
// [start_utf16, end_utf16). All offsets are UTF-16 code units.
bool RunExternalEdit(TestHost& host, TestDocument* document, TfClientId client,
                     ExternalEditMode mode, LONG start_utf16, LONG end_utf16,
                     const std::wstring& text, ExternalEditResult* result,
                     std::string* error);

// Runs the same real read/write edit session with the observer's OWN client id (the host's
// Activate id), for the own-edit cases. This is the test's stand-in for a coordinator
// write; the product path reaches OnEndEdit through CompositionEditSession instead.
bool RunOwnEdit(TestHost& host, TestDocument* document, ExternalEditMode mode,
                LONG start_utf16, LONG end_utf16, const std::wstring& text,
                ExternalEditResult* result, std::string* error);

// --- observer driving ---------------------------------------------------------------------

struct ObserverDriveResult {
  bool settled = false;
  ContextObserverSnapshot final_state;
};

// Calls Pump and bounded-waits on the observer's real selection completion fact while a
// read is pending. Settles when no bind is owed. Never sleeps to guess a result.
ObserverDriveResult PumpUntilSettled(ContextObserver& observer, TestHost& host,
                                     DWORD timeout_ms);
// Dispatches queued STA messages for at most `timeout_ms` without requiring a completion
// fact. Used only to give TSF a chance to deliver a focus notification.
void PumpMessagesFor(TestHost& host, DWORD timeout_ms);

// --- real focus helpers -------------------------------------------------------------------

struct FocusCallResult {
  HRESULT set_focus_hr = E_FAIL;
  bool get_focus_ok = false;
  HRESULT get_focus_hr = E_FAIL;
};

// ITfThreadMgr::SetFocus(dm). This is a real manager call; whether it delivers
// OnSetFocus is the environment question the real-focus case records.
bool SetThreadFocus(TestHost& host, TestDocument* document, FocusCallResult* result,
                    std::string* error);
// S_OK/S_FALSE/... plus whether a document manager is focused.
HRESULT ReadThreadFocus(TestHost& host, bool* has_document);
// Canonical comparison of the TSF focus with this document's manager.
bool IsDocumentFocused(TestHost& host, TestDocument* document);

// Creates a real visible top-level window. Fails in environments that do not allow one
// (the real-focus case reports that as environment-limited). The caller destroys it.
bool CreateVisibleTestWindow(HWND* window, std::string* error);

// P3.13 TEST SCAFFOLDING. Creates one off-screen focusable window owned by this STA and
// puts real Win32 focus on it. This is a Win32 focus fact, not a TSF notification: the
// production guard labels it "win32-thread-focus-revalidation". Controlled cases use it
// because ITfThreadMgr::IsThreadFocus reports S_FALSE in this non-registered lab process;
// the focus sink's notifications are never synthesized. Idempotent.
bool EnsureTestThreadFocus(std::string* error);
void ClearTestThreadFocus();
HWND TestThreadFocusWindow();

// Associates a document manager with a window using ITfThreadMgr::AssociateFocus. Per the
// documented contract the manager does NOT AddRef pdim_new; *previous receives the
// previously associated manager (AddRef'd) that the caller must release. Clearing uses the
// same call with a null manager and returns the old association the same way.
HRESULT AssociateDocumentFocus(ITfThreadMgr* thread_manager, HWND window,
                               ITfDocumentMgr* document_manager,
                               ITfDocumentMgr** previous);
void ReleaseAssociatedDocumentManager(ITfDocumentMgr* document_manager);

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TEST_SUPPORT_H_
