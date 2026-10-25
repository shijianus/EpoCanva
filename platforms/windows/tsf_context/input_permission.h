// EpoCanva P3.9 -- the pure input-permission classification.
//
// WHAT THIS FILE IS. A side-effect-free function over OBSERVED FACTS about one TSF
// context: the context status, two context compartments and the InputScope application
// property. It knows no COM type beyond the plain values the query layer
// (permission_query.h) copied out, so the policy below is testable without TSF.
//
// THE POLICY, IN ONE PARAGRAPH. Any axis that produced hard evidence the context must
// not receive engine input makes the verdict Denied (read-only, keyboard-disabled,
// empty-context, one sensitive InputScope). Failing that, any axis whose observation
// could not be interpreted makes the verdict Unknown (query failure, wrong VARIANT
// type, an InputScope value this SDK revision cannot name) -- never a silent Allowed.
// Only when every axis was actually queried and every answer was interpretable and
// none was blocking is the verdict Allowed. Deny outranks Unknown; within one axis and
// across the scope list the outcome is order-independent.
//
// WHY "ABSENT" IS NOT A GUESS. Each axis has an OFFICIAL no-value answer with a cited
// meaning; treating it as the permissive default is grounded, not assumed:
//   * ITfCompartment::GetValue returns S_FALSE with VT_EMPTY when the compartment has
//     no value (learn.microsoft.com, ITfCompartment::GetValue, Return Values). The
//     predefined-compartments reference defines both GUID_COMPARTMENT_KEYBOARD_DISABLED
//     and GUID_COMPARTMENT_EMPTYCONTEXT as "a DWORD that is nonzero if <condition> or
//     zero otherwise" -- so "no value" carries no condition, the same as zero.
//   * ITfContext::GetAppProperty returns S_FALSE ("the context owner does not support
//     the property") or E_NOTIMPL ("the context owner does not implement this method")
//     when no InputScope metadata exists, and the implicit InputScope of an ordinary
//     field is IS_DEFAULT (inputscope.h) -- unconstrained. Absence of a password scope
//     is therefore not itself evidence of anything; see the residual-risk note in
//     docs/windows/input-permission-contract.md.
//
// WHAT AN ERROR IS. A FAILED GetValue on a compartment, a FAILED GetStatus, a FAILED
// InputScope read, a VARIANT of a type other than the documented one (VT_I4 for the
// compartments, VT_UNKNOWN holding ITfInputScope for the property) and an InputScope
// value outside the pinned header's documented range are each UNINTERPRETABLE. They
// make the verdict Unknown; they are never folded into "false".
//
// UNITS. No offsets or lengths live here; the only numeric values are InputScope enum
// values (dimensionless SDK constants) and the VT_I4 compartment payload.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_INPUT_PERMISSION_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_INPUT_PERMISSION_H_

#include <cstdint>
#include <string>
#include <vector>

