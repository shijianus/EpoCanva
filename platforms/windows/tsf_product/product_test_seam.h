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
  unsigned long long raw_failed_keys;
  unsigned long long raw_failed_units;
  // W2-C host-owned text / suggestion stream accounting. Counts and lengths are text-free.
  unsigned long long host_text_keys;
  unsigned long long suggestion_queued;
  unsigned long long suggestion_submitted;
  unsigned long long suggestion_completed;
  unsigned long long suggestion_dropped;
  unsigned long long suggestion_stale;
  unsigned long long suggestion_resets;
  unsigned long long suggestion_pending;
  unsigned long long replacement_attempts;
  unsigned long long replacement_applied;
  unsigned long long replacement_refused;
  unsigned long long internal_text_losses;
  unsigned long long host_token_units;
  int suggestion_active;
  int suggestion_in_flight;
  int replacement_pending;
  int coordinator_bound;
  unsigned long long last_edit_status;
  unsigned long long last_edit_reason;
  unsigned long long pumps;
  int session_pending;
  int edit_completion_signaled;
  unsigned long long plans_submitted;
  unsigned long long edits_applied;
  unsigned long long edits_refused;
  unsigned long long edits_stale;
  unsigned long long work_kind;
  unsigned long long work_outcome;
  unsigned long long edit_pump_checks;
  unsigned long long edit_pump_ready;
  unsigned long long coordinator_pumps;
  unsigned long long host_edits_matched;
  unsigned long long host_edits_unexpected;
  unsigned long long reset_boundary;
  unsigned long long reset_owed;
  unsigned long long reset_submit_refusal;
  unsigned long long reset_replacement;
  unsigned long long target_losses;
  int observer_awaiting_bind;
  int observer_published;
  unsigned long long observer_last_drop;
  int rebind_pending;
  unsigned long long last_bind_status;
  unsigned long long binds_refused;
  unsigned long long binds_paused_retiring;
  unsigned long long observer_selection_refusals;
  unsigned long long observer_selection_reads;
  int observer_selection_pending;
  int observer_has_context;
  int observer_subscribed;
  unsigned long long retiring_bindings;
  int admission_paused;
  unsigned long long retiring_compositions;
  unsigned long long retired_settled;
  unsigned long long retired_already_ordinary;
  unsigned long long retired_blocked;
  unsigned long long close_requests;
  unsigned long long close_results;
  unsigned long long close_failures;
  unsigned long long unresolved_close;
  unsigned long long unresolved_engine;
  int pending_edit_event;
  // W3-A: the current token was abandoned after a suggestion drop and waits for a boundary.
  int token_abandoned;
  // The suggestion stream is stale right now (a reset is owed or in flight).
  int suggestion_stale_now;
  // Binding invalidations and the last observed target-loss reason (enum values).
  unsigned long long binding_invalidations;
  unsigned long long binding_last_loss_reason;
  // The current candidate snapshot's token caret (ACP, UTF-16 code units) or -1.
  long long candidate_caret;
  // The engine's raw input length for the current menu (UTF-16 code units).
  unsigned long long candidate_input_units;
  // Coordinator target revalidation totals and the last status/refusal cause.
  unsigned long long target_revalidations;
  unsigned long long target_refusals;
  unsigned long long last_target_revalidation;
  long long last_edit_request_hr;
  long long last_edit_session_hr;
  long long last_edit_operation_hr;
  unsigned long long target_callback_checks;
  unsigned long long target_rejections;
  unsigned long long permission_rechecks;
  int last_replacement_outcome;
  int last_replacement_edit_status;
  int last_replacement_edit_reason;
  long long last_replacement_expected_units;
  long long last_edit_observed_caret;
  long long last_suggestion_expected_caret;
  // W3-A: the verified menu anchor, the token-derived expected caret (origin + host units),
  // the per-token layout retry count and the token caret tracking inputs.
  long long candidate_anchor_caret;
  long long candidate_anchor_expected;
  int candidate_anchor_retries;
  int candidate_token_origin_valid;
  long long candidate_token_origin_caret;
  long long predicted_host_caret;
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
// Module lifetime facts behind DllCanUnloadNow, plus the number of live edit-layer COM
// objects counted through module_lifetime.h. Any of the pointers may be null.
__declspec(dllexport) int EpoCanvaTestModuleState(long long* live_objects,
                                                  long long* server_locks,
                                                  long long* host_active,
                                                  long long* host_failed,
                                                  long long* guarded_objects);
// Per-kind live guard counts (edit session, selection read, composition sink, rect read,
// candidate element, candidate sink). Returns the number written.
__declspec(dllexport) int EpoCanvaTestGuardedObjects(long long* out, int capacity);

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
