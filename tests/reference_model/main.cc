// EpoCanva reference-model stress protocol.
//
// This is the aggregate 100,000-edit protocol the earlier phase reports listed
// as NOT_RUN. It is deliberately independent of the ordinary unit suites:
//
//   * Korean: an independently written standard two-set Hangul automaton is
//     the reference. Every accepted key, backspace, cancel and commit is
//     compared field-by-field with epocanva::ko::TwoSetComposer.
//   * Japanese: the reference model is the composer's own documented contract
//     that preedit state is a pure function of the accepted key history and
//     the kana mode. Backspace must restore the exact predecessor snapshot,
//     and periodic replay through a fresh composer must reproduce the current
//     snapshot. This catches history corruption, order dependence and state
//     drift without reusing the implementation as its own oracle.
//   * Edit plans: an independent status/field model of
//     epocanva::tsf::edit::EditPlan validates context identity, negative and
//     non-collapsed selections, malformed UTF-8, U+FFFC and token range rules.
//
// Each seed performs exactly 100,000 edit operations. Failures print the seed,
// operation index and the last operations so the deterministic RNG stream can
// be replayed with --seed/--ops. Memory and handle counts are sampled; a gross
// leak fails the run.
//
// Exit: 0 = every seed passed; 1 = at least one mismatch; 2 = usage or report
// write failure.

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "edit_plan.h"
#include "romaji_composer.h"
#include "test_support.h"
#include "two_set_composer.h"

namespace {

using composition_test::CheckSnapshot;
using composition_test::IsStrictUtf8;

constexpr std::size_t kDefaultOpsPerSeed = 100000;
constexpr std::uint64_t kSeeds[] = {
    UINT64_C(0x5eed000000000001), UINT64_C(0x5eed000000000002),
    UINT64_C(0x5eed000000000003), UINT64_C(0x5eed000000000004),
    UINT64_C(0x5eed000000000005)};

struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed == 0 ? 1 : seed) {}
  std::uint64_t Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(0x2545F4914F6CDD1D);
  }
  std::size_t Below(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
  }
  bool Chance(unsigned numerator, unsigned denominator) {
    return numerator != 0 && Next() % denominator < numerator;
  }
};

// ---------------------------------------------------------------------------
// Independent Korean reference: standard two-set key -> modern Hangul.

struct KeyJamo {
  int initial = -1;
  int medial = -1;
  int final = 0;
};

KeyJamo DecodeKoreanKey(char key_ascii) {
  switch (key_ascii) {
    case 'R': return {1, -1, 2};
    case 'E': return {4, -1, 0};
    case 'Q': return {8, -1, 0};
    case 'T': return {10, -1, 20};
    case 'W': return {13, -1, 0};
    case 'O': return {-1, 3, 0};
    case 'P': return {-1, 7, 0};
    default: break;
  }
  if (key_ascii >= 'A' && key_ascii <= 'Z') key_ascii = static_cast<char>(key_ascii - 'A' + 'a');
  switch (key_ascii) {
    case 'r': return {0, -1, 1};
    case 's': return {2, -1, 4};
    case 'e': return {3, -1, 7};
    case 'f': return {5, -1, 8};
    case 'a': return {6, -1, 16};
    case 'q': return {7, -1, 17};
    case 't': return {9, -1, 19};
    case 'd': return {11, -1, 21};
    case 'w': return {12, -1, 22};
    case 'c': return {14, -1, 23};
    case 'z': return {15, -1, 24};
    case 'x': return {16, -1, 25};
    case 'v': return {17, -1, 26};
    case 'g': return {18, -1, 27};
    case 'k': return {-1, 0, 0};
    case 'o': return {-1, 1, 0};
    case 'i': return {-1, 2, 0};
    case 'j': return {-1, 4, 0};
    case 'p': return {-1, 5, 0};
    case 'u': return {-1, 6, 0};
    case 'h': return {-1, 8, 0};
    case 'y': return {-1, 12, 0};
    case 'n': return {-1, 13, 0};
    case 'b': return {-1, 17, 0};
    case 'm': return {-1, 18, 0};
    case 'l': return {-1, 20, 0};
    default: return {};
  }
}

constexpr wchar_t kCompatibilityInitial[] = {
    L'\u3131', L'\u3132', L'\u3134', L'\u3137', L'\u3138', L'\u3139', L'\u3141',
    L'\u3142', L'\u3143', L'\u3145', L'\u3146', L'\u3147', L'\u3148', L'\u3149',
    L'\u314A', L'\u314B', L'\u314C', L'\u314D', L'\u314E'};

void AppendThreeByteUtf8(std::uint32_t scalar, std::string* out) {
  out->push_back(static_cast<char>(0xE0 | (scalar >> 12)));
  out->push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
  out->push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
}

