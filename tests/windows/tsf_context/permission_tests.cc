// EpoCanva P3.9 -- the input-permission cases.
//
// WHAT IS REAL HERE. Every case except pure-classification drives the REAL TSF manager
// through the P3.7 host: a real ITfThreadMgr/ITfDocumentMgr/ITfContext whose text store
// is the host's ITextStoreACP. Compartment values are written through the real
// ITfCompartmentMgr/ITfCompartment::SetValue path with the host's own activated client
// id; read-only state is the store's real GetStatus answer; InputScope metadata is
// published through the application's documented attribute half
// (RequestSupportedAttrs/RetrieveRequestedAttrs over TS_ATTRID GUID_PROP_INPUTSCOPE)
// and read back through the text service's documented half (GetAppProperty +
// ITfReadOnlyProperty::GetValue inside a real read-only edit session). The
// PermissionSource under test is the production CreateRealPermissionSource object; no
// resolver is mocked anywhere in this program.
//
// WHAT IS PRINTED. Diagnostics carry statuses, HRESULTs, enum values and counters only
// -- never document text (the documents here carry no private content anyway).
#include <windows.h>
#include <msctf.h>
#include <inputscope.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "permission_query.h"
#include "permission_tests.h"
#include "test_host.h"

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

using edit::test::TestDocument;
using edit::test::TestDocumentOptions;
using edit::test::TestHost;
using edit::test::TestScopeAnswer;

int g_total = 0;
int g_failed = 0;

