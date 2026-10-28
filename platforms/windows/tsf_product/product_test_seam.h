// EpoCanva W2-B -- controlled direct-sink test seam.
//
// COMPILED ONLY INTO epocanva_tsf_direct_sink_test. The product DLL has no export of these
// symbols and never calls SetTestController. The seam exists so the controlled tests can
// assert candidate state, selection validation, the bounded raw accounting and the
// asynchronous start lifecycle without reaching into private controller state from the test
// executable. All calls are made on the STA that owns the product objects.
#ifndef EPOCANVA_PRODUCT_TEST_SEAM_H_
#define EPOCANVA_PRODUCT_TEST_SEAM_H_

#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST

#include <windows.h>

extern "C" {

struct EpoCanvaTestCounters {
  unsigned long long key_downs;
  unsigned long long eaten;
  unsigned long long literal_key_downs;
  unsigned long long raw_dropped_keys;
  unsigned long long raw_unrecoverable;
  unsigned long long raw_applied_units;
  unsigned long long raw_cancelled_units;
  unsigned long long raw_commits;
  unsigned long long raw_ignored_edits;
  unsigned long long candidate_version;
  unsigned long long candidate_items;
  unsigned long long candidate_stale;
  int candidate_menu_open;
  int candidate_selected;
  int page_no;
  int page_size;
  int is_last_page;
  int engine_ready;
  int engine_state;  // EngineBootstrapState value
  unsigned long long engine_attach_failures;
  unsigned long long queue_pending;
  unsigned long long queue_in_flight;
  unsigned long long coordinator_phase;
  unsigned long long engine_requests;
  unsigned long long engine_results;
  int raw_active;
  int raw_submitted;
  unsigned long long works_accepted;
  unsigned long long target_binds;
  unsigned long long rebind_requests;
  unsigned long long raw_entered;
  unsigned long long last_submit_status;
  unsigned long long gate;
  unsigned long long permission;
  unsigned long long composition_length;
  int composition_active;
  unsigned long long raw_reason_unapplied;
  unsigned long long raw_reason_not_candidate;
  unsigned long long raw_reason_high_water;
  unsigned long long raw_reason_admission;
  unsigned long long last_raw_composition_start;
  unsigned long long last_raw_composition_units;
  unsigned long long last_raw_input_units;
  unsigned long long last_raw_plan_units;
  unsigned long long raw_unapplied_units;
  unsigned long long raw_exits;
};

__declspec(dllexport) int EpoCanvaTestQueryCounters(EpoCanvaTestCounters* out);
// Copies one current candidate text (UTF-16 code units, NUL-terminated). Returns the code
// unit count, or -1 when no controller/menu/index exists.
__declspec(dllexport) int EpoCanvaTestCandidateText(unsigned index, wchar_t* buffer,
                                                    unsigned capacity);
// CandidateKind value (0 literal, 1 completion, 2 spelling), or -1.
__declspec(dllexport) int EpoCanvaTestCandidateKind(unsigned index);
// Delivers a UI-style selection through the same validated path a click uses.
__declspec(dllexport) int EpoCanvaTestSelectCandidate(unsigned long long snapshot_version,
                                                      int page_index);
// Replaces the engine start with one that signals `entered_event` and blocks on
// `release_event`. Returns 0 on success.
__declspec(dllexport) int EpoCanvaTestBeginBlockingStart(void* entered_event,
                                                         void* release_event);
// Clears a Ready/Failed bootstrap state so a test can request a fresh attempt.
__declspec(dllexport) int EpoCanvaTestResetBootstrap();
__declspec(dllexport) int EpoCanvaTestBootstrapState();
// EngineHostState value: 0 idle, 1 starting, 2 running, 3 stopping, 4 failed.
__declspec(dllexport) int EpoCanvaTestEngineHostState();

}  // extern "C"

namespace epocanva {
namespace tsf {
namespace product {
class ProductTextController;
void SetTestController(ProductTextController* controller);
}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_DIRECT_SINK_TEST

#endif  // EPOCANVA_PRODUCT_TEST_SEAM_H_
