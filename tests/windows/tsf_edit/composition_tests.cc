// EpoCanva P3.7 -- the composition-layer verification cases.
//
// Every edit below travels the real TSF path: RequestEditSession -> ITextStoreACP RequestLock
// -> ITextStoreACPSink OnLockGranted -> ITfEditSession::DoEditSession -> ITfRange /
// ITfComposition / ITfInsertAtSelection -> the controlled test store. The fixtures never call
// the store's text methods directly; the only direct store calls are fault injection and
// diagnostics.
#include "composition_tests.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "edit_plan.h"
#include "edit_result.h"
#include "edit_session.h"
#include "test_host.h"
#include "test_support.h"

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {
namespace {

class Fixture {
 public:
  bool Init(const TestDocumentOptions& options) {
    std::string error;
    host = TestHost::Create(&error);
    if (host == nullptr) {
      Check(false, "real TSF host created", error);
      return false;
    }
    document = host->CreateDocument(options, &error);
    if (document == nullptr) {
      Check(false, "document created", error);
      return false;
    }
    session = std::make_unique<CompositionEditSession>(host->client_id());
    const bool bound = session->Bind(document->context(), document->context_id(),
                                     document->generation());
    Check(bound, "session bound to the real context", "");
    if (!bound) {
      return false;
    }
    session->SetInputPermission(InputPermission::kAllowed);
    return true;
  }

  std::unique_ptr<TestHost> host;
  TestDocument* document = nullptr;
  std::unique_ptr<CompositionEditSession> session;
};

void Evidence(const char* label, const std::string& detail) {
  std::printf("[tsf-edit] evidence %s: %s\n", label, detail.c_str());
}

std::string DescribeResult(const EditResult& result) {
  return std::string("status=") + DescribeEditStatus(result.status) +
         " reason=" + DescribeEditRejectReason(result.reason) +
         " request_hr=" + HrText(result.request_hr) +
         " phr=" + HrText(result.phr_session_hr) +
         " edit_hr=" + HrText(result.edit_session_hr) +
         " op_hr=" + HrText(result.operation_hr) +
         " end_hr=" + HrText(result.end_composition_hr);
}

void ExpectStatus(const EditResult& result, EditStatus expected,
                  const std::string& what) {
  Check(result.status == expected, what, DescribeResult(result));
}

void ExpectRejected(const EditResult& result, EditRejectReason reason,
                    const std::string& what) {
  Check(result.status == EditStatus::kRejected, what, DescribeResult(result));
  Check(result.reason == reason, what + " (reason)",
        DescribeEditRejectReason(result.reason));
}

void ExpectStale(const EditResult& result, const std::string& what) {
  Check(result.status == EditStatus::kStale, what, DescribeResult(result));
  Check(result.reason == EditRejectReason::kNotBound ||
            result.reason == EditRejectReason::kStaleGeneration ||
            result.reason == EditRejectReason::kIdentityMismatch,
        what + " (stale reason)", DescribeEditRejectReason(result.reason));
}

std::optional<EditPlan> AcceptPlan(EditPlanBuildResult built, const std::string& what) {
  Check(built.ok(), what,
        std::string(DescribeEditPlanStatus(built.status)) + " " + built.diagnostic);
  if (!built.ok()) {
    return std::nullopt;
  }
  return std::move(built.plan);
}

std::optional<EditPlan> PlanBegin(Fixture& fixture, const char* utf8, LONG caret) {
  return AcceptPlan(EditPlan::BuildBegin(fixture.session->identity(), utf8, caret, caret),
                    "Begin plan built");
}

std::optional<EditPlan> PlanUpdate(Fixture& fixture, const char* utf8) {
  return AcceptPlan(EditPlan::BuildUpdate(fixture.session->identity(), utf8),
                    "Update plan built");
}

std::optional<EditPlan> PlanCommit(Fixture& fixture, const char* utf8, LONG caret) {
  return AcceptPlan(EditPlan::BuildCommit(fixture.session->identity(), utf8, caret, caret),
                    "Commit plan built");
}

std::optional<EditPlan> PlanCancel(Fixture& fixture) {
  return AcceptPlan(EditPlan::BuildCancel(fixture.session->identity()), "Cancel plan built");
}

TestStoreDiagnostics StoreDiag(Fixture& fixture) {
  return fixture.document->diagnostics().store;
}

std::size_t CountOccurrences(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty()) {
    return 0;
  }
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::wstring::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void CancelIfActive(Fixture& fixture) {
  if (fixture.session == nullptr || !fixture.session->active_composition()) {
    return;
  }
  const auto cancel = PlanCancel(fixture);
  if (!cancel.has_value()) {
    return;
  }
  ExpectStatus(fixture.session->ApplySync(*cancel), EditStatus::kApplied,
               "cleanup Cancel applied");
}

TestDocumentOptions Document(const std::wstring& text, LONG caret) {
  TestDocumentOptions options;
  options.initial_text_utf16 = text;
  options.initial_selection_start_utf16 = caret;
  options.initial_selection_end_utf16 = caret;
  return options;
}

// 1. Real lock and edit callback.
int RunLockCallback() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 3);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectStatus(result, EditStatus::kApplied, "Begin applied through a real lock");
  Check(result.lock_observed, "the result records that DoEditSession ran with a lock", "");
  Check(result.composition_start_utf16 == 3, "composition starts at the caret (UTF-16 3)",
        std::to_string(result.composition_start_utf16));
  Check(result.composition_length_utf16 == 1, "composition holds one UTF-16 code unit",
        std::to_string(result.composition_length_utf16));
  CheckText(fixture.document->text_utf16(), L"abchDEF",
            "the preedit is inserted through TSF");
  const TestStoreDiagnostics store = StoreDiag(fixture);
  CheckU64(store.request_lock_calls, 1, "the store received RequestLock");
  CheckU64(store.write_lock_grants, 1, "a real read/write lock was granted");
  Check(store.text_mutations == 1, "exactly one backing-text mutation",
        std::to_string(store.text_mutations));
  const CompositionEditSessionDiagnostics session = fixture.session->Snapshot();
  Check(session.callbacks >= 1, "DoEditSession ran", std::to_string(session.callbacks));
  Check(session.locks_observed >= 1, "the executor observed the granted lock",
        std::to_string(session.locks_observed));
  Check(session.applied == 1, "one plan is Applied", std::to_string(session.applied));
  Evidence("lock-callback",
           "request_lock=" + std::to_string(store.request_lock_calls) +
               " write_locks=" + std::to_string(store.write_lock_grants) +
               " mutations=" + std::to_string(store.text_mutations) +
               " callbacks=" + std::to_string(session.callbacks) +
               " locks_observed=" + std::to_string(session.locks_observed));
  CancelIfActive(fixture);
  return Finish();
}