namespace {

void Check(bool condition, const std::string& what, const std::string& got = "") {
  ++g_total;
  if (!condition) {
    ++g_failed;
    std::printf("[context-permission] FAIL %s", what.c_str());
    if (!got.empty()) {
      std::printf(" (got: %s)", got.c_str());
    }
    std::printf("\n");
  }
}

std::string Hr(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

// Prints the final line the assert_exit.cmake harness looks for and maps the failure
// count to the exit code.
int Finish() {
  if (g_failed == 0) {
    std::printf("RESULT PASS context-permission %d checks\n", g_total);
    return kExitOk;
  }
  std::printf("[context-permission] RESULT %d/%d checks, %d failed\n",
              g_total - g_failed, g_total, g_failed);
  return kExitCheckFailed;
}

std::string DescribeDecision(const PermissionDecision& decision) {
  return std::string(DescribePermissionState(decision.state)) + " deny[" +
         DescribePermissionFindings(decision.deny_findings) + "] unknown[" +
         DescribePermissionFindings(decision.unknown_findings) + "]";
}

// One real host + one document + the production permission source.
struct Case {
  std::unique_ptr<TestHost> host;
  TestDocument* document = nullptr;
  std::unique_ptr<PermissionSource> source;

  ~Case() {
    if (source != nullptr) {
      source->Abandon();
      source.reset();
    }
    if (host != nullptr) {
      host->Shutdown();
    }
  }
};

bool SetupCase(Case* setup, const TestDocumentOptions& options, const char* label) {
  std::string error;
  setup->host = TestHost::Create(&error);
  Check(setup->host != nullptr, std::string(label) + ": real TSF host created", error);
  if (setup->host == nullptr) {
    return false;
  }
  setup->document = setup->host->CreateDocument(options, &error);
  Check(setup->document != nullptr, std::string(label) + ": real document created", error);
  if (setup->document == nullptr) {
    return false;
  }
  setup->source = CreateRealPermissionSource(setup->host->client_id());
  Check(setup->source != nullptr, std::string(label) + ": production source created", "");
  return setup->source != nullptr;
}

// Runs one query to completion. A query that went async is finished by pumping the
// host's real message loop against the source's completion event -- never by sleeping.
PermissionQueryResult QueryOnce(Case& setup, const char* label) {
  PermissionQueryResult result;
  if (!setup.source->Begin(setup.document->context(), setup.document->context_id(),
                           /*generation=*/1)) {
    Check(false, std::string(label) + ": Begin accepted", "false");
    return result;
  }
  if (setup.source->Status() == PermissionQueryStatus::kPending) {
    Check(setup.host->PumpUntil(setup.source->CompletionEvent(), 20000),
          std::string(label) + ": deferred read session completed", "timeout");
  }
  Check(setup.source->Status() == PermissionQueryStatus::kReady,
        std::string(label) + ": query reached a verdict",
        DescribePermissionQueryStatus(setup.source->Status()));
  if (setup.source->Status() == PermissionQueryStatus::kReady) {
    result = setup.source->Result();
    Check(result.complete, std::string(label) + ": verdict is complete", "");
  }
  const PermissionQueryDiagnostics diagnostics = setup.source->Diagnostics();
  std::printf("[context-permission] diag %s: status=%llu compartment=%llu "
              "scope_reads=%llu sync=%llu async=%llu failures=%llu stage=%d hr=0x%08lX\n",
              label,
              (unsigned long long)diagnostics.status_reads,
              (unsigned long long)diagnostics.compartment_reads,
              (unsigned long long)diagnostics.scope_property_reads,
              (unsigned long long)diagnostics.read_sessions_sync,
              (unsigned long long)diagnostics.read_sessions_async,
              (unsigned long long)diagnostics.read_session_failures,
              diagnostics.last_scope_stage,
              (unsigned long)diagnostics.last_scope_hr);
  return result;
}

void CheckState(const PermissionQueryResult& result, PermissionState want,
                const char* label) {
  Check(result.decision.state == want, label,
        DescribeDecision(result.decision));
}

// --- raw-path probe -----------------------------------------------------------------------

// Reads the raw metadata the way the production source does, but prints the HRESULTs
// and VARIANT shapes before any classification, so the machine's actual answers are
// reproducible evidence in the phase report.
struct RawObservation {
  HRESULT status_hr = E_FAIL;
  bool read_only = false;
  HRESULT disabled_get_hr = E_FAIL;
  VARTYPE disabled_vt = VT_EMPTY;
  long disabled_value = 0;
  HRESULT app_property_hr = E_FAIL;
};

RawObservation ProbeRaw(TestDocument* document, TfClientId client_id) {
  RawObservation raw;
  ITfContext* context = document->context();

  TF_STATUS status = {};
  raw.status_hr = context->GetStatus(&status);
  raw.read_only = SUCCEEDED(raw.status_hr) &&
                  (status.dwDynamicFlags & TF_SD_READONLY) != 0;

  ITfCompartmentMgr* manager = nullptr;
  if (SUCCEEDED(context->QueryInterface(IID_ITfCompartmentMgr,
                                        reinterpret_cast<void**>(&manager))) &&
      manager != nullptr) {
    ITfCompartment* compartment = nullptr;
    if (SUCCEEDED(manager->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED,
                                          &compartment)) &&
        compartment != nullptr) {
      VARIANT value;
      ::VariantInit(&value);
      raw.disabled_get_hr = compartment->GetValue(&value);
      raw.disabled_vt = value.vt;
      if (value.vt == VT_I4) {
        raw.disabled_value = value.lVal;
      }
      ::VariantClear(&value);
      compartment->Release();
    }
    manager->Release();
  }

  ITfReadOnlyProperty* property = nullptr;
  raw.app_property_hr = context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
  if (property != nullptr) {
    property->Release();
  }
  (void)client_id;
  return raw;
}

}  // namespace

// --- 1. pure classification ---------------------------------------------------------------