namespace epocanva {
namespace tsf {
namespace context {

// The three-valued answer every caller of this module gets. Allowed is an assertion
// that every axis was queried and none blocked; Unknown is the safe refusal, never a
// placeholder for "probably fine".
enum class PermissionState : std::uint8_t {
  kAllowed = 0,
  kDenied,
  kUnknown,
};

const char* DescribePermissionState(PermissionState state);

// Why a verdict is what it is. Bitmasks, so one decision can carry several findings at
// once; the names are text-free and carry no document content.
enum PermissionFindings : std::uint16_t {
  kFindingNone = 0,
  // --- Deny findings: hard evidence the context must not receive engine input. ------
  kFindingReadOnlyContext = 1u << 0,      // TF_STATUS.dwDynamicFlags & TF_SD_READONLY
  kFindingKeyboardDisabled = 1u << 1,     // KEYBOARD_DISABLED compartment, nonzero VT_I4
  kFindingEmptyContext = 1u << 2,         // EMPTYCONTEXT compartment, nonzero VT_I4
  kFindingSensitiveInputScope = 1u << 3,  // a password-class InputScope is present
  // --- Unknown findings: an axis could not be interpreted. --------------------------
  kFindingStatusQueryFailed = 1u << 8,     // ITfContext::GetStatus failed or was skipped
  kFindingKeyboardDisabledQueryFailed = 1u << 9,   // read failed / wrong type / skipped
  kFindingEmptyContextQueryFailed = 1u << 10,      // read failed / wrong type / skipped
  kFindingScopeQueryFailed = 1u << 11,     // property read failed / was skipped
  kFindingScopeWrongValueType = 1u << 12,  // property VARIANT not VT_UNKNOWN/VT_EMPTY
  kFindingUnrecognizedScopeValue = 1u << 13,  // InputScope value outside the documented
                                             // range of the pinned SDK header
};

// Text like "ReadOnlyContext|SensitiveInputScope"; "None" when no bit is set.
std::string DescribePermissionFindings(std::uint16_t findings);

// How one axis of the context metadata was actually observed. The distinction between
// kAbsent, kQueryFailed and kWrongType is the whole point of this module: they have
// three different meanings and three different verdicts.
enum class FactPresence : std::uint8_t {
  kNotQueried = 0,  // the query never reached this axis (an incomplete query)
  kAbsent,          // the official no-value answer (see the file comment)
  kValue,           // a well-typed value was read
  kWrongType,       // a value arrived, but not the documented type
  kQueryFailed,     // the query itself failed
};

const char* DescribeFactPresence(FactPresence presence);

// The observed facts. Filled by the query layer (permission_query.h) from real TSF
// reads; hand-built by the unit tests.
struct StatusFact {
  FactPresence presence = FactPresence::kNotQueried;
  bool read_only = false;  // meaningful only when presence == kValue
};

struct CompartmentFact {
  FactPresence presence = FactPresence::kNotQueried;
  // The VT_I4 payload, meaningful only when presence == kValue. The predefined
  // compartments are "nonzero if <condition>", so the classification tests != 0, not
  // == TRUE; any nonzero LONG (including negative values) means the condition holds.
  long value = 0;
};

struct ScopeFact {
  FactPresence presence = FactPresence::kNotQueried;
  // The InputScope enum values ITfInputScope::GetInputScopes delivered, in delivery
  // order. Classification is order-independent by design; the order is kept only so
  // diagnostics can distinguish deliveries.
  std::vector<long> scopes;
};

struct PermissionFacts {
  StatusFact status;                    // ITfContext::GetStatus
  CompartmentFact keyboard_disabled;    // GUID_COMPARTMENT_KEYBOARD_DISABLED
  CompartmentFact empty_context;        // GUID_COMPARTMENT_EMPTYCONTEXT
  ScopeFact input_scope;                // GUID_PROP_INPUTSCOPE app property
};

struct PermissionDecision {
  PermissionState state = PermissionState::kUnknown;
  std::uint16_t deny_findings = kFindingNone;
  std::uint16_t unknown_findings = kFindingNone;
  bool denied() const { return state == PermissionState::kDenied; }
  bool allowed() const { return state == PermissionState::kAllowed; }
};

// The documented InputScope range of the pinned SDK header (um/inputscope.h,
// 10.0.26100.0): IS_ENUMSTRING = -5 is the lowest and IS_CHAT_WITHOUT_EMOJI = 68 the
// highest named value. A value outside this range cannot be named by this SDK revision;
// classifying it as harmless would be a guess, so it makes the verdict Unknown.
constexpr long kMinDocumentedInputScope = -5;
constexpr long kMaxDocumentedInputScope = 68;

// The sensitive InputScope set, pinned to the named constants of um/inputscope.h:
//   IS_PRIVATE = 61, IS_PASSWORD = 31, IS_NUMERIC_PASSWORD = 63, IS_NUMERIC_PIN = 64,
//   IS_ALPHANUMERIC_PIN = 65, IS_ALPHANUMERIC_PIN_SET = 66.
// IS_ONE_TIME_CODE does not exist in the header and is deliberately absent. Password-
// like values this SDK does define but that name ordinary content (IS_SEARCH, IS_URL,
// ...) are not sensitive. Deny-priority: one sensitive value anywhere in the list
// denies, regardless of position or of other values.
bool IsSensitiveInputScope(long scope);

// The classification. Deny findings win over unknown findings: evidence that typing is
// refused is more specific than inability to interpret an axis. An axis that was never
// queried (kNotQueried) contributes an unknown finding -- an incomplete query can never
// yield Allowed.
PermissionDecision DecidePermission(const PermissionFacts& facts);

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_INPUT_PERMISSION_H_
