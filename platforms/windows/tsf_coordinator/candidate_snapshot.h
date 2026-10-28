// EpoCanva W2-B -- one immutable copy of the engine's candidate menu.
//
// WHERE IT COMES FROM. Every engine observation (a key dispatch or a candidate acceptance)
// carries the engine's current page: candidate texts in menu order, the page number, the
// page size, the last-page flag and the highlighted index. The coordinator copies that page
// into a CandidateSnapshot exactly once per observation and gives it a monotonically
// increasing version. The window and the selection path consume that copy; neither asks the
// engine directly.
//
// WHAT THE CLASSIFICATION IS. The product translator emits candidate 0 as the original
// input, then completion candidates, then spelling suggestions. The C API exposes only text
// and comment, so the classification uses the two facts it really has:
//   page 0, index 0            -> kLiteral (the text the user typed)
//   comment == L"spell"        -> kSpelling (the translator's fixed suggestion label)
//   everything else            -> kCompletion
// A future translator that changes these facts must change this function deliberately.
//
// UNITS. Candidate text is UTF-16 code units. `page_index` is a page-relative index, which
// is exactly what select_candidate_on_current_page accepts. `menu_index` is the global menu
// index (page_no * page_size + page_index) for diagnostics only.
//
// PRIVACY. The snapshot holds candidate text because a UI must display it. It is memory
// only: no snapshot is logged, persisted, or included in diagnostics.
#ifndef EPOCANVA_TSF_COORDINATOR_CANDIDATE_SNAPSHOT_H_
#define EPOCANVA_TSF_COORDINATOR_CANDIDATE_SNAPSHOT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "edit_plan.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

enum class CandidateKind : std::uint8_t {
  kLiteral = 0,
  kCompletion,
  kSpelling,
};

const char* DescribeCandidateKind(CandidateKind kind);

// The engine's fixed comment for a spelling suggestion
// (engine/rime_components/english/english_diagnostics.h:
// EPOCANVA_ENGLISH_SPELLING_COMMENT). It is duplicated here because the coordinator does
// not link the engine's private headers; the value is part of the engine/front-end contract.
inline constexpr char16_t kSpellingComment[] = u"spell";

inline CandidateKind ClassifyCandidate(std::size_t page_no, std::size_t page_index,
                                       std::u16string_view comment) {
  if (page_no == 0 && page_index == 0) {
    return CandidateKind::kLiteral;
  }
  if (comment == kSpellingComment) {
    return CandidateKind::kSpelling;
  }
  return CandidateKind::kCompletion;
}

struct CandidateItem {
  std::u16string text_utf16_code_units;
  std::u16string comment_utf16_code_units;  // engine comment, usually empty
  CandidateKind kind = CandidateKind::kCompletion;
  std::size_t page_index = 0;
  std::size_t menu_index = 0;
};

struct CandidateSnapshot {
  bool valid = false;
  // Monotonic; every observation increments it, including observations that clear the
  // menu. A click carries the version it was drawn from.
  std::uint64_t version = 0;
  edit::ContextIdentity identity{};
  std::uint64_t work_id = 0;
  std::uint64_t generation = 0;  // the binding generation the menu was read on
  int page_no = 0;
  int page_size = 0;
  bool is_last_page = true;
  int highlighted_page_index = -1;
  std::vector<CandidateItem> items;
  // The engine's own raw input for this composition (UTF-16 code units). Kept for the
  // literal row/status; never logged.
  std::u16string input_utf16_code_units;

  std::size_t size() const { return items.size(); }
  bool has_page() const { return page_size > 0; }
  // The literal row is the real original text, not a suggestion.
  bool has_literal() const;
};

// One validated selection. The caller fills every field from the snapshot it displayed.
struct CandidateSelectionRequest {
  std::uint64_t snapshot_version = 0;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  std::size_t page_index = 0;
};

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_COORDINATOR_CANDIDATE_SNAPSHOT_H_