int RunPureClassification() {
  PermissionFacts facts;

  facts.status.presence = FactPresence::kValue;
  facts.keyboard_disabled.presence = FactPresence::kAbsent;
  facts.empty_context.presence = FactPresence::kAbsent;
  facts.input_scope.presence = FactPresence::kAbsent;
  Check(DecidePermission(facts).state == PermissionState::kAllowed,
        "pure: every axis clear is Allowed",
        DescribeDecision(DecidePermission(facts)));

  PermissionFacts denied = facts;
  denied.status.read_only = true;
  Check(DecidePermission(denied).state == PermissionState::kDenied,
        "pure: read-only denies", "");
  Check((DecidePermission(denied).deny_findings & kFindingReadOnlyContext) != 0,
        "pure: read-only finding present", "");

  PermissionFacts disabled = facts;
  disabled.keyboard_disabled.presence = FactPresence::kValue;
  disabled.keyboard_disabled.value = 2;  // nonzero, not TRUE
  Check(DecidePermission(disabled).state == PermissionState::kDenied,
        "pure: KEYBOARD_DISABLED value 2 denies (nonzero, not == TRUE)", "");
  disabled.keyboard_disabled.value = -2147483648L;  // LONG_MIN, nonzero
  Check(DecidePermission(disabled).state == PermissionState::kDenied,
        "pure: negative nonzero DWORD denies", "");
  disabled.keyboard_disabled.value = 0;
  Check(DecidePermission(disabled).state == PermissionState::kAllowed,
        "pure: zero DWORD does not deny", "");

  PermissionFacts wrong_type = facts;
  wrong_type.keyboard_disabled.presence = FactPresence::kWrongType;
  Check(DecidePermission(wrong_type).state == PermissionState::kUnknown,
        "pure: wrong compartment type is Unknown, not false",
        DescribeDecision(DecidePermission(wrong_type)));
  Check((DecidePermission(wrong_type).unknown_findings &
         kFindingKeyboardDisabledQueryFailed) != 0,
        "pure: wrong type names its axis", "");

  PermissionFacts failed = facts;
  failed.empty_context.presence = FactPresence::kQueryFailed;
  Check(DecidePermission(failed).state == PermissionState::kUnknown,
        "pure: query failure is Unknown",
        DescribeDecision(DecidePermission(failed)));

  PermissionFacts not_queried;
  Check(DecidePermission(not_queried).state == PermissionState::kUnknown,
        "pure: an incomplete query can never be Allowed",
        DescribeDecision(DecidePermission(not_queried)));

  // Deny outranks Unknown: a known refusal stays reportable beside an unreadable axis.
  PermissionFacts mixed = failed;
  mixed.keyboard_disabled.presence = FactPresence::kValue;
  mixed.keyboard_disabled.value = 1;
  const PermissionDecision mixed_decision = DecidePermission(mixed);
  Check(mixed_decision.state == PermissionState::kDenied,
        "pure: deny outranks unknown", DescribeDecision(mixed_decision));
  Check(mixed_decision.deny_findings != kFindingNone &&
            mixed_decision.unknown_findings != kFindingNone,
        "pure: both findings survive together", "");

  // Sensitive scopes, alone and in lists, order-independent.
  for (long sensitive : {31L, 61L, 63L, 64L, 65L, 66L}) {
    PermissionFacts scoped = facts;
    scoped.input_scope.presence = FactPresence::kValue;
    scoped.input_scope.scopes = {sensitive};
    Check(DecidePermission(scoped).state == PermissionState::kDenied,
          "pure: sensitive scope alone denies", std::to_string(sensitive));
  }
  PermissionFacts front = facts;
  front.input_scope.presence = FactPresence::kValue;
  front.input_scope.scopes = {31L, 1L, 28L};
  PermissionFacts back = facts;
  back.input_scope.presence = FactPresence::kValue;
  back.input_scope.scopes = {28L, 1L, 31L};
  Check(DecidePermission(front).state == PermissionState::kDenied &&
            DecidePermission(back).state == PermissionState::kDenied,
        "pure: one sensitive scope denies at any position", "");
  PermissionFacts ordinary = facts;
  ordinary.input_scope.presence = FactPresence::kValue;
  ordinary.input_scope.scopes = {0L, 1L, 28L, 68L, -5L};
  Check(DecidePermission(ordinary).state == PermissionState::kAllowed,
        "pure: documented ordinary scopes stay Allowed",
        DescribeDecision(DecidePermission(ordinary)));

  // Values outside the pinned header's documented range are uninterpretable.
  PermissionFacts future_value = facts;
  future_value.input_scope.presence = FactPresence::kValue;
  future_value.input_scope.scopes = {69L};
  Check(DecidePermission(future_value).state == PermissionState::kUnknown,
        "pure: scope value above the documented range is Unknown",
        DescribeDecision(DecidePermission(future_value)));
  PermissionFacts below_range = facts;
  below_range.input_scope.presence = FactPresence::kValue;
  below_range.input_scope.scopes = {-6L};
  Check(DecidePermission(below_range).state == PermissionState::kUnknown,
        "pure: scope value below the documented range is Unknown", "");

  PermissionFacts scope_wrong_type = facts;
  scope_wrong_type.input_scope.presence = FactPresence::kWrongType;
  Check(DecidePermission(scope_wrong_type).state == PermissionState::kUnknown,
        "pure: wrong property payload type is Unknown",
        DescribeDecision(DecidePermission(scope_wrong_type)));

  Check(!IsSensitiveInputScope(0) && !IsSensitiveInputScope(47) &&
            !IsSensitiveInputScope(50),
        "pure: ordinary scopes are not sensitive", "");
  Check(IsSensitiveInputScope(31) && IsSensitiveInputScope(66),
        "pure: the password-class set is sensitive", "");

  return Finish();
}

