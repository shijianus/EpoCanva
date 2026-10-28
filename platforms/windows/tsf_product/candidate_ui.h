// EpoCanva W2-B -- the real TSF candidate UI element and its presentation window.
//
// WHAT TSF SEES. One ITfCandidateListUIElement per candidate menu, registered with the
// thread's ITfUIElementMgr through BeginUIElement/UpdateUIElement/EndUIElement, plus an
// ITfUIElementSink so an End issued by TSF is honoured. BeginUIElement's pbShow is the
// host's decision: when the host says it draws the element, this class never creates or
// shows its own window, and still answers the candidate-list queries.
//
// WHERE THE COPY COMES FROM. CandidateUi::Update receives one immutable CandidateSnapshot
// from the coordinator. The element keeps its own copy of the strings; nothing here calls
// the engine, submits a key or writes document text. A window click is forwarded to the
// controller as "finalize this version and index" and is validated there before any engine
// call.
//
// PLACEMENT. The caller supplies a caret rectangle read from a real TSF text-extent query.
// A missing/empty/invisible rectangle hides the window; no mouse position or screen corner
// is ever used as a substitute.
//
// LIFETIME. Everything here runs on the STA that owns the thread manager. Stop() ends the
// element, unadvises the sink and destroys the window. COM references the host may still
// hold keep the element object alive, but its bridge is marked inactive so it can no longer
// call back into a destroyed controller.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_UI_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_UI_H_

#include <windows.h>
#include <msctf.h>
#include <oleauto.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "candidate_snapshot.h"
#include "candidate_window.h"

namespace epocanva {
namespace tsf {
namespace product {

class CandidateUiElement;

// One UI-originated selection. context_id/generation are the values of the snapshot the UI
// was drawn from, not the current binding: the coordinator refuses the request when either
// differs, so a click from an old document can never reach a new one.
struct CandidateUiSelection {
  std::uint64_t snapshot_version = 0;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  int page_index = -1;
};

// The controller implements this. All calls are on the STA.
class CandidateUiHost {
 public:
  virtual ~CandidateUiHost() {}
  virtual void OnUiSelectionChanged(const CandidateUiSelection& selection) = 0;
  virtual void OnUiFinalize(const CandidateUiSelection& selection) = 0;
  virtual void OnUiAbort(std::uint64_t snapshot_version, std::uint64_t context_id,
                         std::uint64_t generation) = 0;
};

struct CandidateUiBridge;

struct CandidateUiDiagnostics {
  bool tsf_manager = false;
  bool sink_advised = false;
  bool begun = false;
  bool host_drawn = false;
  bool window_visible = false;
  bool window_created = false;
  std::uint64_t updates = 0;
  std::uint64_t begins = 0;
  std::uint64_t ends = 0;
  std::uint64_t begin_failures = 0;
  std::uint64_t update_failures = 0;
  std::uint64_t clicks = 0;
  std::uint64_t no_anchor_hides = 0;
  std::uint64_t selected_changes = 0;
};

class CandidateUi {
 public:
  CandidateUi(TfClientId client_id, CandidateUiHost* host, HMODULE module);
  ~CandidateUi();

  CandidateUi(const CandidateUi&) = delete;
  CandidateUi& operator=(const CandidateUi&) = delete;

  // QIs the thread's ITfUIElementMgr and advises the element sink. Returns false only for
  // a structural failure (no module/host); a thread manager without a UI element manager
  // is a valid "no TSF UI" configuration for controlled tests.
  bool Start(ITfThreadMgr* thread_manager, std::wstring* why);
  // Controlled test entry: the fake manager and source are already resolved. `source` may
  // be null; the element then runs without an element-sink advice.
  bool StartWithManagerForTest(ITfUIElementMgr* manager, ITfSource* source,
                               std::wstring* why);
  void Stop();

  // Copies one snapshot into the element (begin or update) and places the window when TSF
  // permits TIP drawing. `anchor` may be null: then the window is hidden, while the element
  // still answers queries. `selected` < 0 means no explicit user selection.
  void Update(const coordinator::CandidateSnapshot& snapshot, ITfContext* context,
              const RECT* anchor, int selected);
  void SetSelection(int selected);
  void End();

  bool tsf_manager() const { return manager_ != nullptr; }
  bool begun() const { return begun_; }
  bool host_drawn() const { return host_drawn_; }
  bool window_visible() const { return window_.visible(); }
  HWND window_for_test() const { return window_.window_for_test(); }
  CandidateUiDiagnostics Diagnostics() const;

  // ITfUIElementSink dispatch targets (called by the COM sink object).
  void OnHostBegin(DWORD element_id);
  void OnHostUpdate(DWORD element_id);
  void OnHostEnd(DWORD element_id);

 private:
  friend class CandidateUiElement;

  bool AttachManager(ITfUIElementMgr* manager, ITfSource* source, std::wstring* why);
  void FinalizeFromElement(const CandidateUiSelection& selection);
  void SelectionFromElement(const CandidateUiSelection& selection);
  void AbortFromElement(std::uint64_t snapshot_version, std::uint64_t context_id,
                        std::uint64_t generation);
  void HideWindowFromElement();
  void ReleaseElement();

  TfClientId client_id_ = 0;
  CandidateUiHost* host_ = nullptr;
  HMODULE module_ = nullptr;
  ITfUIElementMgr* manager_ = nullptr;
  ITfSource* source_ = nullptr;
  DWORD sink_cookie_ = 0;
  ITfUIElementSink* sink_ = nullptr;
  CandidateUiElement* element_ = nullptr;
  std::shared_ptr<CandidateUiBridge> bridge_;
  DWORD element_id_ = 0;
  bool begun_ = false;
  bool host_drawn_ = false;
  CandidateWindow window_;
  CandidateUiDiagnostics diagnostics_;
};

// Reads the caret rectangle of the composition range [start_utf16, start+length) with a
// synchronous read-only TSF edit session. Returns false when no legal session or no
// non-empty, visible rectangle exists. `clipped` reports ITfContextView::GetTextExt's own
// clipped flag. Never reads document text.
bool ReadCompositionRect(ITfContext* context, TfClientId client_id, LONG start_utf16,
                         LONG length_utf16, RECT* rect, bool* clipped,
                         std::wstring* why);

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_UI_H_
