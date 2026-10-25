// EpoCanva P3.9 -- implementation of the pure input-permission classification. The
// contract, the citations for every "absent means" rule and the deny/unknown precedence
// live in input_permission.h; this file adds no policy of its own.
#include "input_permission.h"

namespace epocanva {
namespace tsf {
namespace context {

namespace {

// The sensitive InputScope values, each a named constant of the pinned um/inputscope.h.
constexpr long kSensitiveInputScopes[] = {
    31,  // IS_PASSWORD
    61,  // IS_PRIVATE
    63,  // IS_NUMERIC_PASSWORD
    64,  // IS_NUMERIC_PIN
    65,  // IS_ALPHANUMERIC_PIN
    66,  // IS_ALPHANUMERIC_PIN_SET
};

struct FindingName {
  std::uint16_t bit;
  const char* name;
};

const FindingName kFindingNames[] = {
    {kFindingReadOnlyContext, "ReadOnlyContext"},
    {kFindingKeyboardDisabled, "KeyboardDisabled"},
    {kFindingEmptyContext, "EmptyContext"},
    {kFindingSensitiveInputScope, "SensitiveInputScope"},
    {kFindingStatusQueryFailed, "StatusQueryFailed"},
    {kFindingKeyboardDisabledQueryFailed, "KeyboardDisabledQueryFailed"},
    {kFindingEmptyContextQueryFailed, "EmptyContextQueryFailed"},
    {kFindingScopeQueryFailed, "ScopeQueryFailed"},
    {kFindingScopeWrongValueType, "ScopeWrongValueType"},
    {kFindingUnrecognizedScopeValue, "UnrecognizedScopeValue"},
};

// One compartment axis. kAbsent is the official no-value answer and clears the axis;
// everything except a well-typed value contributes the axis's unknown finding.
void ClassifyCompartment(const CompartmentFact& fact, std::uint16_t deny_bit,
                         std::uint16_t unknown_bit, PermissionDecision* decision) {
  switch (fact.presence) {
    case FactPresence::kValue:
      // "Nonzero if <condition> or zero otherwise" (predefined-compartments reference):
      // the test is against zero, not against TRUE, so every nonzero LONG counts.
      if (fact.value != 0) {
        decision->deny_findings |= deny_bit;
      }
      return;
    case FactPresence::kAbsent:
      return;  // officially no value; the same meaning as zero
    case FactPresence::kNotQueried:
    case FactPresence::kWrongType:
    case FactPresence::kQueryFailed:
    default:
      decision->unknown_findings |= unknown_bit;
      return;
  }
}

}  // namespace

const char* DescribePermissionState(PermissionState state) {
  switch (state) {
    case PermissionState::kAllowed:
      return "Allowed";
    case PermissionState::kDenied:
      return "Denied";
    case PermissionState::kUnknown:
      return "Unknown";
  }
  return "UnknownPermissionState";
}

std::string DescribePermissionFindings(std::uint16_t findings) {
  if (findings == kFindingNone) {
    return "None";
  }
  std::string text;
  for (const FindingName& entry : kFindingNames) {
    if ((findings & entry.bit) != 0) {
      if (!text.empty()) {
        text += "|";
      }
      text += entry.name;
    }
  }
  return text;
}

const char* DescribeFactPresence(FactPresence presence) {
  switch (presence) {
    case FactPresence::kNotQueried:
      return "NotQueried";
    case FactPresence::kAbsent:
      return "Absent";
    case FactPresence::kValue:
      return "Value";
    case FactPresence::kWrongType:
      return "WrongType";
    case FactPresence::kQueryFailed:
      return "QueryFailed";
  }
  return "UnknownFactPresence";
}

bool IsSensitiveInputScope(long scope) {
  for (const long sensitive : kSensitiveInputScopes) {
    if (scope == sensitive) {
      return true;
    }
  }
  return false;
}

PermissionDecision DecidePermission(const PermissionFacts& facts) {
  PermissionDecision decision;

  // Context status: GetStatus either answers or fails; there is no documented
  // "absent/wrong type" shape for it.
  if (facts.status.presence == FactPresence::kValue) {
    if (facts.status.read_only) {
      decision.deny_findings |= kFindingReadOnlyContext;
    }
  } else {
    decision.unknown_findings |= kFindingStatusQueryFailed;
  }

  ClassifyCompartment(facts.keyboard_disabled, kFindingKeyboardDisabled,
                      kFindingKeyboardDisabledQueryFailed, &decision);
  ClassifyCompartment(facts.empty_context, kFindingEmptyContext,
                      kFindingEmptyContextQueryFailed, &decision);

  // InputScope list. One sensitive value denies the whole list; a value this header
  // revision cannot name makes the observation uninterpretable.
  switch (facts.input_scope.presence) {
    case FactPresence::kAbsent:
      break;  // no scopes declared; the implicit IS_DEFAULT is unconstrained
    case FactPresence::kValue:
      for (const long scope : facts.input_scope.scopes) {
        if (IsSensitiveInputScope(scope)) {
          decision.deny_findings |= kFindingSensitiveInputScope;
        } else if (scope < kMinDocumentedInputScope ||
                   scope > kMaxDocumentedInputScope) {
          decision.unknown_findings |= kFindingUnrecognizedScopeValue;
        }
      }
      break;
    case FactPresence::kNotQueried:
    case FactPresence::kQueryFailed:
      decision.unknown_findings |= kFindingScopeQueryFailed;
      break;
    case FactPresence::kWrongType:
    default:
      decision.unknown_findings |= kFindingScopeWrongValueType;
      break;
  }

  // Deny outranks Unknown: "we know typing is refused" stays reportable even when
  // another axis is unreadable.
  if (decision.deny_findings != kFindingNone) {
    decision.state = PermissionState::kDenied;
  } else if (decision.unknown_findings != kFindingNone) {
    decision.state = PermissionState::kUnknown;
  } else {
    decision.state = PermissionState::kAllowed;
  }
  return decision;
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