// --- 2. empirical probe of the real path ---------------------------------------------------

int RunRealPathProbe() {
  {
    Case setup;
    if (!SetupCase(&setup, TestDocumentOptions{}, "probe")) {
      return Finish();
    }
    const RawObservation plain = ProbeRaw(setup.document, setup.host->client_id());
    std::printf("[context-permission] probe plain: GetStatus=%s read_only=%d "
                "keyboard_disabled GetValue=%s vt=%d lVal=%ld GetAppProperty=%s\n",
                Hr(plain.status_hr).c_str(), plain.read_only ? 1 : 0,
                Hr(plain.disabled_get_hr).c_str(), static_cast<int>(plain.disabled_vt),
                plain.disabled_value, Hr(plain.app_property_hr).c_str());
  }
  {
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = {1, 31};
    if (!SetupCase(&setup, options, "probe-scopes")) {
      return Finish();
    }
    const RawObservation scoped = ProbeRaw(setup.document, setup.host->client_id());
    std::printf("[context-permission] probe scopes: GetAppProperty=%s\n",
                Hr(scoped.app_property_hr).c_str());
  }
  {
    Case setup;
    if (!SetupCase(&setup, TestDocumentOptions{}, "probe-disabled")) {
      return Finish();
    }
    const HRESULT set_hr = setup.document->SetContextCompartment(
        GUID_COMPARTMENT_KEYBOARD_DISABLED, setup.host->client_id(), 1);
    Check(SUCCEEDED(set_hr), "probe: app-side compartment SetValue works",
          Hr(set_hr));
    const RawObservation disabled =
        ProbeRaw(setup.document, setup.host->client_id());
    std::printf("[context-permission] probe disabled: GetValue=%s vt=%d lVal=%ld\n",
                Hr(disabled.disabled_get_hr).c_str(),
                static_cast<int>(disabled.disabled_vt), disabled.disabled_value);
    Check(disabled.disabled_get_hr == S_OK && disabled.disabled_vt == VT_I4 &&
              disabled.disabled_value == 1,
          "probe: a set compartment reads back as VT_I4 with its value", "");
  }

  // The officially documented anchor points this machine must hold, whatever else it
  // does: GetStatus answers, and a set compartment reads back as VT_I4.
  return Finish();
}

// --- 3. plain editable context is Allowed --------------------------------------------------

int RunPlainEditableAllowed() {
  Case setup;
  if (!SetupCase(&setup, TestDocumentOptions{}, "plain")) {
    return Finish();
  }
  const PermissionQueryResult result = QueryOnce(setup, "plain");
  CheckState(result, PermissionState::kAllowed, "plain: ordinary editable context is Allowed");
  Check(result.decision.deny_findings == kFindingNone &&
            result.decision.unknown_findings == kFindingNone,
        "plain: Allowed carries no findings", DescribeDecision(result.decision));
  Check(result.context_id == setup.document->context_id() && result.generation == 1,
        "plain: the verdict carries its binding identity", "");
  return Finish();
}

// --- 4. read-only denies -------------------------------------------------------------------

int RunReadOnlyDenied() {
  Case setup;
  TestDocumentOptions options;
  options.read_only = true;
  if (!SetupCase(&setup, options, "readonly")) {
    return Finish();
  }
  const PermissionQueryResult result = QueryOnce(setup, "readonly");
  CheckState(result, PermissionState::kDenied, "readonly: read-only context is Denied");
  Check((result.decision.deny_findings & kFindingReadOnlyContext) != 0,
        "readonly: the ReadOnlyContext finding is present",
        DescribeDecision(result.decision));
  return Finish();
}

// --- 5. KEYBOARD_DISABLED nonzero values ---------------------------------------------------