// 2 and 3. The composition range is replaced, not appended, and the surrounding text is
// untouched on every step.
int RunUpdateScope() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 3);
  if (!begin.has_value()) {
    return Finish();
  }
  EditResult result = fixture.session->ApplySync(*begin);
  ExpectStatus(result, EditStatus::kApplied, "Begin h applied");
  CheckText(fixture.document->text_utf16(), L"abchDEF", "h at caret 3");
  Check(result.composition_start_utf16 == 3 && result.composition_length_utf16 == 1,
        "composition is [3,1)",
        std::to_string(result.composition_start_utf16) + "/" +
            std::to_string(result.composition_length_utf16));

  const auto update_he = PlanUpdate(fixture, "he");
  const auto update_hel = PlanUpdate(fixture, "hel");
  if (!update_he.has_value() || !update_hel.has_value()) {
    return Finish();
  }
  result = fixture.session->ApplySync(*update_he);
  ExpectStatus(result, EditStatus::kApplied, "Update he applied");
  CheckText(fixture.document->text_utf16(), L"abcheDEF",
            "he replaces h inside the composition range");
  Check(result.composition_start_utf16 == 3 && result.composition_length_utf16 == 2,
        "composition is [3,2)",
        std::to_string(result.composition_start_utf16) + "/" +
            std::to_string(result.composition_length_utf16));

  result = fixture.session->ApplySync(*update_hel);
  ExpectStatus(result, EditStatus::kApplied, "Update hel applied");
  const std::wstring text = fixture.document->text_utf16();
  CheckText(text, L"abchelDEF", "hel replaces he inside the composition range");
  Check(result.composition_start_utf16 == 3 && result.composition_length_utf16 == 3,
        "composition is [3,3)",
        std::to_string(result.composition_start_utf16) + "/" +
            std::to_string(result.composition_length_utf16));
  Check(text.size() == 9, "the document grew by exactly the composition length",
        std::to_string(text.size()));
  Check(text.substr(0, 3) == L"abc", "the prefix before the composition is intact", "");
  Check(text.substr(text.size() - 3) == L"DEF", "the suffix after the composition is intact",
        "");
  const TestStoreDiagnostics store = StoreDiag(fixture);
  Check(store.text_mutations == 3, "three replacements, no accumulation",
        std::to_string(store.text_mutations));
  Evidence("update-scope",
           "composition extents [3,1) -> [3,2) -> [3,3); mutations=" +
               std::to_string(store.text_mutations) + "; prefix/suffix intact");
  CancelIfActive(fixture);
  return Finish();
}

// 4. Commit leaves exactly one copy of the committed word.
int RunCommitNoDuplicate() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "hel", 3);
  const auto commit = PlanCommit(fixture, "hello", 3);
  if (!begin.has_value() || !commit.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin hel applied");
  const EditResult result = fixture.session->ApplySync(*commit);
  ExpectStatus(result, EditStatus::kApplied, "Commit hello applied");
  Check(!result.composition_active, "the composition is ended by Commit", "");
  const std::wstring text = fixture.document->text_utf16();
  CheckText(text, L"abchelloDEF", "Commit replaces the preedit with hello");
  Check(CountOccurrences(text, L"hello") == 1, "hello appears once",
        std::to_string(CountOccurrences(text, L"hello")));
  Check(text.substr(0, 3) == L"abc" && text.substr(text.size() - 3) == L"DEF",
        "prefix and suffix survive Commit", "");
  Check(!fixture.session->active_composition(), "no composition remains active", "");
  const auto document = fixture.document->diagnostics();
  CheckU64(document.composition_end_calls, 1, "the context owner saw OnEndComposition");
  const TestStoreDiagnostics store = StoreDiag(fixture);
  Check(store.text_mutations == 2, "insert then replace; no extra write",
        std::to_string(store.text_mutations));
  Evidence("commit-no-duplicate",
           "hello occurrences=" + std::to_string(CountOccurrences(text, L"hello")) +
               " mutations=" + std::to_string(store.text_mutations) +
               " end_calls=" + std::to_string(document.composition_end_calls));

  const auto update = PlanUpdate(fixture, "x");
  const auto cancel = PlanCancel(fixture);
  if (!update.has_value() || !cancel.has_value()) {
    return Finish();
  }
  const std::size_t set_calls_before = store.set_text_calls;
  const std::uint64_t ends_before = document.composition_end_calls;
  const EditResult after_commit = fixture.session->ApplySync(*update);
  ExpectRejected(after_commit, EditRejectReason::kNoComposition,
                 "Update after Commit is refused");
  Check(StoreDiag(fixture).set_text_calls == set_calls_before,
        "the refused Update wrote nothing", "");
  Check(fixture.document->diagnostics().composition_end_calls == ends_before,
        "the refused Update ended no composition on the second attempt", "");
  return Finish();
}

// 5. Cancel removes only this composition.
int RunCancelOnlyOwn() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "hel", 3);
  const auto cancel = PlanCancel(fixture);
  if (!begin.has_value() || !cancel.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin hel applied");
  CheckText(fixture.document->text_utf16(), L"abchelDEF", "preedit present before Cancel");
  const EditResult result = fixture.session->ApplySync(*cancel);
  ExpectStatus(result, EditStatus::kApplied, "Cancel applied");
  Check(!result.composition_active, "Cancel ends the composition", "");
  CheckText(fixture.document->text_utf16(), L"abcDEF",
            "Cancel removes exactly the composition text");
  Check(!fixture.session->active_composition(), "no composition remains active", "");
  const auto document = fixture.document->diagnostics();
  CheckU64(document.composition_end_calls, 1, "OnEndComposition was delivered");
  const TestStoreDiagnostics store = StoreDiag(fixture);
  Check(store.text_mutations == 2, "insert then delete",
        std::to_string(store.text_mutations));
  return Finish();
}

