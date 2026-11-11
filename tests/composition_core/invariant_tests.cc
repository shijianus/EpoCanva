#include "test_support.h"

#include "romaji_composer.h"
#include "two_set_composer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace composition_test {
namespace {

template <typename Composer>
void CheckRejectedBytesAndLimit(TestContext& test, std::size_t input_step_limit) {
  using Result = decltype(Composer{}.PushAscii(' '));
  Composer composer;
  Feed(test, composer, "nk", "invariant.rejection_seed");
  const auto original = composer.GetPreedit();
  for (unsigned int byte_value = 0; byte_value <= 255; ++byte_value) {
    if (byte_value >= 0x20 && byte_value <= 0x7e) continue;
    const auto result = composer.PushAscii(static_cast<char>(static_cast<unsigned char>(byte_value)));
    test.Check(result == Result::kRejectedByte, "invariant.rejected_byte");
    CheckSameSnapshot(test, composer.GetPreedit(), original, "invariant.rejection_atomic");
  }
  // Invalid bytes cannot leak into valid UTF-8, end a syllable, or enter undo.
  test.Check(composer.Backspace(), "invariant.rejected_no_history");
  test.Check(composer.GetPreedit().input_step_count == 1, "invariant.rejected_no_history");
  composer.Cancel();
  for (std::size_t input_step_count = 0; input_step_count < input_step_limit; ++input_step_count) {
    test.Check(composer.PushAscii('1') == Result::kAccepted, "invariant.limit_acceptance");
  }
  const auto full = composer.GetPreedit();
  CheckSnapshot(test, full, input_step_limit, "invariant.limit_snapshot");
  test.Check(full.text_utf8 == std::string(input_step_limit, '1'), "invariant.limit_text");
  test.Check(composer.PushAscii('a') == Result::kInputLimit, "invariant.limit_rejection");
  CheckSameSnapshot(test, composer.GetPreedit(), full, "invariant.limit_atomic");
  test.Check(composer.PushAscii('\0') == Result::kRejectedByte, "invariant.byte_validation_first");
  test.Check(composer.Backspace(), "invariant.limit_undo");
  test.Check(composer.PushAscii('2') == Result::kAccepted, "invariant.limit_recovery");
  const std::string expected_utf8 = std::string(input_step_limit - 1, '1') + "2";
  test.Check(composer.CommitUtf8() == expected_utf8, "invariant.limit_commit");
  ExpectState(test, composer, "", 0, 0, 0, "invariant.limit_reset");
  test.Check(composer.PushAscii('3') == Result::kAccepted, "invariant.limit_after_commit");
  composer.Cancel();
  test.Check(composer.PushAscii('4') == Result::kAccepted, "invariant.limit_after_cancel");
}

template <typename Composer>
void CheckAllPrintablePairs(TestContext& test) {
  using Result = decltype(Composer{}.PushAscii(' '));
  for (unsigned int first_byte = 0x20; first_byte <= 0x7e; ++first_byte) {
    for (unsigned int second_byte = 0x20; second_byte <= 0x7e; ++second_byte) {
      Composer composer;
      test.Check(composer.PushAscii(static_cast<char>(first_byte)) == Result::kAccepted,
                 "invariant.pair.first");
      const auto first = composer.GetPreedit();
      CheckSnapshot(test, first, 1, "invariant.pair.first_state");
      test.Check(composer.PushAscii(static_cast<char>(second_byte)) == Result::kAccepted,
                 "invariant.pair.second");
      const auto second = composer.GetPreedit();
      CheckSnapshot(test, second, 2, "invariant.pair.second_state");
      test.Check(composer.Backspace(), "invariant.pair.undo");
      CheckSameSnapshot(test, composer.GetPreedit(), first, "invariant.pair.undo_state");
      test.Check(composer.PushAscii(static_cast<char>(second_byte)) == Result::kAccepted,
                 "invariant.pair.replay");
      CheckSameSnapshot(test, composer.GetPreedit(), second, "invariant.pair.replay_state");
      test.Check(IsStrictUtf8(composer.CommitUtf8()), "invariant.pair.commit_utf8");
      ExpectState(test, composer, "", 0, 0, 0, "invariant.pair.commit_clear");
    }
  }
}

template <typename Composer>
void CheckDeterministicHistoryWalk(TestContext& test) {
  using Snapshot = decltype(Composer{}.GetPreedit());
  using Result = decltype(Composer{}.PushAscii(' '));
  std::uint32_t generator_state = 0x756e6974u;
  constexpr std::size_t sequence_case_count = 32;
  constexpr std::size_t sequence_input_step_count = 96;
  for (std::size_t sequence_case_ordinal = 0; sequence_case_ordinal < sequence_case_count;
       ++sequence_case_ordinal) {
    Composer composer;
    std::string input_ascii;
    std::vector<Snapshot> predecessor_snapshots;
    for (std::size_t input_step_count = 0; input_step_count < sequence_input_step_count;
         ++input_step_count) {
      predecessor_snapshots.push_back(composer.GetPreedit());
      generator_state = generator_state * 1664525u + 1013904223u;
      const char ascii_byte = static_cast<char>(0x20 + (generator_state % 95u));
      input_ascii.push_back(ascii_byte);
      test.Check(composer.PushAscii(ascii_byte) == Result::kAccepted, "invariant.walk.push");
      CheckSnapshot(test, composer.GetPreedit(), input_step_count + 1, "invariant.walk.snapshot");
    }
    Composer independent;
    Feed(test, independent, input_ascii, "invariant.walk.independent");
    CheckSameSnapshot(test, composer.GetPreedit(), independent.GetPreedit(),
                      "invariant.walk.independent_state");
    while (!predecessor_snapshots.empty()) {
      test.Check(composer.Backspace(), "invariant.walk.undo");
      CheckSameSnapshot(test, composer.GetPreedit(), predecessor_snapshots.back(),
                        "invariant.walk.undo_state");
      predecessor_snapshots.pop_back();
    }
    test.Check(!composer.Backspace(), "invariant.walk.empty_undo");
    Feed(test, composer, input_ascii, "invariant.walk.replay");
    CheckSameSnapshot(test, composer.GetPreedit(), independent.GetPreedit(),
                      "invariant.walk.replay_state");
    const auto committed_utf8 = independent.CommitUtf8();
    test.Check(IsStrictUtf8(committed_utf8), "invariant.walk.commit_utf8");
    test.Check(composer.CommitUtf8() == committed_utf8, "invariant.walk.commit_replay");
  }
}

void CheckIndependentDecoder(TestContext& test) {
  test.Check(IsStrictUtf8(""), "utf8.empty");
  test.Check(IsStrictUtf8(u8"ASCII あ한𐀀"), "utf8.valid_scalars");
  const std::vector<std::string> invalid_utf8 = {
      std::string("\x80", 1), std::string("\xc0\xaf", 2), std::string("\xc2", 1),
      std::string("\xe0\x80\xaf", 3), std::string("\xed\xa0\x80", 3),
      std::string("\xf0\x80\x80\xaf", 4), std::string("\xf4\x90\x80\x80", 4),
      std::string("\xf5\x80\x80\x80", 4), std::string("\xe3\x81", 2),
      std::string("\xe3\x41\x82", 3), std::string("\xff", 1)};
  for (const auto& invalid : invalid_utf8) {
    test.Check(!IsStrictUtf8(invalid), "utf8.reject_malformed");
  }
  test.Check(IsUtf8ByteBoundary(u8"あ1", 0), "utf8.byte_start");
  test.Check(!IsUtf8ByteBoundary(u8"あ1", 1), "utf8.not_continuation_one");
  test.Check(!IsUtf8ByteBoundary(u8"あ1", 2), "utf8.not_continuation_two");
  test.Check(IsUtf8ByteBoundary(u8"あ1", 3), "utf8.ascii_start");
  test.Check(IsUtf8ByteBoundary(u8"あ1", 4), "utf8.byte_end");
  test.Check(!IsUtf8ByteBoundary(u8"あ1", 5), "utf8.past_end");
}

void CheckInstanceIsolation(TestContext& test) {
  epocanva::ja::RomajiComposer hiragana;
  epocanva::ja::RomajiComposer katakana(epocanva::ja::KanaMode::kKatakana);
  epocanva::ko::TwoSetComposer korean;
  Feed(test, hiragana, "n", "isolation.ja_pending");
  Feed(test, katakana, "ka", "isolation.ja_mode");
  Feed(test, korean, "rkr", "isolation.ko_pending");
  katakana.Cancel();
  ExpectState(test, hiragana, "n", 0, 1, 1, "isolation.other_ja_unchanged");
  ExpectState(test, korean, u8"각", 0, 3, 3, "isolation.ko_unchanged");
  test.Check(hiragana.CommitUtf8() == u8"ん", "isolation.ja_commit");
  Feed(test, korean, "k", "isolation.ko_migration");
  test.Check(korean.CommitUtf8() == u8"가가", "isolation.ko_migration");
  Feed(test, katakana, "a", "isolation.ja_mode_retained");
  test.Check(katakana.CommitUtf8() == u8"ア", "isolation.ja_mode_retained");
}

}  // namespace

void RunInvariantTests(TestContext& test) {
  CheckIndependentDecoder(test);
  CheckRejectedBytesAndLimit<epocanva::ja::RomajiComposer>(test, epocanva::ja::kMaxInputSteps);
  CheckRejectedBytesAndLimit<epocanva::ko::TwoSetComposer>(test, epocanva::ko::kMaxInputSteps);
  CheckAllPrintablePairs<epocanva::ja::RomajiComposer>(test);
  CheckAllPrintablePairs<epocanva::ko::TwoSetComposer>(test);
  CheckDeterministicHistoryWalk<epocanva::ja::RomajiComposer>(test);
  CheckDeterministicHistoryWalk<epocanva::ko::TwoSetComposer>(test);
  CheckInstanceIsolation(test);
}

}  // namespace composition_test