int RunKeyboardDisabledValues() {
  Case setup;
  if (!SetupCase(&setup, TestDocumentOptions{}, "kbd-disabled")) {
    return Finish();
  }
  const TfClientId client = setup.host->client_id();
  const long values[] = {1L, 2L, 0x40000000L, -2147483647L - 1L};
  for (const long value : values) {
    const HRESULT set_hr = setup.document->SetContextCompartment(
        GUID_COMPARTMENT_KEYBOARD_DISABLED, client, value);
    Check(SUCCEEDED(set_hr), "kbd-disabled: SetValue real path",
          Hr(set_hr) + " value=" + std::to_string(value));
    const PermissionQueryResult result = QueryOnce(setup, "kbd-disabled");
    CheckState(result, PermissionState::kDenied,
               "kbd-disabled: nonzero DWORD denies (not only == TRUE)");
    Check((result.decision.deny_findings & kFindingKeyboardDisabled) != 0,
          "kbd-disabled: the KeyboardDisabled finding is present",
          DescribeDecision(result.decision));
  }
  const HRESULT zero_hr = setup.document->SetContextCompartment(
      GUID_COMPARTMENT_KEYBOARD_DISABLED, client, 0);
  Check(SUCCEEDED(zero_hr), "kbd-disabled: zero SetValue", Hr(zero_hr));
  const PermissionQueryResult zero = QueryOnce(setup, "kbd-disabled-zero");
  CheckState(zero, PermissionState::kAllowed, "kbd-disabled: zero does not deny");
  return Finish();
}

// --- 6. EMPTYCONTEXT denies ----------------------------------------------------------------

int RunEmptyContextDenied() {
  Case setup;
  if (!SetupCase(&setup, TestDocumentOptions{}, "empty-ctx")) {
    return Finish();
  }
  const HRESULT set_hr = setup.document->SetContextCompartment(
      GUID_COMPARTMENT_EMPTYCONTEXT, setup.host->client_id(), 1);
  Check(SUCCEEDED(set_hr), "empty-ctx: SetValue real path", Hr(set_hr));
  const PermissionQueryResult result = QueryOnce(setup, "empty-ctx");
  CheckState(result, PermissionState::kDenied, "empty-ctx: nonzero EMPTYCONTEXT denies");
  Check((result.decision.deny_findings & kFindingEmptyContext) != 0,
        "empty-ctx: the EmptyContext finding is present",
        DescribeDecision(result.decision));
  return Finish();
}

// --- 7. sensitive scopes deny ---------------------------------------------------------------

int RunSensitiveScopesDenied() {
  // Each sensitive constant alone denies.
  for (const long scope : {31L, 61L, 63L, 64L, 65L, 66L}) {
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = {scope};
    const std::string label = "sensitive-" + std::to_string(scope);
    if (!SetupCase(&setup, options, label.c_str())) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, label.c_str());
    CheckState(result, PermissionState::kDenied,
               "sensitive: the scope value denies");
    Check((result.decision.deny_findings & kFindingSensitiveInputScope) != 0,
          "sensitive: the SensitiveInputScope finding is present",
          DescribeDecision(result.decision));
  }

  // In a list, at either end, the sensitive value still decides.
  for (const std::vector<long>& list :
       std::vector<std::vector<long>>{{31L, 1L, 28L}, {1L, 28L, 31L},
                                      {0L, 64L, 29L}, {64L}}) {
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = list;
    if (!SetupCase(&setup, options, "sensitive-list")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "sensitive-list");
    CheckState(result, PermissionState::kDenied,
               "sensitive: one sensitive scope in a list denies at any position");
  }

  // Ordinary scopes alone stay Allowed.
  Case ordinary;
  TestDocumentOptions ordinary_options;
  ordinary_options.scope_answer = TestScopeAnswer::kScopes;
  ordinary_options.input_scopes = {0L, 1L, 28L, 47L};
  if (!SetupCase(&ordinary, ordinary_options, "sensitive-ordinary")) {
    return Finish();
  }
  const PermissionQueryResult ordinary_result = QueryOnce(ordinary, "sensitive-ordinary");
  CheckState(ordinary_result, PermissionState::kAllowed,
             "sensitive: ordinary scopes stay Allowed");
  return Finish();
}

