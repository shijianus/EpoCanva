// EpoCanva W2-B -- the real TSF candidate UI element and its presentation window.
//
// WHAT TSF SEES. One ITfCandidateListUIElement per candidate menu, registered with the
// thread's ITfUIElementMgr through BeginUIElement/UpdateUIElement/EndUIElement, plus an
// ITfUIElementSink so an End issued by TSF is honoured. BeginUIElement's pbShow is the
// host's decision: when the host answers FALSE, this class never shows its own window and
// still answers the candidate-list queries; it sends the initial UpdateUIElement the
// UILess contract requires. A later ITfUIElement::Show(TRUE) is the only way the host can
// hand presentation back to the TIP.
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
  // The collapsed caret ACP (UTF-16 code-unit offset) the menu was drawn at. The
  // coordinator and the writer both refuse a selection from another position.
  LONG caret_utf16 = 0;
};

// The controller implements this. All calls are on the STA.
class CandidateUiHost {
 public:
  virtual ~CandidateUiHost() {}
  virtual void OnUiSelectionChanged(const CandidateUiSelection& selection) = 0;
  virtual void OnUiFinalize(const CandidateUiSelection& selection) = 0;
  virtual void OnUiAbort(std::uint64_t snapshot_version, std::uint64_t context_id,
                         std::uint64_t generation) = 0;
  // The host called ITfUIElement::Show(TRUE) after suppressing TIP drawing. The controller
  // re-reads the live anchor on the next paced wake; nothing is shown from this callback.
  virtual void OnUiShowRequested() = 0;
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
  std::uint64_t host_drawn_updates = 0;
  std::uint64_t show_false_notifications = 0;
  std::uint64_t show_true_notifications = 0;
  // W3-B fallback accounting. `no_manager_draws` counts menus presented while the thread
  // had no ITfUIElementMgr; `always_self_draws` counts menus presented against a
  // pbShow=FALSE host decision; `host_show_false_suppressions` counts explicit Show(FALSE)
  // calls that kept the window hidden even under kAlwaysSelf.
  std::uint64_t no_manager_draws = 0;
  std::uint64_t always_self_draws = 0;
  std::uint64_t host_show_false_suppressions = 0;
};

// W3-B: how the TIP treats the host's presentation decision.
//   kFollowHost  pbShow=TRUE draws, pbShow=FALSE draws nothing; a thread without an
//                ITfUIElementMgr still gets a window (the TSF-sanctioned downgrade).
//   kAlwaysSelf  additionally draws when the host claims the element with pbShow=FALSE.
//                An explicit ITfUIElement::Show(FALSE) always wins over this preference.
enum class CandidateDisplayPreference {
  kFollowHost = 0,
  kAlwaysSelf = 1,
};

class CandidateUi {
 public:
  CandidateUi(TfClientId client_id, CandidateUiHost* host, HMODULE module,
              CandidateDisplayPreference preference = CandidateDisplayPreference::kFollowHost);
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
  // Applied from the next Update onward. The setting is read on the engine-start thread
  // and published atomically, so the STA never reads settings.ini during key handling.
  void SetDisplayPreference(CandidateDisplayPreference preference) {
    preference_ = preference;
  }
  void SetSelection(int selected);
  void End();

  bool tsf_manager() const { return manager_ != nullptr; }
  bool begun() const { return begun_; }
  bool host_drawn() const { return host_drawn_; }
  // FALSE while the host draws or called Show(FALSE). Only a host Show(TRUE) re-enables
  // TIP presentation; a process-name or host heuristic never does.
  bool tip_ui_allowed() const { return !begun_ || tip_ui_allowed_; }
  // Whether this menu may present the TIP window right now. This is the host decision
  // plus the user preference plus the thread's UI-element capability; it is the only
  // predicate the controller uses for anchor reads.
  bool self_draw_permitted() const;
  // Whether menu keys (arrows/Enter/Escape) belong to the product. A thread without any
  // UI element manager still runs the menu through the controller (state C); the
  // controlled direct-sink build uses it too.
  bool interaction_active() const { return manager_ == nullptr || self_draw_permitted(); }
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
  void PrepareBridge();
  void FinalizeFromElement(const CandidateUiSelection& selection);
  void SelectionFromElement(const CandidateUiSelection& selection);
  void AbortFromElement(std::uint64_t snapshot_version, std::uint64_t context_id,
                        std::uint64_t generation);
  void HideWindowFromElement();
  void OnElementShow(bool show);
  bool ElementShown() const;
  void ReleaseElement();

  TfClientId client_id_ = 0;
  CandidateUiHost* host_ = nullptr;
  HMODULE module_ = nullptr;
  CandidateDisplayPreference preference_ = CandidateDisplayPreference::kFollowHost;
  // A thread without ITfUIElementMgr is the TSF-sanctioned case for the TIP drawing its
  // own window. The controlled direct-sink build is not a real TSF host and keeps it off.
  bool no_manager_self_draw_ = true;
  ITfUIElementMgr* manager_ = nullptr;
  ITfSource* source_ = nullptr;
  DWORD sink_cookie_ = 0;
  ITfUIElementSink* sink_ = nullptr;
  CandidateUiElement* element_ = nullptr;
  std::shared_ptr<CandidateUiBridge> bridge_;
  DWORD element_id_ = 0;
  bool begun_ = false;
  bool host_drawn_ = false;
  bool host_show_false_ = false;
  bool tip_ui_allowed_ = true;
  CandidateWindow window_;
  CandidateUiDiagnostics diagnostics_;
};

// Reads the collapsed default-selection caret with a synchronous read-only TSF edit
// session. Returns false when no legal session or no collapsed caret exists. Never reads
// document text.
bool ReadCaretPosition(ITfContext* context, TfClientId client_id, LONG* caret_utf16,
                       std::wstring* why);

// Reads the screen rectangle of the host token [caret_utf16 - token_units_utf16,
// caret_utf16) with a synchronous read-only TSF edit session. The session first verifies
// that the live default selection is collapsed at exactly `caret_utf16`; a moved caret
// yields no rectangle, so the window is hidden rather than placed at a guessed position.
// `clipped` reports ITfContextView::GetTextExt's own clipped flag. Never reads text.
// `observed_caret_utf16` (optional) receives the live caret even when the rectangle is
// unavailable, so the caller can distinguish "moved" from "no layout".
bool ReadTokenRect(ITfContext* context, TfClientId client_id, LONG caret_utf16,
                   LONG token_units_utf16, RECT* rect, bool* clipped,
                   LONG* observed_caret_utf16, std::wstring* why);

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_UI_H_