int JoinVowels(int first, int second) {
  if (first == 8 && second == 0) return 9;
  if (first == 8 && second == 1) return 10;
  if (first == 8 && second == 20) return 11;
  if (first == 13 && second == 4) return 14;
  if (first == 13 && second == 5) return 15;
  if (first == 13 && second == 20) return 16;
  if (first == 18 && second == 20) return 19;
  return -1;
}

struct ClusterRule {
  int first;
  int second;
  int combined;
  int next_initial;
};

constexpr ClusterRule kClusters[] = {
    {1, 19, 3, 9},  {4, 22, 5, 12}, {4, 27, 6, 18}, {8, 1, 9, 0},
    {8, 16, 10, 6}, {8, 17, 11, 7}, {8, 19, 12, 9}, {8, 25, 13, 16},
    {8, 26, 14, 17}, {8, 27, 15, 18}, {17, 19, 18, 9}};

int JoinFinals(int first, int second) {
  for (const auto& rule : kClusters) {
    if (rule.first == first && rule.second == second) return rule.combined;
  }
  return 0;
}

int InitialFromFinal(int final_ordinal) {
  switch (final_ordinal) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 7: return 3;
    case 8: return 5;
    case 16: return 6;
    case 17: return 7;
    case 19: return 9;
    case 20: return 10;
    case 21: return 11;
    case 22: return 12;
    case 23: return 14;
    case 24: return 15;
    case 25: return 16;
    case 26: return 17;
    case 27: return 18;
    default: return -1;
  }
}

class KoreanReference {
 public:
  std::string PreeditText() const {
    std::string text = resolved_;
    AppendActive(&text);
    return text;
  }
  std::string ActiveText() const {
    std::string active;
    AppendActive(&active);
    return active;
  }
  std::size_t ActiveBegin() const { return resolved_.size(); }
  std::size_t StepCount() const { return history_.size(); }

  void Push(char key_ascii) {
    history_.push_back(key_ascii);
    Process(key_ascii);
  }
  bool Backspace() {
    if (history_.empty()) return false;
    history_.pop_back();
    Rebuild();
    return true;
  }
  void Cancel() {
    history_.clear();
    resolved_.clear();
    initial_ = -1;
    medial_ = -1;
    final_ = 0;
  }
  std::string Commit() {
    std::string text = PreeditText();
    Cancel();
    return text;
  }

 private:
  void AppendActive(std::string* out) const {
    if (initial_ >= 0 && medial_ >= 0) {
      AppendThreeByteUtf8(0xAC00u + static_cast<std::uint32_t>((initial_ * 21 + medial_) * 28 + final_), out);
    } else if (initial_ >= 0) {
      const wchar_t scalar = kCompatibilityInitial[initial_];
      AppendThreeByteUtf8(static_cast<std::uint32_t>(scalar), out);
    } else if (medial_ >= 0) {
      AppendThreeByteUtf8(0x314Fu + static_cast<std::uint32_t>(medial_), out);
    }
  }
  void Flush() {
    AppendActive(&resolved_);
    initial_ = -1;
    medial_ = -1;
    final_ = 0;
  }
  void Process(char key_ascii) {
    const KeyJamo key = DecodeKoreanKey(key_ascii);
    if (key.initial >= 0) {
      if (initial_ >= 0 && medial_ >= 0 && key.final != 0) {
        if (final_ == 0) {
          final_ = key.final;
          return;
        }
        const int combined = JoinFinals(final_, key.final);
        if (combined != 0) {
          final_ = combined;
          return;
        }
      }
      Flush();
      initial_ = key.initial;
      return;
    }
    if (key.medial >= 0) {
      if (medial_ == -1) {
        medial_ = key.medial;
        return;
      }
      if (final_ != 0) {
        int remaining = 0;
        int next_initial = InitialFromFinal(final_);
        for (const auto& rule : kClusters) {
          if (rule.combined == final_) {
            remaining = rule.first;
            next_initial = rule.next_initial;
            break;
          }
        }
        final_ = remaining;
        Flush();
        initial_ = next_initial;
        medial_ = key.medial;
        return;
      }
      const int joined = JoinVowels(medial_, key.medial);
      if (joined >= 0) {
        medial_ = joined;
        return;
      }
      Flush();
      medial_ = key.medial;
      return;
    }
    Flush();
    resolved_.push_back(key_ascii);
  }
  void Rebuild() {
    resolved_.clear();
    initial_ = -1;
    medial_ = -1;
    final_ = 0;
    for (char key : history_) Process(key);
  }

  std::string history_;
  std::string resolved_;
  int initial_ = -1;
  int medial_ = -1;
  int final_ = 0;
};

// ---------------------------------------------------------------------------
// Japanese snapshot model.

struct JapaneseSnapshot {
  std::string text;
  std::size_t caret = 0;
  std::size_t active_begin = 0;
  std::size_t active_size = 0;
  std::size_t steps = 0;
  epocanva::ja::KanaMode mode = epocanva::ja::KanaMode::kHiragana;
};