// 6 and 7. An ended composition object is not used again; a no-preedit commit inserts once.
int RunEndedCompositionUnused() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 3);
  const auto commit = PlanCommit(fixture, "h", 3);
  if (!begin.has_value() || !commit.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin h applied");
  ExpectStatus(fixture.session->ApplySync(*commit), EditStatus::kApplied, "Commit h applied");
  CheckText(fixture.document->text_utf16(), L"abchDEF", "committed h");
  const TestStoreDiagnostics before = StoreDiag(fixture);
  Check(!fixture.session->active_composition(), "composition already ended", "");

  const auto update = PlanUpdate(fixture, "he");
  const auto cancel = PlanCancel(fixture);
  if (!update.has_value() || !cancel.has_value()) {
    return Finish();
  }
  ExpectRejected(fixture.session->ApplySync(*update), EditRejectReason::kNoComposition,
                 "Update on an ended composition is refused");
  ExpectRejected(fixture.session->ApplySync(*cancel), EditRejectReason::kNoComposition,
                 "Cancel on an ended composition is refused");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.set_text_calls == before.set_text_calls,
        "no SetText was issued for the ended composition",
        std::to_string(after.set_text_calls) + " vs " + std::to_string(before.set_text_calls));
  Check(after.text_mutations == before.text_mutations, "the text is unchanged", "");
  CheckText(fixture.document->text_utf16(), L"abchDEF", "still exactly one h");
  return Finish();
}

int RunNoPreeditCommit() {
  Fixture fixture;
  if (!fixture.Init(Document(L"ab", 2))) {
    return Finish();
  }
  const auto commit = PlanCommit(fixture, " ", 2);
  if (!commit.has_value()) {
    return Finish();
  }
  const EditResult result = fixture.session->ApplySync(*commit);
  ExpectStatus(result, EditStatus::kApplied, "no-preedit Commit applied");
  Check(!result.composition_active, "no composition was created", "");
  const std::wstring text = fixture.document->text_utf16();
  CheckText(text, L"ab ", "the space is inserted once at the collapsed caret");
  Check(CountOccurrences(text, L" ") == 1, "exactly one inserted space",
        std::to_string(CountOccurrences(text, L" ")));
  const auto document = fixture.document->diagnostics();
  Check(document.composition_start_calls == document.composition_end_calls,
        "TSF's temporary composition around the insert is balanced",
        std::to_string(document.composition_start_calls) + "/" +
            std::to_string(document.composition_end_calls));
  const TestStoreDiagnostics store = StoreDiag(fixture);
  Check(store.insert_text_calls == 1, "one InsertTextAtSelection call",
        std::to_string(store.insert_text_calls));
  Check(store.text_mutations == 1, "one mutation",
        std::to_string(store.text_mutations));
  Check(store.set_text_calls == 0, "no SetText was used",
        std::to_string(store.set_text_calls));
  Check(StoreDiag(fixture).text_mutations == 1, "no automatic replay followed the commit",
        std::to_string(StoreDiag(fixture).text_mutations));
  return Finish();
}

// 8. Unicode prefix/suffix and suppression of a surrogate split.
int RunUnicodeBoundaries() {
  Fixture fixture;
  if (!fixture.Init(Document(L"e\u0301\U0001F600Z", 4))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "he", 4);
  const auto commit = PlanCommit(fixture, "hello", 4);
  if (!begin.has_value() || !commit.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin he applied");
  CheckText(fixture.document->text_utf16(), L"e\u0301\U0001F600heZ",
            "preedit inserted after the surrogate pair");
  const EditResult result = fixture.session->ApplySync(*commit);
  ExpectStatus(result, EditStatus::kApplied, "Commit hello applied");
  const std::wstring text = fixture.document->text_utf16();
  CheckText(text, L"e\u0301\U0001F600helloZ",
            "prefix (including the combining mark and surrogate pair) and suffix survive");
  Check(text.substr(0, 4) == std::wstring(L"e\u0301\U0001F600"),
        "the four UTF-16 code units before the caret are intact", "");
  Check(text.substr(text.size() - 1) == L"Z", "the suffix is intact", "");
  Check(CountOccurrences(text, L"\U0001F600") == 1, "the surrogate pair appears once", "");
  Check(CountOccurrences(text, L"\u0301") == 1, "the combining mark appears once", "");
  return Finish();
}

int RunSurrogateOffsetRejected() {
  Fixture fixture;
  if (!fixture.Init(Document(L"A\U0001F600B", 2))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "x", 2);
  if (!begin.has_value()) {
    return Finish();
  }
  const TestStoreDiagnostics before = StoreDiag(fixture);
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectRejected(result, EditRejectReason::kSelectionInsideSurrogatePair,
                 "a caret inside a surrogate pair is refused");
  CheckText(fixture.document->text_utf16(), L"A\U0001F600B", "the document is unchanged");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.set_text_calls == before.set_text_calls, "no SetText was attempted", "");
  Check(after.text_mutations == before.text_mutations, "no mutation occurred", "");
  Check(fixture.document->diagnostics().composition_start_calls == 0,
        "no composition was started", "");
  Check(!fixture.session->active_composition(), "no composition is active", "");

  // The plan itself also refuses a non-collapsed selection and a negative offset.
  const EditPlanBuildResult non_collapsed =
      EditPlan::BuildBegin(fixture.session->identity(), "x", 1, 2);
  Check(!non_collapsed.ok() &&
            non_collapsed.status == EditPlanStatus::kSelectionNotCollapsed,
        "a non-collapsed plan is refused at build time",
        DescribeEditPlanStatus(non_collapsed.status));
  const EditPlanBuildResult negative =
      EditPlan::BuildBegin(fixture.session->identity(), "x", -1, -1);
  Check(!negative.ok() && negative.status == EditPlanStatus::kInvalidSelection,
        "a negative UTF-16 offset is refused at build time",
        DescribeEditPlanStatus(negative.status));
  return Finish();
}

// 10a. The real context reports read-only; the layer refuses before requesting a write lock.
int RunReadOnlyRejected() {
  TestDocumentOptions options = Document(L"abc", 1);
  options.read_only = true;
  Fixture fixture;
  if (!fixture.Init(options)) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const TestStoreDiagnostics before = StoreDiag(fixture);
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectRejected(result, EditRejectReason::kReadOnlyContext,
                 "a read-only context is refused");
  CheckText(fixture.document->text_utf16(), L"abc", "the read-only text is unchanged");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.request_lock_calls == before.request_lock_calls,
        "no write lock was even requested",
        std::to_string(after.request_lock_calls) + " vs " +
            std::to_string(before.request_lock_calls));
  Check(after.get_status_calls > before.get_status_calls,
        "the real context's GetStatus was consulted", "");
  Check(after.text_mutations == 0, "no mutation occurred", "");
  Check(fixture.document->diagnostics().composition_start_calls == 0,
        "no composition was started", "");
  Evidence("read-only",
           std::string("reason=") + DescribeEditRejectReason(result.reason) +
               " request_locks_before=" + std::to_string(before.request_lock_calls) +
               " after=" + std::to_string(after.request_lock_calls) +
               " get_status_calls=" + std::to_string(after.get_status_calls));
  return Finish();
}

// 10b. The store's own write-lock refusal, independent of the status check.
int RunWriteLockRefused() {
  TestDocumentOptions options = Document(L"abc", 1);
  options.refuse_write_locks = true;
  Fixture fixture;
  if (!fixture.Init(options)) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const TestStoreDiagnostics before = StoreDiag(fixture);
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectRejected(result, EditRejectReason::kWriteLockRefused,
                 "the store's write-lock refusal is surfaced");
  CheckText(fixture.document->text_utf16(), L"abc", "the text is unchanged");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.write_lock_refusals > before.write_lock_refusals,
        "the store refused a read/write lock",
        std::to_string(after.write_lock_refusals));
  Check(after.text_mutations == 0, "no mutation occurred", "");
  Evidence("write-lock-refused",
           std::string("reason=") + DescribeEditRejectReason(result.reason) +
               " refusals_before=" + std::to_string(before.write_lock_refusals) +
               " after=" + std::to_string(after.write_lock_refusals) +
               " mutations=" + std::to_string(after.text_mutations));
  return Finish();
}

// 11. A context removed from its document stack is refused.
int RunDisconnectedRejected() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abc", 1))) {
    return Finish();
  }
  Check(fixture.document->Disconnect(), "the document left its TSF stack",
        HrText(fixture.document->disconnect_result()));
  const auto begin = PlanBegin(fixture, "h", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectRejected(result, EditRejectReason::kContextDisconnected,
                 "a disconnected context is refused");
  CheckText(fixture.document->text_utf16(), L"abc", "the text is unchanged");
  Check(StoreDiag(fixture).text_mutations == 0, "no mutation occurred", "");
  return Finish();
}

// 12. An asynchronous request stays Pending until its callback runs.
int RunAsyncPending() {
  TestDocumentOptions options = Document(L"abcDEF", 3);
  options.async_locks = true;
  Fixture fixture;
  if (!fixture.Init(options)) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 3);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult pending = fixture.session->ApplyAsync(*begin);
  ExpectStatus(pending, EditStatus::kPending, "ApplyAsync returns Pending");
  CheckHr(pending.request_hr, S_OK, "RequestEditSession returned S_OK");
  CheckHr(pending.phr_session_hr, TF_S_ASYNC,
          "phrSession is TF_S_ASYNC (waiting, not applied)");
  Check(fixture.session->HasPending(), "the session has an outstanding request", "");
  CheckText(fixture.document->text_utf16(), L"abcDEF",
            "the text is untouched while the lock is deferred");
  Check(fixture.session->Snapshot().callbacks == 0,
        "DoEditSession has not run", "");
  const TestStoreDiagnostics before_pump = StoreDiag(fixture);
  Check(before_pump.write_lock_grants == 0, "no write lock was granted yet",
        std::to_string(before_pump.write_lock_grants));
  Check(before_pump.text_mutations == 0, "no mutation happened yet", "");
  Check(fixture.session->PendingResult().status == EditStatus::kPending,
        "PendingResult still reports Pending", "");

  Check(fixture.host->PumpUntil(fixture.session->CompletionEvent(), 10000),
        "the STA message loop delivered the deferred grant", "");
  const EditResult final = fixture.session->PendingResult();
  ExpectStatus(final, EditStatus::kApplied, "the callback applied the plan");
  Check(!fixture.session->HasPending(), "the request is complete", "");
  CheckHr(final.request_hr, S_OK, "the final result keeps RequestEditSession's S_OK");
  CheckHr(final.phr_session_hr, TF_S_ASYNC,
          "the final result keeps the TF_S_ASYNC waiting phrase");
  CheckText(fixture.document->text_utf16(), L"abchDEF",
            "the deferred edit landed exactly once");
  CheckU64(StoreDiag(fixture).async_lock_grants, 1,
           "the store granted the lock asynchronously");
  CheckU64(fixture.session->Snapshot().callbacks, 1, "the callback ran once");
  Evidence("async-pending",
           std::string("pending_status=") + DescribeEditStatus(pending.status) +
               " final_status=" + DescribeEditStatus(final.status) +
               " request_hr=" + HrText(final.request_hr) +
               " phr=" + HrText(final.phr_session_hr) +
               " callbacks=" +
               std::to_string(fixture.session->Snapshot().callbacks) +
               " async_grants=" +
               std::to_string(StoreDiag(fixture).async_lock_grants));
  CancelIfActive(fixture);
  return Finish();
}

// 13. A generation change before the callback makes the old plan Stale; it writes nothing.
int RunStaleGeneration() {
  TestDocumentOptions options = Document(L"abc", 1);
  options.async_locks = true;
  Fixture fixture;
  if (!fixture.Init(options)) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult pending = fixture.session->ApplyAsync(*begin);
  ExpectStatus(pending, EditStatus::kPending, "request queued");
  fixture.session->Invalidate();  // generation changes before the callback
  Check(fixture.host->PumpUntil(fixture.session->CompletionEvent(), 10000),
        "the deferred callback still ran", "");
  const EditResult final = fixture.session->PendingResult();
  ExpectStale(final, "the old plan is Stale after the generation change");
  CheckText(fixture.document->text_utf16(), L"abc",
            "the stale plan wrote nothing");
  Check(StoreDiag(fixture).text_mutations == 0, "no mutation occurred", "");
  Check(fixture.document->diagnostics().composition_start_calls == 0,
        "no composition was started", "");
  Check(!fixture.session->active_composition(), "no composition is active", "");
  Evidence("stale-generation",
           std::string("status=") + DescribeEditStatus(final.status) +
               " reason=" + DescribeEditRejectReason(final.reason) +
               " mutations=" + std::to_string(StoreDiag(fixture).text_mutations) +
               " composition_starts=" +
               std::to_string(fixture.document->diagnostics().composition_start_calls));
  return Finish();
}