// --- 8. absent / wrong type / failing attribute read ---------------------------------------

int RunScopeAbsentWrongTypeFail() {
  {
    Case setup;
    if (!SetupCase(&setup, TestDocumentOptions{}, "absent")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "absent");
    CheckState(result, PermissionState::kAllowed,
               "absent: no published scope metadata is Allowed (the official default)");
  }
  {
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kWrongType;
    if (!SetupCase(&setup, options, "wrong-type")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "wrong-type");
    CheckState(result, PermissionState::kUnknown,
               "wrong-type: a VT_I4 payload is Unknown, never interpreted");
    Check((result.decision.unknown_findings & kFindingScopeWrongValueType) != 0,
          "wrong-type: the finding names the payload type",
          DescribeDecision(result.decision));
  }
  {
    // A well-formed answer whose VARIANT carries no payload. Whether msctf surfaces it as
    // S_OK+VT_EMPTY or as its no-coverage answer, it is an absence, not a permission fact.
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kEmptyValue;
    if (!SetupCase(&setup, options, "empty-value")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "empty-value");
    CheckState(result, PermissionState::kAllowed,
               "empty-value: a normal empty value is absent, not Unknown");
    Check(result.decision.unknown_findings == kFindingNone,
          "empty-value: no unknown finding", DescribeDecision(result.decision));
  }
  {
    // The store refuses GetSelection while the read lock is held: the InputScope range
    // read genuinely fails, while GetStatus and both compartments stay readable. This is
    // the distinguishable query-failure shape (as opposed to the E_FAIL ambiguity below).
    Case setup;
    TestDocumentOptions options;
    options.refuse_selection_reads = true;
    if (!SetupCase(&setup, options, "scope-query-failure")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "scope-query-failure");
    CheckState(result, PermissionState::kUnknown,
               "query-failure: an unreadable selection read is Unknown");
    Check((result.decision.unknown_findings & kFindingScopeQueryFailed) != 0,
          "query-failure: the finding names the scope axis",
          DescribeDecision(result.decision));
    const PermissionQueryDiagnostics diagnostics = setup.source->Diagnostics();
    Check(diagnostics.last_scope_stage == 1,
          "query-failure: the failure is the selection read",
          std::to_string(diagnostics.last_scope_stage));
  }
  {
    // DOCUMENTED GAP: when the context owner's RetrieveRequestedAttrs fails, the real
    // msctf.dll surfaces the same E_FAIL from GetAppProperty(...)->GetValue that an
    // attribute-free owner produces (observed on this machine, stage-2 hr=0x80004005 in
    // both configurations). The two states are indistinguishable from the text service
    // side, so both classify as Absent; a broken owner cannot be refused through this
    // property. The wrong-type mode above is the one malformed-metadata state that
    // stays distinguishable and Unknown.
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kFail;
    if (!SetupCase(&setup, options, "scope-fail")) {
      return Finish();
    }
    const PermissionQueryResult result = QueryOnce(setup, "scope-fail");
    CheckState(result, PermissionState::kAllowed,
               "scope-fail: an owner read failure is indistinguishable from absence");
  }
  return Finish();
}

// --- 9. cleared compartment is Unknown -----------------------------------------------------

int RunClearedCompartmentRecreated() {
  Case setup;
  if (!SetupCase(&setup, TestDocumentOptions{}, "cleared")) {
    return Finish();
  }
  const TfClientId client = setup.host->client_id();
  HRESULT hr = setup.document->SetContextCompartment(
      GUID_COMPARTMENT_KEYBOARD_DISABLED, client, 1);
  Check(SUCCEEDED(hr), "cleared: set first", Hr(hr));
  hr = setup.document->ClearContextCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED,
                                               client);
  Check(SUCCEEDED(hr), "cleared: ClearCompartment real path", Hr(hr));
  const PermissionQueryResult result = QueryOnce(setup, "cleared");
  // The E_UNEXPECTED state ITfCompartment::GetValue documents for a cleared
  // compartment is NOT observable through a fresh query on this machine: the
  // Compartments reference explains that a compartment is created by the first
  // GetCompartment call, so the next query recreates it and reads the official
  // no-value answer (S_FALSE + VT_EMPTY, verified: the verdict is Allowed with no
  // findings). The production reader still maps E_UNEXPECTED to Unknown for a
  // reference that kept the pre-clear identity; this case pins the recreated
  // behaviour that real hosts see.
  CheckState(result, PermissionState::kAllowed,
             "cleared: a cleared-then-recreated compartment reads as no value");
  Check(result.decision.unknown_findings == kFindingNone,
        "cleared: the recreated no-value compartment carries no findings",
        DescribeDecision(result.decision));
  return Finish();
}