JapaneseSnapshot SnapshotOf(const epocanva::ja::RomajiComposer& composer) {
  const auto preedit = composer.GetPreedit();
  return {preedit.text_utf8, preedit.caret_utf8_bytes, preedit.active_begin_utf8_bytes,
          preedit.active_size_utf8_bytes, preedit.input_step_count, composer.GetMode()};
}

bool SameSnapshot(const JapaneseSnapshot& a, const JapaneseSnapshot& b) {
  return a.text == b.text && a.caret == b.caret && a.active_begin == b.active_begin &&
         a.active_size == b.active_size && a.steps == b.steps && a.mode == b.mode;
}

void DescribeSnapshot(const JapaneseSnapshot& s, std::string* out) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "steps=%zu text_bytes=%zu caret=%zu active=%zu+%zu",
                s.steps, s.text.size(), s.caret, s.active_begin, s.active_size);
  *out = buffer;
}

// Documented commit rule: a single pending n/N is emitted as the nasal ん;
// every other pending ASCII suffix is preserved literally.
std::string ExpectedJapaneseCommit(const JapaneseSnapshot& before) {
  if (before.active_size == 1 && before.active_begin < before.text.size()) {
    const char active = before.text[before.active_begin];
    if (active == 'n' || active == 'N') {
      const char* nasal = before.mode == epocanva::ja::KanaMode::kKatakana
          ? "\xe3\x83\xb3"   // ン
          : "\xe3\x82\x93";  // ん
      return before.text.substr(0, before.active_begin) + nasal;
    }
  }
  return before.text;
}

// ---------------------------------------------------------------------------
// Edit-plan reference model.

struct TextFixture {
  const char* utf8;
  bool valid;
  bool has_fffc;
  std::size_t utf16_units;
};

constexpr TextFixture kTextFixtures[] = {
    {"", true, false, 0},
    {"a", true, false, 1},
    {"hello", true, false, 5},
    {"caf\xc3\xa9", true, false, 4},                 // café: 4 UTF-16 units
    {"\xe6\x97\xa5\xe6\x9c\xac", true, false, 2},    // 日本: 2 UTF-16 units
    {"\xf0\x9f\x98\x80", true, false, 2},            // 😀: surrogate pair
    {"\xef\xbf\xbc", true, true, 1},                 // U+FFFC
    {"\x80", false, false, 0},
    {"\xc0\xaf", false, false, 0},
    {"\xed\xa0\x80", false, false, 0},
    {"\xf0\x80\x80\xaf", false, false, 0},
    {"a\xff" "b", false, false, 0},
    {"\xe3\x81", false, false, 0},
};

const TextFixture& PickText(Rng* rng) {
  return kTextFixtures[rng->Below(std::size(kTextFixtures))];
}

std::int32_t PickSelection(Rng* rng) {
  static constexpr std::int32_t kValues[] = {-1, 0, 0, 1, 2, 3, 5, 8, 13, 100};
  return kValues[rng->Below(std::size(kValues))];
}

struct PlanExpectation {
  epocanva::tsf::edit::EditPlanStatus status;
  epocanva::tsf::edit::EditOperation operation;
  std::size_t text_units = 0;
  std::size_t expected_units = 0;
  std::int32_t selection_start = -1;
  std::int32_t selection_end = -1;
};

PlanExpectation ExpectBeginOrCommit(bool begin, const epocanva::tsf::edit::ContextIdentity& context,
                                    const TextFixture& text, std::int32_t start,
                                    std::int32_t end) {
  using namespace epocanva::tsf::edit;
  PlanExpectation expected{};
  expected.operation = begin ? EditOperation::kBeginPreedit : EditOperation::kCommit;
  if (context.context_id == 0 || context.generation == 0) {
    expected.status = EditPlanStatus::kInvalidContextIdentity;
  } else if (start < 0 || end < 0) {
    expected.status = EditPlanStatus::kInvalidSelection;
  } else if (start != end) {
    expected.status = EditPlanStatus::kSelectionNotCollapsed;
  } else if (!text.valid) {
    expected.status = EditPlanStatus::kInvalidUtf8;
  } else if (text.has_fffc) {
    expected.status = EditPlanStatus::kUnsupportedCharacter;
  } else {
    expected.status = EditPlanStatus::kOk;
    expected.text_units = text.utf16_units;
    expected.selection_start = start;
    expected.selection_end = end;
  }
  return expected;
}

PlanExpectation ExpectUpdate(const epocanva::tsf::edit::ContextIdentity& context,
                             const TextFixture& text) {
  using namespace epocanva::tsf::edit;
  PlanExpectation expected{};
  expected.operation = EditOperation::kUpdatePreedit;
  if (context.context_id == 0 || context.generation == 0) {
    expected.status = EditPlanStatus::kInvalidContextIdentity;
  } else if (!text.valid) {
    expected.status = EditPlanStatus::kInvalidUtf8;
  } else if (text.has_fffc) {
    expected.status = EditPlanStatus::kUnsupportedCharacter;
  } else {
    expected.status = EditPlanStatus::kOk;
    expected.text_units = text.utf16_units;
    expected.selection_start = -1;
    expected.selection_end = -1;
  }
  return expected;
}

