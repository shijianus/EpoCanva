#include "test_support.h"

#include "romaji_composer.h"

#include <array>
#include <string_view>

namespace composition_test {
namespace {

using epocanva::ja::KanaMode;
using epocanva::ja::RomajiComposer;

struct ConversionCase {
  const char* case_id;
  std::string_view input_ascii;
  std::string_view expected_utf8;
};

void CheckConversions(TestContext& test) {
  // Authored expected text, not generated from the implementation's rule table.
  constexpr ConversionCase cases[] = {
      {"ja.basic_vowels", "aiueo", u8"あいうえお"},
      {"ja.k_row", "kakikukeko", u8"かきくけこ"},
      {"ja.s_row", "sashisuseso", u8"さしすせそ"},
      {"ja.t_row", "tachitsuteto", u8"たちつてと"},
      {"ja.n_row", "naninuneno", u8"なにぬねの"},
      {"ja.h_row", "hahifuheho", u8"はひふへほ"},
      {"ja.m_row", "mamimumemo", u8"まみむめも"},
      {"ja.y_row", "yayuyo", u8"やゆよ"},
      {"ja.r_row", "rarirurero", u8"らりるれろ"},
      {"ja.w_row", "wawiwewo", u8"わうぃうぇを"},
      {"ja.g_row", "gagigugego", u8"がぎぐげご"},
      {"ja.z_row", "zajizuzezo", u8"ざじずぜぞ"},
      {"ja.d_row", "dadidudedo", u8"だぢづでど"},
      {"ja.b_row", "babibubebo", u8"ばびぶべぼ"},
      {"ja.p_row", "papipupepo", u8"ぱぴぷぺぽ"},
      {"ja.aliases", "sisishichititutsuhufuziji", u8"しししちちつつふふじじ"},
      {"ja.k_yoon", "kyakyukyo", u8"きゃきゅきょ"},
      {"ja.g_yoon", "gyagyugyo", u8"ぎゃぎゅぎょ"},
      {"ja.s_yoon", "shashusho", u8"しゃしゅしょ"},
      {"ja.s_yoon_alias", "syasyusyo", u8"しゃしゅしょ"},
      {"ja.j_yoon", "jajujo", u8"じゃじゅじょ"},
      {"ja.j_yoon_alias", "zyazyuzyojyajyujyo", u8"じゃじゅじょじゃじゅじょ"},
      {"ja.ch_yoon", "chachucho", u8"ちゃちゅちょ"},
      {"ja.ch_yoon_alias", "tyatyutyocyacyucyo", u8"ちゃちゅちょちゃちゅちょ"},
      {"ja.d_yoon", "dyadyudyo", u8"ぢゃぢゅぢょ"},
      {"ja.n_yoon", "nyanyunyo", u8"にゃにゅにょ"},
      {"ja.h_yoon", "hyahyuhyo", u8"ひゃひゅひょ"},
      {"ja.b_yoon", "byabyubyo", u8"びゃびゅびょ"},
      {"ja.p_yoon", "pyapyupyo", u8"ぴゃぴゅぴょ"},
      {"ja.m_yoon", "myamyumyo", u8"みゃみゅみょ"},
      {"ja.r_yoon", "ryaryuryo", u8"りゃりゅりょ"},
      {"ja.small_vowels_x", "xaxixuxexo", u8"ぁぃぅぇぉ"},
      {"ja.small_vowels_l", "lalilulelo", u8"ぁぃぅぇぉ"},
      {"ja.small_y_x", "xyaxyuxyo", u8"ゃゅょ"},
      {"ja.small_y_l", "lyalyulyo", u8"ゃゅょ"},
      {"ja.small_tsu", "xtuxtsultultsu", u8"っっっっ"},
      {"ja.small_wa_ka_ke", "xwalwaxkalkaxkelke", u8"ゎゎゕゕゖゖ"},
      {"ja.foreign_vowels", "fafifefo", u8"ふぁふぃふぇふぉ"},
      {"ja.voiced_u", "vavivuvevo", u8"ゔぁゔぃゔゔぇゔぉ"},
      {"ja.foreign_yoon", "fyafyufyovyavyuvyo", u8"ふゃふゅふょゔゃゔゅゔょ"},
      {"ja.foreign_e", "shesyejejyechetyecye", u8"しぇしぇじぇじぇちぇちぇちぇ"},
      {"ja.foreign_ts", "tsatsitsetso", u8"つぁつぃつぇつぉ"},
      {"ja.foreign_th", "thathithuthetho", u8"てゃてぃてゅてぇてょ"},
      {"ja.foreign_dh", "dhadhidhudhedho", u8"でゃでぃでゅでぇでょ"},
      {"ja.foreign_wh", "whawhiwhuwhewho", u8"うぁうぃううぇうぉ"},
      {"ja.sokuon_k", "gakkou", u8"がっこう"},
      {"ja.sokuon_sh", "issho", u8"いっしょ"},
      {"ja.sokuon_tch", "matcha", u8"まっちゃ"},
      {"ja.sokuon_cch", "maccha", u8"まっちゃ"},
      {"ja.sokuon_yoon", "kyakka", u8"きゃっか"},
      {"ja.sokuon_repeat", "kkka", u8"っっか"},
      {"ja.sokuon_pending_commit", "kk", u8"っk"},
      {"ja.double_x_not_sokuon", "xxa", u8"xぁ"},
      {"ja.double_l_not_sokuon", "lla", u8"lぁ"},
      {"ja.q_not_sokuon", "qqa", u8"qqあ"},
      {"ja.uppercase", "KaNaSHI", u8"かなし"},
      {"ja.n_commit", "n", u8"ん"},
      {"ja.n_before_consonant", "kanpai", u8"かんぱい"},
      {"ja.n_before_boundary", "n1n n!", u8"ん1ん ん!"},
      {"ja.n_apostrophe_vowel", "n'a", u8"んあ"},
      {"ja.n_apostrophe_yoon", "n'ya", u8"んや"},
      {"ja.n_apostrophe_consumed", "n'", u8"ん"},
      {"ja.n_yoon_not_nasal", "nya", u8"にゃ"},
      {"ja.nn_commit", "nn", u8"ん"},
      {"ja.nn_vowel_reuse", "nna", u8"んな"},
      {"ja.nn_yoon_reuse", "nnya", u8"んにゃ"},
      {"ja.nn_boundary", "nn ", u8"ん "},
      {"ja.nn_apostrophe", "nn'a", u8"んあ"},
      {"ja.nnn_commit", "nnn", u8"んん"},
      {"ja.nnnn_commit", "nnnn", u8"んん"},
      {"ja.nn_consonant", "nnka", u8"んか"},
      {"ja.konnichiwa", "konnichiha", u8"こんにちは"},
      {"ja.incomplete_ny", "ny", "ny"},
      {"ja.incomplete_uppercase", "SH", "SH"},
      {"ja.failed_prefix", "kza", u8"kざ"},
      {"ja.failed_tc_prefix", "tca", u8"tcあ"},
      {"ja.failed_ny_prefix", "nyq", "nyq"},
      {"ja.mixed_punctuation", "Qka12,sh!n?", u8"Qか12,sh!ん?"},
      {"ja.apostrophe_literal", "ka'na", u8"か'な"},
      {"ja.prolonged_mark", "ko-hi-", u8"こーひー"},
      {"ja.empty_commit", "", ""},
  };
  for (const auto& conversion : cases) {
    RomajiComposer composer;
    Feed(test, composer, conversion.input_ascii, conversion.case_id);
    CheckSnapshot(test, composer.GetPreedit(), conversion.input_ascii.size(), conversion.case_id);
    const std::string committed_utf8 = composer.CommitUtf8();
    test.Check(committed_utf8 == conversion.expected_utf8, conversion.case_id);
    test.Check(IsStrictUtf8(committed_utf8), conversion.case_id);
    ExpectState(test, composer, "", 0, 0, 0, conversion.case_id);
    test.Check(!composer.Backspace(), conversion.case_id);
    test.Check(composer.CommitUtf8().empty(), conversion.case_id);
  }
}

void CheckIncrementalAndHistory(TestContext& test) {
  RomajiComposer composer;
  Feed(test, composer, "k", "ja.trace.k");
  ExpectState(test, composer, "k", 0, 1, 1, "ja.trace.k");
  Feed(test, composer, "y", "ja.trace.ky");
  ExpectState(test, composer, "ky", 0, 2, 2, "ja.trace.ky");
  Feed(test, composer, "a", "ja.trace.kya");
  ExpectState(test, composer, u8"きゃ", 6, 0, 3, "ja.trace.kya");
  Feed(test, composer, "1", "ja.trace.literal");
  ExpectState(test, composer, u8"きゃ1", 7, 0, 4, "ja.trace.literal");
  Feed(test, composer, "n", "ja.trace.n");
  ExpectState(test, composer, u8"きゃ1n", 7, 1, 5, "ja.trace.n");
  Feed(test, composer, "'", "ja.trace.apostrophe");
  ExpectState(test, composer, u8"きゃ1ん", 10, 0, 6, "ja.trace.apostrophe");
  test.Check(composer.Backspace(), "ja.undo.apostrophe");
  ExpectState(test, composer, u8"きゃ1n", 7, 1, 5, "ja.undo.apostrophe");
  test.Check(composer.Backspace(), "ja.undo.n");
  ExpectState(test, composer, u8"きゃ1", 7, 0, 4, "ja.undo.n");
  test.Check(composer.Backspace(), "ja.undo.literal");
  ExpectState(test, composer, u8"きゃ", 6, 0, 3, "ja.undo.literal");
  test.Check(composer.Backspace(), "ja.undo.yoon");
  ExpectState(test, composer, "ky", 0, 2, 2, "ja.undo.yoon");
  test.Check(composer.Backspace(), "ja.undo.y");
  ExpectState(test, composer, "k", 0, 1, 1, "ja.undo.y");
  test.Check(composer.Backspace(), "ja.undo.k");
  test.Check(!composer.Backspace(), "ja.undo.empty");

  Feed(test, composer, "kk", "ja.trace.sokuon");
  ExpectState(test, composer, u8"っk", 3, 1, 2, "ja.trace.sokuon");
  test.Check(composer.Backspace(), "ja.undo.sokuon");
  ExpectState(test, composer, "k", 0, 1, 1, "ja.undo.sokuon");
  composer.Cancel();
  Feed(test, composer, "n", "ja.trace.n_start");
  ExpectState(test, composer, "n", 0, 1, 1, "ja.trace.n_start");
  Feed(test, composer, "n", "ja.trace.nn");
  ExpectState(test, composer, u8"ん", 3, 0, 2, "ja.trace.nn");
  Feed(test, composer, "a", "ja.trace.nna");
  ExpectState(test, composer, u8"んな", 6, 0, 3, "ja.trace.nna");
  test.Check(composer.Backspace(), "ja.undo.nna");
  ExpectState(test, composer, u8"ん", 3, 0, 2, "ja.undo.nna");
  test.Check(composer.Backspace(), "ja.undo.nn");
  ExpectState(test, composer, "n", 0, 1, 1, "ja.undo.nn");
  composer.Cancel();
  Feed(test, composer, "tc", "ja.trace.tc");
  ExpectState(test, composer, "tc", 0, 2, 2, "ja.trace.tc");
  Feed(test, composer, "h", "ja.trace.tch");
  ExpectState(test, composer, u8"っch", 3, 2, 3, "ja.trace.tch");
  test.Check(composer.Backspace(), "ja.undo.tch");
  ExpectState(test, composer, "tc", 0, 2, 2, "ja.undo.tch");
  composer.Cancel();
  Feed(test, composer, "xn", "ja.trace.literal_n");
  ExpectState(test, composer, "xn", 1, 1, 2, "ja.trace.literal_n");
  test.Check(composer.CommitUtf8() == u8"xん", "ja.commit.literal_n");
}

void CheckModeAndLifecycle(TestContext& test) {
  RomajiComposer composer;
  test.Check(composer.GetMode() == KanaMode::kHiragana, "ja.mode.default");
  Feed(test, composer, "kyaN", "ja.mode.input");
  composer.SetMode(KanaMode::kKatakana);
  ExpectState(test, composer, u8"キャN", 6, 1, 4, "ja.mode.rerender");
  test.Check(composer.GetMode() == KanaMode::kKatakana, "ja.mode.get");
  test.Check(composer.Backspace(), "ja.mode.undo_input_only");
  ExpectState(test, composer, u8"キャ", 6, 0, 3, "ja.mode.undo_input_only");
  test.Check(composer.Backspace(), "ja.mode.undo_yoon");
  ExpectState(test, composer, "ky", 0, 2, 2, "ja.mode.undo_yoon");
  Feed(test, composer, "aQ1-", "ja.mode.literal");
  ExpectState(test, composer, u8"キャQ1ー", 11, 0, 6, "ja.mode.literal");
  composer.SetMode(KanaMode::kHiragana);
  ExpectState(test, composer, u8"きゃQ1ー", 11, 0, 6, "ja.mode.hiragana");
  composer.SetMode(KanaMode::kKatakana);
  composer.Cancel();
  ExpectState(test, composer, "", 0, 0, 0, "ja.cancel.clears");
  test.Check(composer.GetMode() == KanaMode::kKatakana, "ja.cancel.retains_mode");
  test.Check(!composer.Backspace(), "ja.cancel.no_history");
  Feed(test, composer, "n", "ja.commit.katakana_n");
  test.Check(composer.CommitUtf8() == u8"ン", "ja.commit.katakana_n");
  test.Check(composer.GetMode() == KanaMode::kKatakana, "ja.commit.retains_mode");
  Feed(test, composer, "a", "ja.commit.new_boundary");
  test.Check(composer.CommitUtf8() == u8"ア", "ja.commit.new_boundary");
  Feed(test, composer, "nn", "ja.cancel.hidden_n");
  composer.Cancel();
  Feed(test, composer, "a", "ja.cancel.hidden_n_reset");
  test.Check(composer.CommitUtf8() == u8"ア", "ja.cancel.hidden_n_reset");

  constexpr ConversionCase katakana_cases[] = {
      {"ja.katakana.sokuon", "gakkou", u8"ガッコウ"},
      {"ja.katakana.loanword", "konpyu-ta-", u8"コンピューター"},
      {"ja.katakana.no_m_nasal_alias", "kompyu-ta-", u8"コmピューター"},
      {"ja.katakana.small", "xaxixuxexoxyaxyuxyoxtsuxwaxkaxke", u8"ァィゥェォャュョッヮヵヶ"},
      {"ja.katakana.voiced_u", "vavivuvevo", u8"ヴァヴィヴヴェヴォ"},
  };
  for (const auto& conversion : katakana_cases) {
    RomajiComposer katakana(KanaMode::kKatakana);
    Feed(test, katakana, conversion.input_ascii, conversion.case_id);
    test.Check(katakana.CommitUtf8() == conversion.expected_utf8, conversion.case_id);
  }

  // Every pending shape: commit preserves incomplete non-n bytes; cancel drops.
  constexpr std::array<std::string_view, 10> pending_inputs = {
      "k", "sh", "ny", "xts", "tc", "KY", "n", "nn", "n'", "kk"};
  for (std::string_view input_ascii : pending_inputs) {
    RomajiComposer cancelled;
    Feed(test, cancelled, input_ascii, "ja.cancel.pending");
    cancelled.Cancel();
    ExpectState(test, cancelled, "", 0, 0, 0, "ja.cancel.pending");
    test.Check(cancelled.CommitUtf8().empty(), "ja.cancel.no_commit");
    test.Check(!cancelled.Backspace(), "ja.cancel.no_undo");
  }
}

}  // namespace

void RunJapaneseTests(TestContext& test) {
  CheckConversions(test);
  CheckIncrementalAndHistory(test);
  CheckModeAndLifecycle(test);
}

}  // namespace composition_test