// 14. Two real contexts do not share state or text.
int RunTwoContexts() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  Check(host != nullptr, "real TSF host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options_a = Document(L"aaa", 1);
  TestDocumentOptions options_b = Document(L"bbb", 1);
  TestDocument* document_a = host->CreateDocument(options_a, &error);
  TestDocument* document_b = host->CreateDocument(options_b, &error);
  Check(document_a != nullptr && document_b != nullptr, "two real documents created", error);
  if (document_a == nullptr || document_b == nullptr) {
    return Finish();
  }
  CompositionEditSession session_a(host->client_id());
  CompositionEditSession session_b(host->client_id());
  Check(session_a.Bind(document_a->context(), document_a->context_id(),
                       document_a->generation()),
        "session A bound", "");
  Check(session_b.Bind(document_b->context(), document_b->context_id(),
                       document_b->generation()),
        "session B bound", "");
  session_a.SetInputPermission(InputPermission::kAllowed);
  session_b.SetInputPermission(InputPermission::kAllowed);

  const EditPlanBuildResult plan_a_build =
      EditPlan::BuildBegin(session_a.identity(), "x", 1, 1);
  const EditPlanBuildResult plan_b_build =
      EditPlan::BuildBegin(session_b.identity(), "y", 1, 1);
  if (!plan_a_build.ok() || !plan_b_build.ok()) {
    Check(false, "two Begin plans built", "");
    return Finish();
  }
  ExpectStatus(session_a.ApplySync(*plan_a_build.plan), EditStatus::kApplied,
               "plan A applied");
  CheckText(document_a->text_utf16(), L"axaa", "document A changed");
  CheckText(document_b->text_utf16(), L"bbb", "document B is untouched by plan A");
  ExpectStatus(session_b.ApplySync(*plan_b_build.plan), EditStatus::kApplied,
               "plan B applied");
  CheckText(document_b->text_utf16(), L"bybb", "document B changed");
  CheckText(document_a->text_utf16(), L"axaa", "document A is untouched by plan B");

  // The old plan for A must not write into B.
  const EditResult wrong = session_b.ApplySync(*plan_a_build.plan);
  ExpectStale(wrong, "a plan for context A is Stale on context B");
  Check(wrong.reason == EditRejectReason::kIdentityMismatch,
        "the stale reason is the identity mismatch",
        DescribeEditRejectReason(wrong.reason));
  CheckText(document_b->text_utf16(), L"bybb", "the stale plan wrote nothing to B");

  const EditPlanBuildResult cancel_a =
      EditPlan::BuildCancel(session_a.identity());
  const EditPlanBuildResult cancel_b =
      EditPlan::BuildCancel(session_b.identity());
  if (cancel_a.ok() && cancel_b.ok()) {
    ExpectStatus(session_a.ApplySync(*cancel_a.plan), EditStatus::kApplied,
                 "session A cancelled its composition");
    ExpectStatus(session_b.ApplySync(*cancel_b.plan), EditStatus::kApplied,
                 "session B cancelled its composition");
    CheckText(document_a->text_utf16(), L"aaa", "document A restored");
    CheckText(document_b->text_utf16(), L"bbb", "document B restored");
  }
  return Finish();
}

// 15. Write-call failures: one executor conclusion across different host truths.
//
// Every sub-case below runs the same Begin("he") whose SetText the store fails. What the
// HOST does differs -- change nothing vs. change the text, E_FAIL vs. E_UNEXPECTED -- but
// nothing of that difference is observable through TSF, so the executor must return the same
// conservative conclusion in all four: FailedPossiblyMutated, may-have-changed, unverified.
// The store's own text (host truth, read only by the test) proves the two E_FAIL cases and
// the two E_UNEXPECTED cases really differ underneath.
//
// The number of store-level GetText calls a faulted Begin may cause, observed on this build
// (Windows 10.0.26200): 1. The writer issues two one-unit boundary probes through ITfRange,
// but msctf serves part of them from its cache, so only one reaches the store. The writer's
// only other reads (selection, own-range verification) are equally bounded, and the
// classification path performs no read-back at all -- a reintroduced whole-document or
// span read-back for failure classification would push the delta above this constant.
constexpr std::uint64_t kFaultedBeginGetTextCalls = 1;

struct WriteFailureExpectation {
  MutationFault fault;
  HRESULT write_hr;                 // the HRESULT the store returns from SetText
  const wchar_t* host_text_after;   // host truth after the failed write
  std::uint64_t host_mutations;     // host truth: backing mutations caused by the write
  const char* label;
};

int RunWriteFailureCase(const WriteFailureExpectation& expectation) {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  TestTextStore* store = fixture.document->store();
  const TestStoreDiagnostics before = StoreDiag(fixture);

  store->SetMutationFault(expectation.fault);
  const auto begin = PlanBegin(fixture, "he", 3);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectStatus(result, EditStatus::kFailedPossiblyMutated,
               "a failed write call is classified as possibly mutated");
  Check(result.reason == EditRejectReason::kSetTextFailed,
        "the failed write names SetText",
        DescribeEditRejectReason(result.reason));
  CheckHr(result.operation_hr, expectation.write_hr,
          "the store's HRESULT is preserved, not interpreted");
  Check(result.text_may_have_changed && !result.mutation_verified,
        "the result claims a possible change and no knowledge", "");
  Check(result.cleanup_end_composition,
        "the composition end inside the failed Begin is recorded as cleanup", "");
  Check(!result.composition_active, "the cleanup end closed the composition object", "");
  Check(result.composition_length_utf16 == -1,
        "the composition extent is unknown after an unconfirmed write",
        std::to_string(result.composition_length_utf16));

  CheckText(fixture.document->text_utf16(), expectation.host_text_after,
            "host truth after the failed write");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.text_mutations == before.text_mutations + expectation.host_mutations,
        "host truth: the number of backing mutations",
        std::to_string(after.text_mutations));
  Check(after.set_text_calls == before.set_text_calls + 1,
        "exactly one write call was issued (no retry)",
        std::to_string(after.set_text_calls - before.set_text_calls));
  Check(after.get_text_calls == before.get_text_calls + kFaultedBeginGetTextCalls,
        "no classification read-back followed the failed write",
        std::to_string(after.get_text_calls - before.get_text_calls));
  CheckU64(fixture.document->diagnostics().composition_end_calls, 1,
           "the cleanup end was delivered to the context owner");
  const CompositionEditSessionDiagnostics session = fixture.session->Snapshot();
  CheckU64(session.failed_possibly_mutated, 1, "one PossiblyMutated outcome was counted");

  // The session is now terminal for editing, but this case leaves the refusal details to
  // edit-uncertain-state; here only the counter evidence is recorded.
  Evidence(expectation.label,
           std::string("status=") + DescribeEditStatus(result.status) +
               " op_hr=" + HrText(result.operation_hr) +
               " may_change=" + (result.text_may_have_changed ? "1" : "0") +
               " verified=" + (result.mutation_verified ? "1" : "0") +
               " cleanup_end=" + (result.cleanup_end_composition ? "1" : "0") +
               " host_mutations=" + std::to_string(after.text_mutations) +
               " set_text_delta=1 get_text_delta=" +
               std::to_string(after.get_text_calls - before.get_text_calls));
  return Finish();
}