// --- 10. repeated queries are consistent ---------------------------------------------------

int RunRepeatedQueriesConsistent() {
  Case setup;
  TestDocumentOptions options;
  options.scope_answer = TestScopeAnswer::kScopes;
  options.input_scopes = {1, 31};
  if (!SetupCase(&setup, options, "repeat")) {
    return Finish();
  }
  const PermissionQueryResult first = QueryOnce(setup, "repeat");
  for (int index = 1; index < 3; ++index) {
    const PermissionQueryResult again = QueryOnce(setup, "repeat");
    Check(again.decision.state == first.decision.state &&
              again.decision.deny_findings == first.decision.deny_findings &&
              again.decision.unknown_findings == first.decision.unknown_findings,
          "repeat: the same metadata yields the same decision every time",
          DescribeDecision(again.decision) + " vs " + DescribeDecision(first.decision));
  }
  const PermissionQueryDiagnostics diagnostics = setup.source->Diagnostics();
  Check(diagnostics.queries_begun == 3 && diagnostics.queries_completed == 3,
        "repeat: three begun, three completed", "");
  return Finish();
}

// --- 11. deferred read session stays pending until pumped ----------------------------------

int RunAsyncQueryPending() {
  Case setup;
  TestDocumentOptions options;
  options.scope_answer = TestScopeAnswer::kScopes;
  options.input_scopes = {1};
  options.async_locks = true;
  options.refuse_sync_locks = true;
  if (!SetupCase(&setup, options, "async")) {
    return Finish();
  }
  Check(setup.source->Begin(setup.document->context(), setup.document->context_id(), 1),
        "async: Begin accepted", "");
  Check(setup.source->Status() == PermissionQueryStatus::kPending,
        "async: a deferred read session is Pending, not a verdict",
        DescribePermissionQueryStatus(setup.source->Status()));
  Check(setup.source->CompletionEvent() != nullptr,
        "async: a pending query exposes its completion event", "");
  // Pumping the real message loop is what lets the store grant the deferred lock and
  // the real callback run; no sleep and no busy wait.
  Check(setup.host->PumpUntil(setup.source->CompletionEvent(), 20000),
        "async: the deferred read completed after pumping", "timeout");
  Check(setup.source->Status() == PermissionQueryStatus::kReady,
        "async: the query reached its verdict",
        DescribePermissionQueryStatus(setup.source->Status()));
  CheckState(setup.source->Result(), PermissionState::kAllowed,
             "async: the deferred verdict classifies normally");
  const PermissionQueryDiagnostics diagnostics = setup.source->Diagnostics();
  Check(diagnostics.read_sessions_async == 1,
        "async: exactly one async fallback was used", "");
  return Finish();
}

// --- 12. abandon ends a pending query immediately ------------------------------------------

