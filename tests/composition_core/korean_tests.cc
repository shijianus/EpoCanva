#include "test_support.h"

#include "two_set_composer.h"

#include <array>
#include <string>
#include <string_view>

namespace composition_test {
namespace {

using epocanva::ko::TwoSetComposer;

struct ConversionCase {
  const char* case_id;
  std::string_view input_ascii;
  std::string_view expected_utf8;
};

void CheckConversions(TestContext& test) {
  constexpr ConversionCase cases[] = {
      {"ko.hello", "dkssudgktpdy", u8"안녕하세요"},
      {"ko.hangul", "gksrmf", u8"한글"},
      {"ko.republic", "eogksalsrnr", u8"대한민국"},
      {"ko.simple_migration", "rkrk", u8"가가"},
      {"ko.shifted_final_migration", "rkRk", u8"가까"},
      {"ko.shifted_s_final_migration", "rkTk", u8"가싸"},
      {"ko.cluster_word", "ekfr", u8"닭"},
      {"ko.cluster_word_migration", "ekfrk", u8"달가"},
      {"ko.compound_medial_final", "rhkrt", u8"곿"},
      {"ko.compound_medial_migration", "rhkrtk", u8"곽사"},
      {"ko.mixed_digits_punctuation", "gksrmf123! dkssud?", u8"한글123! 안녕?"},
      {"ko.literal_boundary", "rk1k", u8"가1ㅏ"},
      {"ko.letters_are_keys_not_english", "abc", u8"뮻"},
      {"ko.uppercase_nonshifted", "GKS", u8"한"},
      {"ko.shifted_initials", "RkEkQkTkWk", u8"까따빠싸짜"},
      {"ko.shifted_vowels", "dOdP", u8"얘예"},
      {"ko.no_implicit_onset", "k", u8"ㅏ"},
      {"ko.explicit_onset", "dk", u8"아"},
      {"ko.initial_without_vowel", "r", u8"ㄱ"},
      {"ko.vowel_then_consonant", "kr", u8"ㅏㄱ"},
      {"ko.repeated_initial", "rrk", u8"ㄱ가"},
      {"ko.repeated_s_initial", "ttk", u8"ㅅ사"},
      {"ko.repeated_shifted_initial", "RRk", u8"ㄲ까"},
      {"ko.repeated_final", "rkrrk", u8"각가"},
      {"ko.repeated_s_final", "rkttk", u8"갓사"},
      {"ko.no_tense_d_final", "rkE", u8"가ㄸ"},
      {"ko.no_tense_b_final", "rkQ", u8"가ㅃ"},
      {"ko.no_tense_j_final", "rkW", u8"가ㅉ"},
      {"ko.repeated_vowels", "kk", u8"ㅏㅏ"},
      {"ko.no_ae_alias", "kl", u8"ㅏㅣ"},
      {"ko.no_e_alias", "jl", u8"ㅓㅣ"},
      {"ko.no_three_step_wae_alias", "hkl", u8"ㅘㅣ"},
      {"ko.no_three_step_we_alias", "njl", u8"ㅝㅣ"},
      {"ko.repeated_syllable_vowel", "rkk", u8"가ㅏ"},
      {"ko.no_initial_cluster", "rtk", u8"ㄱ사"},
      {"ko.no_three_component_final", "rkrtgk", u8"갃하"},
      {"ko.literal_only", "0123456789 !@#$%^&*()_+-=[]{};:'\",.<>/?`~\\|", "0123456789 !@#$%^&*()_+-=[]{};:'\",.<>/?`~\\|"},
      {"ko.empty", "", ""},
  };
  for (const auto& conversion : cases) {
    TwoSetComposer composer;
    Feed(test, composer, conversion.input_ascii, conversion.case_id);
    CheckSnapshot(test, composer.GetPreedit(), conversion.input_ascii.size(), conversion.case_id);
    const std::string committed_utf8 = composer.CommitUtf8();
    test.Check(committed_utf8 == conversion.expected_utf8, conversion.case_id);
    test.Check(IsStrictUtf8(committed_utf8), conversion.case_id);
    ExpectState(test, composer, "", 0, 0, 0, conversion.case_id);
    test.Check(!composer.Backspace(), conversion.case_id);
    test.Check(composer.CommitUtf8().empty(), conversion.case_id);
  }

  constexpr ConversionCase single_keys[] = {
      {"ko.key.r", "r", u8"ㄱ"}, {"ko.key.R", "R", u8"ㄲ"},
      {"ko.key.s", "s", u8"ㄴ"}, {"ko.key.e", "e", u8"ㄷ"},
      {"ko.key.E", "E", u8"ㄸ"}, {"ko.key.f", "f", u8"ㄹ"},
      {"ko.key.a", "a", u8"ㅁ"}, {"ko.key.q", "q", u8"ㅂ"},
      {"ko.key.Q", "Q", u8"ㅃ"}, {"ko.key.t", "t", u8"ㅅ"},
      {"ko.key.T", "T", u8"ㅆ"}, {"ko.key.d", "d", u8"ㅇ"},
      {"ko.key.w", "w", u8"ㅈ"}, {"ko.key.W", "W", u8"ㅉ"},
      {"ko.key.c", "c", u8"ㅊ"}, {"ko.key.z", "z", u8"ㅋ"},
      {"ko.key.x", "x", u8"ㅌ"}, {"ko.key.v", "v", u8"ㅍ"},
      {"ko.key.g", "g", u8"ㅎ"}, {"ko.key.k", "k", u8"ㅏ"},
      {"ko.key.o", "o", u8"ㅐ"}, {"ko.key.i", "i", u8"ㅑ"},
      {"ko.key.O", "O", u8"ㅒ"}, {"ko.key.j", "j", u8"ㅓ"},
      {"ko.key.p", "p", u8"ㅔ"}, {"ko.key.u", "u", u8"ㅕ"},
      {"ko.key.P", "P", u8"ㅖ"}, {"ko.key.h", "h", u8"ㅗ"},
      {"ko.key.y", "y", u8"ㅛ"}, {"ko.key.n", "n", u8"ㅜ"},
      {"ko.key.b", "b", u8"ㅠ"}, {"ko.key.m", "m", u8"ㅡ"},
      {"ko.key.l", "l", u8"ㅣ"},
  };
  for (const auto& key : single_keys) {
    TwoSetComposer composer;
    Feed(test, composer, key.input_ascii, key.case_id);
    ExpectState(test, composer, key.expected_utf8, 0, 3, 1, key.case_id);
    test.Check(composer.CommitUtf8() == key.expected_utf8, key.case_id);
  }
}

void CheckCompounds(TestContext& test) {
  constexpr ConversionCase vowels[] = {
      {"ko.vowel.wa", "hk", u8"ㅘ"}, {"ko.vowel.wae", "ho", u8"ㅙ"},
      {"ko.vowel.oe", "hl", u8"ㅚ"}, {"ko.vowel.wo", "nj", u8"ㅝ"},
      {"ko.vowel.we", "np", u8"ㅞ"}, {"ko.vowel.wi", "nl", u8"ㅟ"},
      {"ko.vowel.ui", "ml", u8"ㅢ"},
  };
  for (const auto& vowel : vowels) {
    TwoSetComposer composer;
    Feed(test, composer, vowel.input_ascii, vowel.case_id);
    ExpectState(test, composer, vowel.expected_utf8, 0, 3, 2, vowel.case_id);
    test.Check(composer.CommitUtf8() == vowel.expected_utf8, vowel.case_id);
  }
  struct ClusterCase {
    const char* case_id;
    std::string_view keys_ascii;
    std::string_view cluster_utf8;
    std::string_view moved_utf8;
    std::string_view before_second_final_utf8;
  };
  constexpr ClusterCase clusters[] = {
      {"ko.cluster.gs", "rkrt", u8"갃", u8"각사", u8"각"},
      {"ko.cluster.nj", "rksw", u8"갅", u8"간자", u8"간"},
      {"ko.cluster.nh", "rksg", u8"갆", u8"간하", u8"간"},
      {"ko.cluster.lg", "rkfr", u8"갉", u8"갈가", u8"갈"},
      {"ko.cluster.lm", "rkfa", u8"갊", u8"갈마", u8"갈"},
      {"ko.cluster.lb", "rkfq", u8"갋", u8"갈바", u8"갈"},
      {"ko.cluster.ls", "rkft", u8"갌", u8"갈사", u8"갈"},
      {"ko.cluster.lt", "rkfx", u8"갍", u8"갈타", u8"갈"},
      {"ko.cluster.lp", "rkfv", u8"갎", u8"갈파", u8"갈"},
      {"ko.cluster.lh", "rkfg", u8"갏", u8"갈하", u8"갈"},
      {"ko.cluster.bs", "rkqt", u8"값", u8"갑사", u8"갑"},
  };
  for (const auto& cluster : clusters) {
    TwoSetComposer composer;
    Feed(test, composer, cluster.keys_ascii, cluster.case_id);
    ExpectState(test, composer, cluster.cluster_utf8, 0, 3, 4, cluster.case_id);
    Feed(test, composer, "k", cluster.case_id);
    ExpectState(test, composer, cluster.moved_utf8, 3, 3, 5, cluster.case_id);
    test.Check(composer.Backspace(), cluster.case_id);
    ExpectState(test, composer, cluster.cluster_utf8, 0, 3, 4, cluster.case_id);
    test.Check(composer.Backspace(), cluster.case_id);
    ExpectState(test, composer, cluster.before_second_final_utf8, 0, 3, 3, cluster.case_id);
    test.Check(composer.Backspace(), cluster.case_id);
    ExpectState(test, composer, u8"가", 0, 3, 2, cluster.case_id);
  }
}

void CheckHistoryAndLifecycle(TestContext& test) {
  TwoSetComposer composer;
  Feed(test, composer, "r", "ko.trace.initial");
  ExpectState(test, composer, u8"ㄱ", 0, 3, 1, "ko.trace.initial");
  Feed(test, composer, "h", "ko.trace.medial");
  ExpectState(test, composer, u8"고", 0, 3, 2, "ko.trace.medial");
  Feed(test, composer, "k", "ko.trace.compound_medial");
  ExpectState(test, composer, u8"과", 0, 3, 3, "ko.trace.compound_medial");
  Feed(test, composer, "r", "ko.trace.final");
  ExpectState(test, composer, u8"곽", 0, 3, 4, "ko.trace.final");
  Feed(test, composer, "t", "ko.trace.compound_final");
  ExpectState(test, composer, u8"곿", 0, 3, 5, "ko.trace.compound_final");
  Feed(test, composer, "k", "ko.trace.migration");
  ExpectState(test, composer, u8"곽사", 3, 3, 6, "ko.trace.migration");
  Feed(test, composer, "9", "ko.trace.literal");
  ExpectState(test, composer, u8"곽사9", 7, 0, 7, "ko.trace.literal");
  constexpr std::array<std::string_view, 7> undo_texts = {
      u8"곽사", u8"곿", u8"곽", u8"과", u8"고", u8"ㄱ", ""};
  std::size_t remaining_input_step_count = 7;
  for (std::string_view expected_utf8 : undo_texts) {
    test.Check(composer.Backspace(), "ko.undo.trace");
    --remaining_input_step_count;
    const std::size_t active_begin_utf8_bytes = remaining_input_step_count == 6 ? 3 : 0;
    const std::size_t active_size_utf8_bytes = expected_utf8.empty() ? 0 : 3;
    ExpectState(test, composer, expected_utf8, active_begin_utf8_bytes,
                active_size_utf8_bytes, remaining_input_step_count, "ko.undo.trace");
  }
  test.Check(!composer.Backspace(), "ko.undo.empty");
  Feed(test, composer, "rkR", "ko.undo.tense_final");
  ExpectState(test, composer, u8"갂", 0, 3, 3, "ko.undo.tense_final");
  test.Check(composer.Backspace(), "ko.undo.tense_is_single_step");
  ExpectState(test, composer, u8"가", 0, 3, 2, "ko.undo.tense_is_single_step");
  composer.Cancel();
  Feed(test, composer, "O", "ko.undo.shifted_vowel");
  test.Check(composer.Backspace(), "ko.undo.shifted_vowel");
  ExpectState(test, composer, "", 0, 0, 0, "ko.undo.shifted_vowel");

  constexpr std::array<std::string_view, 9> pending_inputs = {
      "r", "k", "hk", "rk", "rkr", "rkrt", "rkrtk", "rk1", "R"};
  for (std::string_view input_ascii : pending_inputs) {
    Feed(test, composer, input_ascii, "ko.cancel.pending");
    composer.Cancel();
    ExpectState(test, composer, "", 0, 0, 0, "ko.cancel.pending");
    test.Check(composer.CommitUtf8().empty(), "ko.cancel.no_commit");
    test.Check(!composer.Backspace(), "ko.cancel.no_undo");
  }
  Feed(test, composer, "rkr", "ko.commit.boundary");
  test.Check(composer.CommitUtf8() == u8"각", "ko.commit.boundary");
  Feed(test, composer, "k", "ko.commit.no_migration");
  test.Check(composer.CommitUtf8() == u8"ㅏ", "ko.commit.no_migration");
  test.Check(!composer.Backspace(), "ko.commit.no_old_history");
}

void CheckEveryModernSyllable(TestContext& test) {
  // Independent keyboard sequences in Unicode alphabet order. Sweep expected
  // scalars in order, rather than call/duplicate the implementation's formula.
  constexpr std::array<std::string_view, 19> initial_keys = {
      "r", "R", "s", "e", "E", "f", "a", "q", "Q", "t",
      "T", "d", "w", "W", "c", "z", "x", "v", "g"};
  constexpr std::array<std::string_view, 21> medial_keys = {
      "k", "o", "i", "O", "j", "p", "u", "P", "h", "hk", "ho",
      "hl", "y", "n", "nj", "np", "nl", "b", "m", "ml", "l"};
  constexpr std::array<std::string_view, 28> final_keys = {
      "", "r", "R", "rt", "s", "sw", "sg", "e", "f", "fr", "fa", "fq", "ft", "fx",
      "fv", "fg", "a", "q", "qt", "t", "T", "d", "w", "c", "z", "x", "v", "g"};
  std::uint32_t expected_unicode_scalar = 0xac00;
  std::size_t syllable_case_count = 0;
  for (std::string_view initial_ascii : initial_keys) {
    for (std::string_view medial_ascii : medial_keys) {
      for (std::string_view final_ascii : final_keys) {
        TwoSetComposer composer;
        const std::string input_ascii = std::string(initial_ascii) +
                                        std::string(medial_ascii) + std::string(final_ascii);
        Feed(test, composer, input_ascii, "ko.exhaustive.accepted");
        const auto snapshot = composer.GetPreedit();
        CheckSnapshot(test, snapshot, input_ascii.size(), "ko.exhaustive.snapshot");
        test.Check(snapshot.active_begin_utf8_bytes == 0 && snapshot.active_size_utf8_bytes == 3,
                   "ko.exhaustive.active_bytes");
        test.Check(DecodeOneThreeByteScalar(snapshot.text_utf8) == expected_unicode_scalar,
                   "ko.exhaustive.unicode_scalar");
        test.Check(composer.CommitUtf8() == snapshot.text_utf8, "ko.exhaustive.commit");
        test.Check(composer.GetPreedit().input_step_count == 0, "ko.exhaustive.clear");
        ++expected_unicode_scalar;
        ++syllable_case_count;
      }
    }
  }
  test.Check(expected_unicode_scalar == 0xd7a4, "ko.exhaustive.final_scalar");
  test.Check(syllable_case_count == 11172, "ko.exhaustive.syllable_case_count");
}

}  // namespace

void RunKoreanTests(TestContext& test) {
  CheckConversions(test);
  CheckCompounds(test);
  CheckHistoryAndLifecycle(test);
  CheckEveryModernSyllable(test);
}

}  // namespace composition_test