int RunFailEFailUnchanged() {
  const WriteFailureExpectation expectation{
      MutationFault::kEFailWithoutMutation, E_FAIL, L"abcDEF", 0,
      "failure-efail-unchanged"};
  return RunWriteFailureCase(expectation);
}

int RunFailEFailMutated() {
  const WriteFailureExpectation expectation{
      MutationFault::kEFailAfterMutation, E_FAIL, L"abcheDEF", 1,
      "failure-efail-mutated"};
  return RunWriteFailureCase(expectation);
}

int RunFailEUnexpectedUnchanged() {
  const WriteFailureExpectation expectation{
      MutationFault::kEUnexpectedWithoutMutation, E_UNEXPECTED, L"abcDEF", 0,
      "failure-eunexpected-unchanged"};
  return RunWriteFailureCase(expectation);
}

int RunFailEUnexpectedMutated() {
  const WriteFailureExpectation expectation{
      MutationFault::kEUnexpectedAfterMutation, E_UNEXPECTED, L"abcheDEF", 1,
      "failure-eunexpected-mutated"};
  return RunWriteFailureCase(expectation);
}

// 15b. After an unconfirmed write failure the session refuses everything and clears nothing.
int RunUncertainStateRefusal() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  TestTextStore* store = fixture.document->store();
  store->SetMutationFault(MutationFault::kEFailWithoutMutation);
  const auto begin = PlanBegin(fixture, "he", 3);
  if (!begin.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kFailedPossiblyMutated,
               "the seeding failure is possibly mutated");
  store->SetMutationFault(MutationFault::kNone);
  const TestStoreDiagnostics after_failure = StoreDiag(fixture);

  const auto update = PlanUpdate(fixture, "x");
  const auto commit = PlanCommit(fixture, "hello", 3);
  const auto cancel = PlanCancel(fixture);
  const auto restart = PlanBegin(fixture, "z", 3);
  if (!update.has_value() || !commit.has_value() || !cancel.has_value() ||
      !restart.has_value()) {
    return Finish();
  }
  ExpectRejected(fixture.session->ApplySync(*update), EditRejectReason::kUncertainTextState,
                 "Update after an unconfirmed failure is refused");
  ExpectRejected(fixture.session->ApplySync(*commit), EditRejectReason::kUncertainTextState,
                 "Commit after an unconfirmed failure is refused");
  ExpectRejected(fixture.session->ApplySync(*cancel), EditRejectReason::kUncertainTextState,
                 "Cancel after an unconfirmed failure is refused");
  ExpectRejected(fixture.session->ApplySync(*restart),
                 EditRejectReason::kUncertainTextState,
                 "a fresh Begin after an unconfirmed failure is refused");
  Check(!fixture.session->Bind(fixture.document->context(), fixture.document->context_id(),
                               fixture.document->generation() + 1),
        "re-binding an uncertain session is refused", "");

  const TestStoreDiagnostics after_refusals = StoreDiag(fixture);
  Check(after_refusals.set_text_calls == after_failure.set_text_calls,
        "no write call followed the refusals (no retry, no replay)",
        std::to_string(after_refusals.set_text_calls));
  Check(after_refusals.request_lock_calls == after_failure.request_lock_calls,
        "the refusals requested no lock at all",
        std::to_string(after_refusals.request_lock_calls));
  Check(after_refusals.get_text_calls == after_failure.get_text_calls,
        "the refusals read nothing back", "");
  CheckText(fixture.document->text_utf16(), L"abcDEF",
            "the uncertain session left the text exactly as the host truth says");

  fixture.session->Close();
  CheckText(fixture.document->text_utf16(), L"abcDEF",
            "Close cleared no text and ended no composition by itself");
  const CompositionEditSessionDiagnostics closed = fixture.session->Snapshot();
  Check(closed.rejected >= 4, "the four refusals were counted",
        std::to_string(closed.rejected));
  Evidence("uncertain-state",
           "refusals=" + std::to_string(closed.rejected) +
               " set_text_delta=0 lock_delta=0 get_text_delta=0 close_cleared=nothing");
  return Finish();
}

// 15c. Write call succeeds, then a later controlled step is made to fail: SetText S_OK, then
// the context owner refuses OnEndComposition. What this case observes is an empirical
// property of the platform: on this build (Windows 10.0.26200) msctf.dll does NOT propagate
// the owner's refusal to ITfComposition::EndComposition's caller, so the executor sees
// success and reports Applied. That branch documents the exact boundary where coverage stops
// -- the FailedAfterMutation-by-EndComposition path stays implemented but unverified through
// real TSF. If a future build does propagate the refusal, the first branch runs and the case
// becomes the real post-write-step-failure test: the mutation is known, the session latches
// uncertain, and nothing is rolled back or replayed.
int RunFailWriteThenStep() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  TestTextStore* store = fixture.document->store();
  const auto begin = PlanBegin(fixture, "hel", 3);
  const auto commit = PlanCommit(fixture, "hello", 3);
  if (!begin.has_value() || !commit.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin hel applied");
  store->SetFailCompositionEnd(true);
  const TestStoreDiagnostics before = StoreDiag(fixture);

  const EditResult result = fixture.session->ApplySync(*commit);
  const TestStoreDiagnostics after = StoreDiag(fixture);
  const auto document = fixture.document->diagnostics();
  CheckU64(document.composition_end_refusals, 1,
           "the context owner really refused the composition end");
  Check(after.text_mutations == before.text_mutations + 1,
        "the write call landed in the backing text exactly once",
        std::to_string(after.text_mutations - before.text_mutations));
  CheckText(fixture.document->text_utf16(), L"abchelloDEF",
            "the written text is present exactly once either way");

  if (FAILED(result.end_composition_hr)) {
    // The platform propagated the later step's failure: this is the post-write-step case.
    ExpectStatus(result, EditStatus::kFailedAfterMutation,
                 "a succeeded write followed by a failed step is AfterMutation");
    Check(result.reason == EditRejectReason::kEndCompositionFailed,
          "the later step's failure is named",
          DescribeEditRejectReason(result.reason));
    Check(result.mutation_verified && result.text_may_have_changed,
          "the mutation is known from the succeeded write call", "");
    Check(result.composition_active, "the composition object is still held", "");
    const auto update = PlanUpdate(fixture, "x");
    const auto cancel = PlanCancel(fixture);
    if (!update.has_value() || !cancel.has_value()) {
      return Finish();
    }
    ExpectRejected(fixture.session->ApplySync(*update),
                   EditRejectReason::kUncertainTextState,
                   "Update after a failed commit step is refused");
    ExpectRejected(fixture.session->ApplySync(*cancel),
                   EditRejectReason::kUncertainTextState,
                   "Cancel after a failed commit step is refused");
    Check(StoreDiag(fixture).set_text_calls == after.set_text_calls,
          "no further write call happened", "");
    fixture.session->Close();
    CheckText(fixture.document->text_utf16(), L"abchelloDEF",
              "Close cleared none of the written text");
    Evidence("write-then-step",
             std::string("propagated=1 status=") + DescribeEditStatus(result.status) +
                 " end_hr=" + HrText(result.end_composition_hr) +
                 " host_mutations=" + std::to_string(after.text_mutations));
    return Finish();
  }

  // Observed on this build: the refusal never reaches the executor. Everything it can
  // observe says success, so Applied is the honest conclusion; the refusal count above is
  // what proves the failing step really ran and really failed underneath.
  CheckHr(result.end_composition_hr, S_OK,
          "this msctf build swallows the owner's EndComposition refusal");
  ExpectStatus(result, EditStatus::kApplied,
               "the executor reports what the platform shows it");
  Check(!result.composition_active, "the composition is ended as far as TSF reports", "");
  Evidence("write-then-step",
           std::string("propagated=0 status=") + DescribeEditStatus(result.status) +
               " end_hr=" + HrText(result.end_composition_hr) +
               " owner_refusals=" + std::to_string(document.composition_end_refusals) +
               " host_mutations=" + std::to_string(after.text_mutations) +
               " note=EndComposition-failure-not-inducible-from-store");
  return Finish();
}

// 16. Denied and unknown permission are refused before any lock request.
int RunPermissionRefused() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abc", 1))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "h", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const TestStoreDiagnostics before = StoreDiag(fixture);

  fixture.session->SetInputPermission(InputPermission::kDenied);
  const EditResult denied = fixture.session->ApplySync(*begin);
  ExpectRejected(denied, EditRejectReason::kPermissionDenied,
                 "denied permission is refused");
  fixture.session->SetInputPermission(InputPermission::kUnknown);
  const EditResult unknown = fixture.session->ApplySync(*begin);
  ExpectRejected(unknown, EditRejectReason::kPermissionUnknown,
                 "unknown permission is refused");
  const TestStoreDiagnostics after = StoreDiag(fixture);
  Check(after.request_lock_calls == before.request_lock_calls,
        "neither refusal requested a lock",
        std::to_string(after.request_lock_calls) + " vs " +
            std::to_string(before.request_lock_calls));
  Check(after.set_text_calls == before.set_text_calls, "neither refusal wrote text", "");
  Check(after.text_mutations == 0, "no mutation occurred", "");
  CheckText(fixture.document->text_utf16(), L"abc", "the text is unchanged");
  const CompositionEditSessionDiagnostics session = fixture.session->Snapshot();
  Check(session.rejected == 2, "two policy rejections were counted",
        std::to_string(session.rejected));
  return Finish();
}

// Section 5. StartComposition rejected by the context owner: S_OK with a null composition.
int RunCompositionRejected() {
  TestDocumentOptions options = Document(L"abc", 1);
  options.refuse_compositions = true;
  Fixture fixture;
  if (!fixture.Init(options)) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "he", 1);
  if (!begin.has_value()) {
    return Finish();
  }
  const EditResult result = fixture.session->ApplySync(*begin);
  ExpectRejected(result, EditRejectReason::kCompositionRejected,
                 "StartComposition's S_OK + NULL is a rejection, not a composition");
  Check(!result.composition_active, "no composition object is active", "");
  CheckText(fixture.document->text_utf16(), L"abc", "the text is unchanged");
  const auto document = fixture.document->diagnostics();
  Check(document.composition_start_calls == 1, "the owner saw OnStartComposition",
        std::to_string(document.composition_start_calls));
  Check(document.composition_refusals == 1, "the owner refused the composition",
        std::to_string(document.composition_refusals));
  Check(document.composition_end_calls == 0, "no composition was ended", "");
  const TestStoreDiagnostics store = StoreDiag(fixture);
  Check(store.set_text_calls == 0, "no SetText was attempted", "");
  Check(store.text_mutations == 0, "no mutation occurred", "");
  Evidence("composition-rejected",
           std::string("reason=") + DescribeEditRejectReason(result.reason) +
               " starts=" + std::to_string(document.composition_start_calls) +
               " refusals=" + std::to_string(document.composition_refusals) +
               " ends=" + std::to_string(document.composition_end_calls) +
               " set_text_calls=" + std::to_string(store.set_text_calls));
  return Finish();
}