int RunAbandonPendingQuery() {
  Case setup;
  TestDocumentOptions options;
  options.scope_answer = TestScopeAnswer::kScopes;
  options.input_scopes = {1};
  options.async_locks = true;
  options.refuse_sync_locks = true;
  if (!SetupCase(&setup, options, "abandon")) {
    return Finish();
  }
  Check(setup.source->Begin(setup.document->context(), setup.document->context_id(), 1),
        "abandon: Begin accepted", "");
  Check(setup.source->Status() == PermissionQueryStatus::kPending,
        "abandon: query is pending", "");
  // Abandon must return immediately (this whole case is bounded by its CTest timeout)
  // and must leave no usable verdict.
  setup.source->Abandon();
  Check(setup.source->Status() == PermissionQueryStatus::kIdle,
        "abandon: an abandoned query is terminal",
        DescribePermissionQueryStatus(setup.source->Status()));
  Check(!setup.source->Result().complete,
        "abandon: no verdict can be read afterwards", "");
  // The deferred grant message is still queued; draining it (the host's Shutdown does
  // this) must not resurrect anything.
  setup.host->DispatchQueuedMessages();
  Check(setup.source->Status() == PermissionQueryStatus::kIdle,
        "abandon: a late callback publishes nothing",
        DescribePermissionQueryStatus(setup.source->Status()));
  const PermissionQueryDiagnostics diagnostics = setup.source->Diagnostics();
  Check(diagnostics.queries_abandoned == 1 && diagnostics.queries_completed == 0,
        "abandon: counted as abandoned, not completed", "");

  // A second, sharper lifetime case: a new query is begun (on a different context) while
  // the abandoned query's deferred grant is STILL queued. Each Begin must own a separate
  // state, so the late old callback cannot complete or corrupt the new query's verdict.
  std::string error;
  TestDocument* document_b =
      setup.host->CreateDocument(TestDocumentOptions{}, &error);
  Check(document_b != nullptr, "abandon: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }
  Check(setup.source->Begin(setup.document->context(), setup.document->context_id(),
                            /*generation=*/2),
        "abandon: second query begun", "");
  Check(setup.source->Status() == PermissionQueryStatus::kPending,
        "abandon: second query pending", "");
  setup.source->Abandon();
  // The second document's read is synchronous (no deferred lock), so this verdict is
  // complete before the old context's queued grant is dispatched.
  Check(setup.source->Begin(document_b->context(), document_b->context_id(),
                            /*generation=*/3),
        "abandon: replacement query begun on the second context", "");
  Check(setup.source->Status() == PermissionQueryStatus::kReady,
        "abandon: the replacement verdict is ready before the old callback runs",
        DescribePermissionQueryStatus(setup.source->Status()));
  const PermissionQueryResult replacement = setup.source->Result();
  Check(replacement.complete && replacement.decision.state == PermissionState::kAllowed,
        "abandon: the replacement verdict is Allowed", "");
  Check(replacement.generation == 3 &&
            replacement.context_id == document_b->context_id(),
        "abandon: the replacement verdict carries its own identity",
        "generation=" + std::to_string(replacement.generation) +
            " context=" + std::to_string(replacement.context_id));
  setup.host->DispatchQueuedMessages();
  const PermissionQueryResult after_callback = setup.source->Result();
  Check(after_callback.complete &&
            after_callback.generation == replacement.generation &&
            after_callback.context_id == replacement.context_id &&
            after_callback.decision.state == replacement.decision.state,
        "abandon: a later old callback cannot rewrite the replacement verdict", "");
  const PermissionQueryDiagnostics after = setup.source->Diagnostics();
  Check(after.queries_completed == 1 && after.queries_abandoned == 2,
        "abandon: exactly one completion and two abandons were counted",
        "completed=" + std::to_string(after.queries_completed) +
            " abandoned=" + std::to_string(after.queries_abandoned));
  return Finish();
}

// --- 13. resource balance ------------------------------------------------------------------

int RunResourceBalance() {
  const int scopes_before = edit::test::TestTextStore::LiveInstanceCount();
  {
    Case setup;
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = {1, 31};
    if (!SetupCase(&setup, options, "balance")) {
      return Finish();
    }
    for (int index = 0; index < 3; ++index) {
      const PermissionQueryResult result = QueryOnce(setup, "balance");
      CheckState(result, PermissionState::kDenied, "balance: query denies");
    }
    // While the document lives, exactly the store's own ITfInputScope object exists;
    // every reference a query took was returned through VariantClear.
    Check(edit::test::TestTextStore::LiveInstanceCount() == scopes_before + 1,
          "balance: one scope object alive during queries", "");
    setup.source->Abandon();
    setup.source.reset();
    // The source released its context reference; the document/host references are
    // still accounted by the P3.7 teardown, which Shutdown() reports on.
  }
  Check(edit::test::TestTextStore::LiveInstanceCount() == scopes_before,
        "balance: no scope object outlives its document", "");
  return Finish();
}

// --- 14. falsifiability control ------------------------------------------------------------

int RunCanFail() {
  Check(1 + 1 == 3, "can-fail: this assertion is deliberately false", "");
  // A failing suite prints no PASS marker; the exit code is the evidence.
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva
