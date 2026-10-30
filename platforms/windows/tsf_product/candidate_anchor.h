// EpoCanva W3-A -- candidate anchor identity and bounded layout retry.
//
// The live caret fixes a display position only for the SAME host token. The token origin is
// the caret observed before the first character of that token; origin + host UTF-16 units is
// the only accepted end. A caret anywhere else is a different token even when the preceding
// text is identical, so it must never retarget a menu or authorize a write.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_ANCHOR_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_ANCHOR_H_

#include <cstddef>
#include <cstdint>
#include <limits>

namespace epocanva {
namespace tsf {
namespace product {

// Paced layout retries are bounded per host token, not per candidate snapshot version: a
// host that keeps publishing new versions must not reset the budget.
inline constexpr int kCandidateAnchorMaxRetries = 40;

enum class CandidateAnchorDecision : std::uint8_t {
  kAccept = 0,
  kUnknownToken,
  kCaretMismatch,
};

struct CandidateAnchorCheck {
  CandidateAnchorDecision decision = CandidateAnchorDecision::kUnknownToken;
  // Valid only when decision == kAccept.
  std::int32_t expected_caret_utf16 = -1;
};

inline bool CandidateTokenEndUtf16(bool token_origin_valid,
                                   std::int32_t token_origin_caret_utf16,
                                   std::size_t host_token_units_utf16,
                                   std::int32_t* expected_caret_utf16) {
  if (expected_caret_utf16 == nullptr || !token_origin_valid ||
      token_origin_caret_utf16 < 0 || host_token_units_utf16 == 0) {
    return false;
  }
  const std::int64_t expected =
      static_cast<std::int64_t>(token_origin_caret_utf16) +
      static_cast<std::int64_t>(host_token_units_utf16);
  if (expected > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  *expected_caret_utf16 = static_cast<std::int32_t>(expected);
  return true;
}

inline CandidateAnchorCheck CheckCandidateAnchor(bool token_origin_valid,
                                                 std::int32_t token_origin_caret_utf16,
                                                 std::size_t host_token_units_utf16,
                                                 std::int32_t live_caret_utf16) {
  CandidateAnchorCheck result;
  if (!CandidateTokenEndUtf16(token_origin_valid, token_origin_caret_utf16,
                              host_token_units_utf16,
                              &result.expected_caret_utf16)) {
    return result;
  }
  result.decision = live_caret_utf16 == result.expected_caret_utf16
                        ? CandidateAnchorDecision::kAccept
                        : CandidateAnchorDecision::kCaretMismatch;
  return result;
}

class CandidateAnchorRetryBudget {
 public:
  void Reset() { used_ = 0; }
  bool TryAcquire() {
    if (used_ >= kCandidateAnchorMaxRetries) {
      return false;
    }
    ++used_;
    return true;
  }
  int used() const { return used_; }
  bool exhausted() const { return used_ >= kCandidateAnchorMaxRetries; }

 private:
  int used_ = 0;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_ANCHOR_H_