// 17. Composition, session, context and ThreadMgr cleanup, with the msctf retention recorded.
int RunResourceCleanup() {
  Fixture fixture;
  if (!fixture.Init(Document(L"abcDEF", 3))) {
    return Finish();
  }
  const auto begin = PlanBegin(fixture, "hel", 3);
  const auto commit = PlanCommit(fixture, "hello", 3);
  if (!begin.has_value() || !commit.has_value()) {
    return Finish();
  }
  ExpectStatus(fixture.session->ApplySync(*begin), EditStatus::kApplied, "Begin applied");
  ExpectStatus(fixture.session->ApplySync(*commit), EditStatus::kApplied, "Commit applied");
  CheckText(fixture.document->text_utf16(), L"abchelloDEF", "committed text");
  Check(!fixture.session->active_composition(), "no composition is active before cleanup", "");
  CheckU64(fixture.document->diagnostics().composition_end_calls, 1,
           "the composition was ended exactly once");
  const std::uint64_t composition_ends =
      fixture.document->diagnostics().composition_end_calls;

  fixture.session->Close();
  const CompositionEditSessionDiagnostics closed = fixture.session->Snapshot();
  Check(closed.closed && !closed.bound, "the session is closed and unbound", "");
  Check(!closed.active_composition, "the session holds no composition", "");

  fixture.host->Shutdown();
  const TestHostCleanupReport& report = fixture.host->cleanup_report();
  const TestDocumentTeardownRecord& teardown =
      fixture.document->teardown_record();
  Check(report.shutdown_called, "Shutdown ran", "");
  Check(report.com_initialized && report.com_uninitialized,
        "COM was initialized and uninitialized", "");
  Check(report.thread_manager_created && report.thread_manager_activated &&
            report.thread_manager_deactivated && report.thread_manager_released,
        "the real ThreadMgr was created, activated, deactivated and released", "");
  Check(report.all_documents_disconnected, "the document left the stack", "");
  Check(report.all_contexts_released, "the host's context references were released", "");
  Check(report.all_document_managers_released,
        "the host's document-manager references were released", "");
  Check(report.advised_store_sinks_alive == 0, "no advised store sink survived", "");
  Check(report.forced_store_sink_releases == 0,
        "the application unadvised the sink instead of forcing it", "");
  Check(report.any_composition_sink_installed,
        "TSF installed the context-owner composition sink", "");
  Check(report.document_managers_still_enumerated == 0,
        "no document manager remained enumerated", "");
  Check(SUCCEEDED(teardown.pop_hr) && SUCCEEDED(teardown.sink_unadvise_hr),
        "Pop and UnadviseSink succeeded", "");
  Check(teardown.store_sink_advised, "the manager had advised the store sink", "");
  Check(!teardown.store_sink_advised_after_pop, "Pop unadvised the manager sink", "");
  Check(teardown.store_references_before_release == 1,
        "only the host's store reference remained after Pop",
        std::to_string(teardown.store_references_before_release));
  Check(teardown.store_live_after_host_release == 0,
        "releasing the host reference destroyed the store",
        std::to_string(teardown.store_live_after_host_release));
  Check(report.store_reference_count_after_shutdown == 0,
        "the store reference count is zero after shutdown",
        std::to_string(report.store_reference_count_after_shutdown));
  Check(report.store_instances_alive == 0, "no TestTextStore instance survived",
        std::to_string(report.store_instances_alive));
  const TestStoreDiagnostics& ledger = teardown.store_diagnostics_before_release;
  Check(ledger.addref_calls == ledger.release_calls,
        "AddRef and Release are balanced once the host reference is included",
        std::to_string(ledger.addref_calls) + "/" +
            std::to_string(ledger.release_calls));
  Check(ledger.qi_text_store_calls == 1 && ledger.qi_composition_sink_calls == 1,
        "msctf queried ITextStoreACP and the composition sink once each",
        std::to_string(ledger.qi_text_store_calls) + "/" +
            std::to_string(ledger.qi_composition_sink_calls));
  Evidence("resource-cleanup",
           "store_refs=" + std::to_string(report.store_reference_count_after_shutdown) +
               " live_stores=" + std::to_string(report.store_instances_alive) +
               " sink_before=" + (teardown.store_sink_advised ? "1" : "0") +
               " sink_after_pop=" +
               (teardown.store_sink_advised_after_pop ? "1" : "0") +
               " addref=" + std::to_string(ledger.addref_calls) +
               " release=" + std::to_string(ledger.release_calls) +
               " composition_ends=" + std::to_string(composition_ends));
  fixture.host.reset();
  Check(TestHost::LiveInstanceCount() == 0, "no TestHost instance survived", "");
  return Finish();
}

int RunCanFail() {
  Check(false, "self-check: this composition claim is deliberately false", "false");
  return kExitSelfCheckFailed;
}

}  // namespace

int RunCompositionCase(const std::string& case_name) {
  if (case_name == "edit-lock-callback") {
    return RunLockCallback();
  }
  if (case_name == "edit-update-scope") {
    return RunUpdateScope();
  }
  if (case_name == "edit-commit-no-duplicate") {
    return RunCommitNoDuplicate();
  }
  if (case_name == "edit-cancel-only-own") {
    return RunCancelOnlyOwn();
  }
  if (case_name == "edit-ended-composition") {
    return RunEndedCompositionUnused();
  }
  if (case_name == "edit-no-preedit-commit") {
    return RunNoPreeditCommit();
  }
  if (case_name == "edit-unicode-boundaries") {
    return RunUnicodeBoundaries();
  }
  if (case_name == "edit-surrogate-offset") {
    return RunSurrogateOffsetRejected();
  }
  if (case_name == "edit-read-only") {
    return RunReadOnlyRejected();
  }
  if (case_name == "edit-write-lock-refused") {
    return RunWriteLockRefused();
  }
  if (case_name == "edit-disconnected") {
    return RunDisconnectedRejected();
  }
  if (case_name == "edit-async-pending") {
    return RunAsyncPending();
  }
  if (case_name == "edit-stale-generation") {
    return RunStaleGeneration();
  }
  if (case_name == "edit-two-contexts") {
    return RunTwoContexts();
  }
  if (case_name == "edit-fail-efail-unchanged") {
    return RunFailEFailUnchanged();
  }
  if (case_name == "edit-fail-efail-mutated") {
    return RunFailEFailMutated();
  }
  if (case_name == "edit-fail-eunexpected-unchanged") {
    return RunFailEUnexpectedUnchanged();
  }
  if (case_name == "edit-fail-eunexpected-mutated") {
    return RunFailEUnexpectedMutated();
  }
  if (case_name == "edit-uncertain-state") {
    return RunUncertainStateRefusal();
  }
  if (case_name == "edit-fail-write-then-step") {
    return RunFailWriteThenStep();
  }
  if (case_name == "edit-permission-refused") {
    return RunPermissionRefused();
  }
  if (case_name == "edit-composition-rejected") {
    return RunCompositionRejected();
  }
  if (case_name == "edit-resource-cleanup") {
    return RunResourceCleanup();
  }
  if (case_name == "edit-can-fail") {
    return RunCanFail();
  }
  return -1;
}

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