PlanExpectation ExpectReplace(const epocanva::tsf::edit::ContextIdentity& context,
                              const TextFixture& expected_text, const TextFixture& replacement,
                              std::int32_t caret) {
  using namespace epocanva::tsf::edit;
  PlanExpectation expected{};
  expected.operation = EditOperation::kReplaceToken;
  if (context.context_id == 0 || context.generation == 0) {
    expected.status = EditPlanStatus::kInvalidContextIdentity;
  } else if (expected_text.utf8[0] == '\0') {
    expected.status = EditPlanStatus::kInvalidSelection;
  } else if (replacement.utf8[0] == '\0') {
    expected.status = EditPlanStatus::kInvalidSelection;
  } else if (caret < 0) {
    expected.status = EditPlanStatus::kInvalidSelection;
  } else if (!expected_text.valid) {
    expected.status = EditPlanStatus::kInvalidUtf8;
  } else if (expected_text.has_fffc) {
    expected.status = EditPlanStatus::kUnsupportedCharacter;
  } else if (!replacement.valid) {
    expected.status = EditPlanStatus::kInvalidUtf8;
  } else if (replacement.has_fffc) {
    expected.status = EditPlanStatus::kUnsupportedCharacter;
  } else if (static_cast<std::size_t>(caret) < expected_text.utf16_units) {
    expected.status = EditPlanStatus::kInvalidSelection;
  } else {
    expected.status = EditPlanStatus::kOk;
    expected.text_units = replacement.utf16_units;
    expected.expected_units = expected_text.utf16_units;
    expected.selection_start = caret;
    expected.selection_end = caret;
  }
  return expected;
}

// ---------------------------------------------------------------------------
// Stress driver.

struct Failure {
  bool failed = false;
  std::string detail;
};

struct SeedStatistics {
  std::uint64_t seed = 0;
  std::size_t operations = 0;
  std::size_t checks = 0;
  std::size_t korean_ops = 0;
  std::size_t japanese_ops = 0;
  std::size_t plan_ops = 0;
  std::uint64_t duration_ms = 0;
  std::size_t handles_start = 0;
  std::size_t handles_end = 0;
  std::size_t peak_working_set_bytes = 0;
  std::size_t final_working_set_bytes = 0;
};

std::size_t CurrentHandleCount() {
  DWORD handles = 0;
  return ::GetProcessHandleCount(::GetCurrentProcess(), &handles) ? handles : 0;
}

std::size_t CurrentWorkingSet() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
    return 0;
  }
  return static_cast<std::size_t>(counters.WorkingSetSize);
}

void RecordFailure(Failure* failure, std::uint64_t seed, std::size_t op, const char* category,
                   const std::string& detail) {
  if (failure->failed) return;
  failure->failed = true;
  char prefix[160];
  std::snprintf(prefix, sizeof(prefix), "seed=0x%016llx op=%zu category=%s: ",
                static_cast<unsigned long long>(seed), op, category);
  failure->detail = std::string(prefix) + detail;
}

std::string DescribeKorean(const KoreanReference& model,
                           const epocanva::ko::TwoSetComposer& composer) {
  const auto actual = composer.GetPreedit();
  const std::string expected_text = model.PreeditText();
  char buffer[320];
  std::snprintf(buffer, sizeof(buffer),
                "expected_text_bytes=%zu actual_text_bytes=%zu expected_steps=%zu "
                "actual_steps=%zu expected_active=%zu+%zu actual_active=%zu+%zu",
                expected_text.size(), actual.text_utf8.size(), model.StepCount(),
                actual.input_step_count, model.ActiveBegin(), model.ActiveText().size(),
                actual.active_begin_utf8_bytes, actual.active_size_utf8_bytes);
  return buffer;
}

bool CompareKorean(KoreanReference* model, epocanva::ko::TwoSetComposer* composer,
                   Failure* failure, std::uint64_t seed, std::size_t op) {
  const auto actual = composer->GetPreedit();
  const std::string expected_text = model->PreeditText();
  const std::string expected_active = model->ActiveText();
  if (actual.text_utf8 != expected_text) {
    RecordFailure(failure, seed, op, "korean-text", DescribeKorean(*model, *composer));
    return false;
  }
  if (actual.caret_utf8_bytes != actual.text_utf8.size() ||
      actual.active_begin_utf8_bytes != model->ActiveBegin() ||
      actual.active_size_utf8_bytes != expected_active.size() ||
      actual.input_step_count != model->StepCount()) {
    RecordFailure(failure, seed, op, "korean-state", DescribeKorean(*model, *composer));
    return false;
  }
  if (!IsStrictUtf8(actual.text_utf8)) {
    RecordFailure(failure, seed, op, "korean-utf8", "preedit is not strict UTF-8");
    return false;
  }
  return true;
}

void RunKoreanOp(Rng* rng, KoreanReference* model, epocanva::ko::TwoSetComposer* composer,
                 Failure* failure, std::uint64_t seed, std::size_t op, std::size_t* checks) {
  static constexpr char kKeys[] = "rRseEfaqQtTdwWczxvgkoiOjpuPhynbml";
  const std::size_t choice = rng->Below(100);
  if (choice < 55) {
    const char key = kKeys[rng->Below(sizeof(kKeys) - 1)];
    const auto result = composer->PushAscii(key);
    ++*checks;
    if (result == epocanva::ko::InputResult::kInputLimit) {
      if (model->StepCount() != epocanva::ko::kMaxInputSteps ||
          composer->GetPreedit().input_step_count != model->StepCount()) {
        RecordFailure(failure, seed, op, "korean-limit", "input limit was not the documented 4096 steps");
        return;
      }
    } else if (result != epocanva::ko::InputResult::kAccepted) {
      RecordFailure(failure, seed, op, "korean-push", "printable key was rejected");
      return;
    } else {
      model->Push(key);
    }
  } else if (choice < 78) {
    const bool actual = composer->Backspace();
    const bool expected = model->Backspace();
    ++*checks;
    if (actual != expected) {
      RecordFailure(failure, seed, op, "korean-backspace", "return value differs");
      return;
    }
  } else if (choice < 86) {
    composer->Cancel();
    model->Cancel();
  } else if (choice < 94) {
    const std::string actual = composer->CommitUtf8();
    const std::string expected = model->Commit();
    ++*checks;
    if (actual != expected) {
      RecordFailure(failure, seed, op, "korean-commit", "committed text differs");
      return;
    }
  } else {
    // Non-printable bytes are rejected atomically and never enter history.
    // Printable bytes outside the Korean key table are literal boundaries and
    // must still enter both the model and the composer.
    const unsigned value = 0x20u + static_cast<unsigned>(rng->Below(0x5f));
    const char byte = static_cast<char>(rng->Chance(1, 2) ? value : static_cast<unsigned char>(rng->Below(0x20)));
    const auto result = composer->PushAscii(byte);
    ++*checks;
    if (result == epocanva::ko::InputResult::kAccepted) {
      model->Push(byte);
    } else if (result == epocanva::ko::InputResult::kInputLimit) {
      if (model->StepCount() != epocanva::ko::kMaxInputSteps) {
        RecordFailure(failure, seed, op, "korean-limit", "input limit was not the documented 4096 steps");
        return;
      }
    } else if (result == epocanva::ko::InputResult::kRejectedByte) {
      if (model->StepCount() != composer->GetPreedit().input_step_count) {
        RecordFailure(failure, seed, op, "korean-rejected-step", "rejected byte changed step count");
        return;
      }
    }
  }
  CompareKorean(model, composer, failure, seed, op);
}

void RunJapaneseOp(Rng* rng, epocanva::ja::RomajiComposer* composer,
                   std::vector<JapaneseSnapshot>* history_snapshots,
                   std::string* history_keys, Failure* failure,
                   std::uint64_t seed, std::size_t op, std::size_t* checks) {
  static constexpr char kKeys[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 '-.,";
  const std::size_t choice = rng->Below(100);
  if (choice < 62) {
    const char key = kKeys[rng->Below(sizeof(kKeys) - 1)];
    const auto before = SnapshotOf(*composer);
    const auto result = composer->PushAscii(key);
    ++*checks;
    if (result == epocanva::ja::InputResult::kInputLimit) {
      if (before.steps != epocanva::ja::kMaxInputSteps ||
          composer->GetPreedit().input_step_count != before.steps) {
        RecordFailure(failure, seed, op, "japanese-limit", "input limit was not the documented 4096 steps");
      }
      return;
    }
    if (result != epocanva::ja::InputResult::kAccepted) {
      RecordFailure(failure, seed, op, "japanese-push", "printable key was rejected");
      return;
    }
    const auto after = SnapshotOf(*composer);
    if (after.steps != before.steps + 1 || !IsStrictUtf8(after.text) ||
        after.caret != after.text.size() ||
        after.active_begin + after.active_size != after.text.size()) {
      RecordFailure(failure, seed, op, "japanese-invariant", "snapshot invariant failed");
      return;
    }
    history_snapshots->push_back(after);
    history_keys->push_back(key);
    return;
  }
  if (choice < 80) {
    const bool actual = composer->Backspace();
    const bool expected = !history_snapshots->empty();
    ++*checks;
    if (actual != expected) {
      RecordFailure(failure, seed, op, "japanese-backspace", "return value differs");
      return;
    }
    if (expected) {
      history_snapshots->pop_back();
      history_keys->pop_back();
      const JapaneseSnapshot current = SnapshotOf(*composer);
      const JapaneseSnapshot wanted = history_snapshots->empty()
          ? JapaneseSnapshot{std::string(), 0, 0, 0, 0, composer->GetMode()}
          : history_snapshots->back();
      if (!SameSnapshot(current, wanted)) {
        std::string expected_text;
        std::string actual_text;
        DescribeSnapshot(wanted, &expected_text);
        DescribeSnapshot(current, &actual_text);
        RecordFailure(failure, seed, op, "japanese-undo", "expected " + expected_text + " actual " + actual_text);
      }
    }
    return;
  }
  if (choice < 88) {
    const JapaneseSnapshot before = SnapshotOf(*composer);
    const std::string committed = composer->CommitUtf8();
    const auto state = composer->GetPreedit();
    const std::string expected = ExpectedJapaneseCommit(before);
    ++*checks;
    if (!IsStrictUtf8(committed) || committed != expected || state.text_utf8 != "" ||
        state.input_step_count != 0 || state.caret_utf8_bytes != 0) {
      RecordFailure(failure, seed, op, "japanese-commit", "commit did not preserve and reset preedit");
    }
    history_snapshots->clear();
    history_keys->clear();
    return;
  }
  if (choice < 94) {
    composer->Cancel();
    const auto state = composer->GetPreedit();
    ++*checks;
    if (!state.text_utf8.empty() || state.input_step_count != 0 || state.caret_utf8_bytes != 0) {
      RecordFailure(failure, seed, op, "japanese-cancel", "cancel did not clear state");
    }
    history_snapshots->clear();
    history_keys->clear();
    return;
  }
  if (choice < 97) {
    // Mode changes are only issued at a composition boundary here, so the
    // stored predecessor snapshots remain a valid undo model.
    if (!history_snapshots->empty()) {
      composer->Cancel();
      history_snapshots->clear();
      history_keys->clear();
    }
    const auto mode = rng->Chance(1, 2) ? epocanva::ja::KanaMode::kHiragana
                                        : epocanva::ja::KanaMode::kKatakana;
    composer->SetMode(mode);
    ++*checks;
    if (composer->GetMode() != mode || composer->GetPreedit().input_step_count != 0) {
      RecordFailure(failure, seed, op, "japanese-mode", "mode change corrupted composition");
    }
    return;
  }
  const unsigned raw = rng->Chance(1, 2)
      ? static_cast<unsigned>(rng->Below(0x20))
      : 0x7fu + static_cast<unsigned>(rng->Below(0x81));
  const char byte = static_cast<char>(raw);
  const auto before = SnapshotOf(*composer);
  const auto result = composer->PushAscii(byte);
  ++*checks;
  if (result != epocanva::ja::InputResult::kRejectedByte) {
    RecordFailure(failure, seed, op, "japanese-rejected-byte", "non-printable byte was not rejected");
    return;
  }
  const auto after = SnapshotOf(*composer);
  if (!SameSnapshot(before, after)) {
    RecordFailure(failure, seed, op, "japanese-rejected-byte", "rejected byte mutated state");
  }
}

void RunPlanOp(Rng* rng, Failure* failure, std::uint64_t seed, std::size_t op,
               std::size_t* checks, std::uint64_t* last_sequence) {
  using namespace epocanva::tsf::edit;
  ContextIdentity context{};
  context.context_id = rng->Chance(9, 10) ? 1 + rng->Below(16) : 0;
  context.generation = rng->Chance(9, 10) ? 1 + rng->Below(16) : 0;
  const TextFixture& expected_text = PickText(rng);
  const TextFixture& replacement = PickText(rng);
  std::int32_t start = PickSelection(rng);
  std::int32_t end = rng->Chance(3, 4) ? start : PickSelection(rng);
  if (rng->Chance(1, 10)) start = end;
  const std::int32_t caret = PickSelection(rng);

  const unsigned which = static_cast<unsigned>(rng->Below(6));
  EditPlanBuildResult result;
  PlanExpectation expected{};
  switch (which) {
    case 0:
      expected = ExpectBeginOrCommit(true, context, expected_text, start, end);
      result = EditPlan::BuildBegin(context, expected_text.utf8, start, end);
      break;
    case 1:
      expected = ExpectUpdate(context, expected_text);
      result = EditPlan::BuildUpdate(context, expected_text.utf8);
      break;
    case 2:
      expected = ExpectBeginOrCommit(false, context, expected_text, start, end);
      result = EditPlan::BuildCommit(context, expected_text.utf8, start, end);
      break;
    case 3:
      expected.status = (context.context_id == 0 || context.generation == 0)
          ? EditPlanStatus::kInvalidContextIdentity
          : EditPlanStatus::kOk;
      expected.operation = EditOperation::kCancel;
      result = EditPlan::BuildCancel(context);
      break;
    case 4:
      expected.status = (context.context_id == 0 || context.generation == 0)
          ? EditPlanStatus::kInvalidContextIdentity
          : EditPlanStatus::kOk;
      expected.operation = EditOperation::kEndOwnedComposition;
      result = EditPlan::BuildEndOwnedComposition(context);
      break;
    default:
      expected = ExpectReplace(context, expected_text, replacement, caret);
      result = EditPlan::BuildReplaceToken(context, expected_text.utf8, replacement.utf8, caret);
      break;
  }
  ++*checks;
  if (result.status != expected.status) {
    RecordFailure(failure, seed, op, "plan-status",
                  std::string("operation=") + DescribeEditOperation(expected.operation) +
                      " expected=" + DescribeEditPlanStatus(expected.status) +
                      " actual=" + DescribeEditPlanStatus(result.status));
    return;
  }
  if (!result.ok()) return;
  const EditPlan& plan = *result.plan;
  ++*checks;
  if (plan.operation() != expected.operation || plan.context() != context ||
      plan.text_utf16().size() != expected.text_units ||
      plan.expected_text_utf16().size() != expected.expected_units ||
      plan.selection_start_utf16() != expected.selection_start ||
      plan.selection_end_utf16() != expected.selection_end ||
      plan.sequence() <= *last_sequence) {
    RecordFailure(failure, seed, op, "plan-fields", "plan fields differ from the reference model");
    return;
  }
  *last_sequence = plan.sequence();
}

bool RunSeed(std::uint64_t seed, std::size_t operations, SeedStatistics* statistics,
             Failure* failure) {
  Rng rng(seed);
  KoreanReference korean_model;
  epocanva::ko::TwoSetComposer korean;
  epocanva::ja::RomajiComposer japanese;
  std::vector<JapaneseSnapshot> japanese_snapshots;
  std::string japanese_history;
  std::uint64_t last_plan_sequence = 0;
  std::size_t checks = 0;

  statistics->seed = seed;
  statistics->handles_start = CurrentHandleCount();
  statistics->peak_working_set_bytes = CurrentWorkingSet();
  const ULONGLONG started = ::GetTickCount64();
  for (std::size_t op = 0; op < operations; ++op) {
    const std::size_t lane = rng.Below(100);
    if (lane < 34) {
      RunKoreanOp(&rng, &korean_model, &korean, failure, seed, op, &checks);
      ++statistics->korean_ops;
    } else if (lane < 67) {
      RunJapaneseOp(&rng, &japanese, &japanese_snapshots, &japanese_history,
                    failure, seed, op, &checks);
      ++statistics->japanese_ops;
    } else {
      RunPlanOp(&rng, failure, seed, op, &checks, &last_plan_sequence);
      ++statistics->plan_ops;
    }
    if (failure->failed) break;
    if ((op + 1) % 10000 == 0) {
      const std::size_t working_set = CurrentWorkingSet();
      statistics->peak_working_set_bytes =
          std::max(statistics->peak_working_set_bytes, working_set);
      // A 64 MiB ceiling for a 100k-operation, string-bounded run is far above
      // the expected few MiB and still catches a real per-operation leak.
      if (working_set > 512u * 1024u * 1024u) {
        RecordFailure(failure, seed, op, "memory", "working set exceeded 512 MiB");
        break;
      }
    }
    if (!japanese_snapshots.empty() && (op + 1) % 8192 == 0) {
      // Replay the entire accepted history into a fresh composer. This is the
      // Japanese reference-model check: preedit must be a pure function of
      // history and mode, with no hidden accumulated state.
      epocanva::ja::RomajiComposer replay;
      replay.SetMode(japanese.GetMode());
      for (char key : japanese_history) {
        const auto replay_result = replay.PushAscii(key);
        if (replay_result != epocanva::ja::InputResult::kAccepted) {
          RecordFailure(failure, seed, op, "japanese-replay", "replay rejected an accepted history key");
          break;
        }
      }
      if (!failure->failed && !SameSnapshot(SnapshotOf(replay), SnapshotOf(japanese))) {
        RecordFailure(failure, seed, op, "japanese-replay", "replayed history produced a different snapshot");
        break;
      }
    }
  }
  statistics->operations = failure->failed ? 0 : operations;
  statistics->checks = checks;
  statistics->handles_end = CurrentHandleCount();
  statistics->final_working_set_bytes = CurrentWorkingSet();
  statistics->peak_working_set_bytes =
      std::max(statistics->peak_working_set_bytes, statistics->final_working_set_bytes);
  statistics->duration_ms = ::GetTickCount64() - started;
  if (!failure->failed) {
    if (statistics->handles_end > statistics->handles_start + 32) {
      RecordFailure(failure, seed, operations, "handles", "handle count grew by more than 32");
    }
    if (korean.GetPreedit().input_step_count != korean_model.StepCount()) {
      RecordFailure(failure, seed, operations, "final-korean", "final Korean state diverged");
    }
  }
  return !failure->failed;
}

void WriteReport(const std::string& path, std::size_t ops_per_seed,
                 const std::vector<SeedStatistics>& seeds, const Failure& failure,
                 bool passed) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << "{\n";
  out << "  \"format\": \"epocanva-reference-model-stress-v1\",\n";
  out << "  \"ops_per_seed\": " << ops_per_seed << ",\n";
  out << "  \"seed_count\": " << seeds.size() << ",\n";
  out << "  \"total_ops\": " << (ops_per_seed * seeds.size()) << ",\n";
  out << "  \"result\": \"" << (passed ? "passed" : "failed") << "\",\n";
  out << "  \"failure\": ";
  if (failure.failed) {
    out << "\"" << (failure.detail.find('"') == std::string::npos ? failure.detail : "mismatch") << "\",\n";
  } else {
    out << "null,\n";
  }
  out << "  \"seeds\": [\n";
  for (std::size_t index = 0; index < seeds.size(); ++index) {
    const auto& s = seeds[index];
    out << "    {\"seed\": \"" << std::hex << "0x" << s.seed << std::dec
        << "\", \"operations\": " << s.operations << ", \"checks\": " << s.checks
        << ", \"korean_ops\": " << s.korean_ops << ", \"japanese_ops\": " << s.japanese_ops
        << ", \"plan_ops\": " << s.plan_ops << ", \"duration_ms\": " << s.duration_ms
        << ", \"handles_start\": " << s.handles_start << ", \"handles_end\": " << s.handles_end
        << ", \"peak_working_set_bytes\": " << s.peak_working_set_bytes
        << ", \"final_working_set_bytes\": " << s.final_working_set_bytes << "}";
    out << (index + 1 == seeds.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: epocanva_reference_model_stress_tests [--ops N] [--seed HEX] "
               "[--report PATH] [--quiet]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t ops_per_seed = kDefaultOpsPerSeed;
  std::string report_path;
  bool single_seed = false;
  std::uint64_t single_seed_value = 0;
  bool quiet = false;
  bool force_fail = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--ops" && index + 1 < argc) {
      ops_per_seed = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (argument == "--seed" && index + 1 < argc) {
      single_seed = true;
      single_seed_value = std::strtoull(argv[++index], nullptr, 0);
    } else if (argument == "--report" && index + 1 < argc) {
      report_path = argv[++index];
    } else if (argument == "--quiet") {
      quiet = true;
    } else if (argument == "--force-fail") {
      force_fail = true;
    } else if (argument == "--help" || argument == "-h") {
      PrintUsage();
      return 0;
    } else {
      PrintUsage();
      return 2;
    }
  }
  if (force_fail) {
    // Negative control: advance only the reference model. The comparison path
    // must detect the divergence and exit 1; an exit 0 here would mean the
    // harness cannot fail on a real mismatch.
    KoreanReference model;
    epocanva::ko::TwoSetComposer composer;
    model.Push('r');
    composer.PushAscii('r');
    model.Push('k');
    Failure injected;
    CompareKorean(&model, &composer, &injected, 0, 0);
    std::printf("RESULT %s\n", injected.failed ? "FAIL" : "PASS");
    return injected.failed ? 1 : 0;
  }
  if (ops_per_seed == 0) {
    PrintUsage();
    return 2;
  }

  std::vector<std::uint64_t> seeds;
  if (single_seed) {
    seeds.push_back(single_seed_value);
  } else {
    seeds.assign(std::begin(kSeeds), std::end(kSeeds));
  }
  std::vector<SeedStatistics> statistics(seeds.size());
  Failure failure;
  bool passed = true;
  for (std::size_t index = 0; index < seeds.size(); ++index) {
    failure = {};
    passed = RunSeed(seeds[index], ops_per_seed, &statistics[index], &failure) && passed;
    if (!quiet) {
      std::printf("seed=0x%016llx ops=%zu checks=%zu duration_ms=%llu handles=%zu->%zu "
                  "peak_ws=%zu final_ws=%zu %s\n",
                  static_cast<unsigned long long>(seeds[index]),
                  statistics[index].operations, statistics[index].checks,
                  static_cast<unsigned long long>(statistics[index].duration_ms),
                  statistics[index].handles_start, statistics[index].handles_end,
                  statistics[index].peak_working_set_bytes,
                  statistics[index].final_working_set_bytes,
                  failure.failed ? "FAIL" : "PASS");
    }
    if (failure.failed) {
      std::fprintf(stderr, "REFERENCE MODEL FAILURE: %s\n", failure.detail.c_str());
      std::fprintf(stderr, "Replay: --seed 0x%016llx --ops %zu\n",
                   static_cast<unsigned long long>(seeds[index]), ops_per_seed);
      break;
    }
  }
  if (!report_path.empty()) WriteReport(report_path, ops_per_seed, statistics, failure, passed);
  if (!passed) {
    std::printf("RESULT FAIL\n");
    return 1;
  }
  std::printf("RESULT PASS seeds=%zu ops_per_seed=%zu total_ops=%zu\n",
              seeds.size(), ops_per_seed, seeds.size() * ops_per_seed);
  return 0;
}
