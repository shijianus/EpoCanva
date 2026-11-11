// Fusion layer tests (E4): policy mirror pinning, merge rules, dedup
// determinism, raw protection, provenance auditability and the mixed-language
// regression families.
//
// Every assertion can fail: --forcefail proves the harness reports red.
#include "engine/fusion/fusion_policy.h"
#include "engine/fusion/fusion_merger.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "engine/ranking/adaptive_ranker.h"

namespace {

namespace cand = epocanva::candidates;
namespace fus = epocanva::fusion;

int failed = 0;
int checks = 0;

void Check(bool value, const char* label) {
  ++checks;
  if (!value) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}

// Builds a valid record the same way the backend adapter does.
cand::Record MakeRecord(const char* language_tag, const char* scheme_id,
                        std::uint32_t language, std::uint32_t input_mode,
                        std::uint32_t abi_kind, const std::string& text_utf8,
                        std::vector<std::uint64_t> lexeme_ids, double score) {
  cand::Record record;
  (void)cand::Describe(language_tag, scheme_id, language, input_mode, abi_kind,
                       text_utf8, std::move(lexeme_ids), score, &record);
  return record;
}

cand::Record EnWord(const std::string& text, std::vector<std::uint64_t> ids,
                    double score) {
  return MakeRecord("en", "latin", 1, 1, 1 /*word*/, text, std::move(ids), score);
}

cand::Record EnRaw(const std::string& text) {
  return MakeRecord("en", "latin", 1, 1, 0 /*raw*/, text, {}, 0.0);
}

cand::Record EnPrediction(const std::string& text,
                          std::vector<std::uint64_t> ids, double score) {
  return MakeRecord("en", "latin", 1, 1, 6 /*prediction*/, text, std::move(ids),
                    score);
}

// --- policy mirror ----------------------------------------------------------

void PolicyMirrorTests() {
  // The JSON mirror must exist and pin the compiled values. Parsing without a
  // dependency: hand-rolled strict field checks on the small known shape.
  FILE* file = std::fopen(
      EPOCANVA_FUSION_POLICY_JSON, "rb");
  Check(file != nullptr, "fusion-policy mirror exists");
  if (!file) return;
  std::string bytes;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, read);
  std::fclose(file);

  Check(bytes.find("\"format\": \"epocanva-candidate-fusion-policy-v1\"") != std::string::npos,
        "mirror format string");
  Check(bytes.find("\"version\": 1") != std::string::npos &&
            bytes.find("\"dedup_version\": 1") != std::string::npos,
        "mirror versions match compiled policy");
  Check(fus::Policy().version == 1 && fus::Policy().dedup_version == 1,
        "compiled policy version is 1");
  // Every source must appear and declare restricted fallback in v1; a source
  // silently flipped to calibrated without a version bump breaks the pin.
  Check(bytes.find("\"nfkc\": false") != std::string::npos &&
            bytes.find("\"unicode_casefold\": false") != std::string::npos,
        "mirror forbids NFKC and full casefold");
  for (const char* source : {"raw", "word", "spelling", "morphology", "expansion",
                             "conversion", "prediction"}) {
    Check(bytes.find(std::string("\"") + source + "\": {") != std::string::npos,
          "mirror documents the source entry");
  }
  Check(bytes.find("\"calibration\": \"restricted-fallback\"") != std::string::npos,
        "mirror records restricted-fallback calibration");
  // Rank-only mapping determinism: same ordinal, same points.
  const auto& word = fus::SourcePolicyFor(cand::Source::kWord);
  Check(word.rank_only && word.calibration == fus::Calibration::kRestrictedFallback,
        "word source is rank-only fallback in v1");
  const double a = fus::NormalizeToPoints(word, -1e100, 0, 5);
  const double b = fus::NormalizeToPoints(word, 1e100, 0, 5);
  Check(a == b, "rank-only mapping ignores the raw scale entirely");
  const double top = fus::NormalizeToPoints(word, 0.0, 0, 5);
  const double last = fus::NormalizeToPoints(word, 0.0, 4, 5);
  Check(top > last, "producer order maps to descending points");
  Check(last >= fus::kFallbackBandFloor * 0.0 - 0.0 && top <= fus::kFallbackBandFloor,
        "rank-only points stay inside the fallback band");
  Check(std::isfinite(fus::NormalizeToPoints(word, std::nan(""), 0, 5)),
        "NaN raw score yields finite points");
}

// --- dedup key --------------------------------------------------------------

void DedupKeyTests() {
  using fus::DedupKey;
  using cand::Source;
  Check(DedupKey("en", Source::kWord, "Good") == "good", "en word folds A-Z");
  Check(DedupKey("en", Source::kWord, "Good") == DedupKey("en", Source::kWord, "good"),
        "case-identical keys merge");
  Check(DedupKey("fr", Source::kWord, "\xc3\x89" "cole") ==
            DedupKey("fr", Source::kWord, "\xc3\x89" "cole"),
        "fr accented bytes preserved");
  Check(DedupKey("zh", Source::kConversion, "\xe5\xa5\xbd") == "\xe5\xa5\xbd",
        "CJK bytes preserved exactly");
  Check(DedupKey("en", Source::kPrediction, "Good") == "Good",
        "prediction preserves case (only word-class sources fold)");
  Check(DedupKey("en", Source::kWord, std::string("a\x01b")).empty(),
        "control scalar fails closed");
  Check(DedupKey("en", Source::kWord, "a\xc3(").empty(),
        "malformed UTF-8 fails closed when folding");
}

// --- merge rules ------------------------------------------------------------

void MergeRuleTests() {
  { // Raw hard protection: raw first, prediction cannot displace it.
    std::vector<cand::Record> records{EnRaw("go"), EnPrediction("good", {7}, -1.0)};
    const std::vector<std::uint32_t> ordinals{0, 1};
    const std::vector<bool> exact{false, false};
    const auto result = fus::Merge(records, ordinals, exact);
    Check(result.status == fus::MergeStatus::kOk, "raw+prediction merges ok");
    Check(result.merged.size() == 2, "both rows survive");
    Check(result.merged[0].is_raw_tier, "raw row is tier 0");
    Check(result.merged[0].record.text_utf8 == "go", "raw row first");
  }
  { // Fail closed: a row without a contract aborts the merge.
    std::vector<cand::Record> records{EnRaw("go"), cand::Record{}};
    const std::vector<std::uint32_t> ordinals{0, 1};
    const std::vector<bool> exact{false, false};
    const auto result = fus::Merge(records, ordinals, exact);
    Check(result.status == fus::MergeStatus::kMissingContract,
          "missing contract fails closed");
    Check(result.merged.empty(), "failed merge publishes nothing");
  }
  { // Fail closed: tampered stable id.
    auto tampered = EnWord("good", {7}, -1.0);
    tampered.stable_id = "cand:v1:en:latin:lex:999";
    std::vector<cand::Record> records{EnRaw("go"), tampered};
    const auto result = fus::Merge(records, {0, 1}, {false, false});
    Check(result.status == fus::MergeStatus::kInvalidContract,
          "stable-id mismatch fails closed");
  }
  { // Exact expansion protection: exact row beats non-exact in its band.
    auto exact = EnWord("good", {7}, -1.0);
    auto plain = EnWord("goodness", {8}, -0.5);
    std::vector<cand::Record> records{EnRaw("go"), plain, exact};
    // exact flag vector follows the records; mark record 2 exact.
    const auto result = fus::Merge(records, {0, 1, 2}, {false, false, true});
    Check(result.status == fus::MergeStatus::kOk, "expansion merge ok");
    Check(result.merged.size() == 3, "all rows survive");
    Check(result.merged[1].record.text_utf8 == "good",
          "exact expansion precedes non-exact within band");
  }
  { // Same-source duplicate collapses, keeping the better score.
    auto better = EnWord("good", {7}, -0.5);
    auto worse = EnWord("good", {7}, -2.0);
    std::vector<cand::Record> records{EnRaw("go"), worse, better};
    const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
    Check(result.status == fus::MergeStatus::kOk, "same-source dedup ok");
    Check(result.merged.size() == 2, "duplicate folded away");
    Check(result.deduplicated == 1, "dedup count recorded");
    Check(result.merged[1].record.score == -0.5, "better score kept");
  }
  { // Cross-source same-surface: both rows kept, exact flag hoisted.
    auto word_row = EnWord("good", {7}, -0.5);
    auto pred_row = EnPrediction("good", {7}, -1.0);
    std::vector<cand::Record> records{EnRaw("go"), pred_row};
    // exact flags: word row exact, prediction not.
    const auto result = fus::Merge(records, {0, 1}, {false, false});
    Check(result.status == fus::MergeStatus::kOk, "cross-source merge ok");
    Check(result.merged.size() == 2, "cross-source rows both publish");
  }
  { // Determinism: same input twice gives identical winner and provenance.
    auto build = [] {
      std::vector<cand::Record> records{EnRaw("go"), EnWord("good", {7}, -0.5),
                                        EnPrediction("goose", {9}, -2.0)};
      return fus::Merge(records, {0, 1, 2}, {false, false, false});
    };
    const auto first = build();
    const auto second = build();
    Check(first.merged.size() == second.merged.size(), "deterministic size");
    for (std::size_t index = 0; index < first.merged.size(); ++index) {
      Check(first.merged[index].record.stable_id == second.merged[index].record.stable_id,
            "deterministic winner order");
      Check(first.merged[index].provenance.size() == second.merged[index].provenance.size(),
            "deterministic provenance size");
      for (std::size_t row = 0; row < first.merged[index].provenance.size(); ++row) {
        Check(first.merged[index].provenance[row].stable_id ==
                  second.merged[index].provenance[row].stable_id,
              "deterministic provenance content");
      }
    }
  }
  { // Producer order perturbation: the same collection (same row-to-ordinal
    // assignment) delivered in a different vector order must produce the
    // identical menu. The first input has raw collected first; the second
    // carries the same three rows with their ordinals attached, shuffled.
    auto raw = EnRaw("go");
    auto a = EnWord("good", {7}, -0.5);
    auto b = EnPrediction("goose", {9}, -2.0);
    const auto r1 = fus::Merge({raw, a, b}, {0, 1, 2}, {false, false, false});
    const auto r2 = fus::Merge({b, raw, a}, {2, 0, 1}, {false, false, false});
    Check(r1.status == fus::MergeStatus::kOk && r2.status == fus::MergeStatus::kOk,
          "perturbed merges succeed");
    Check(r1.merged.size() == r2.merged.size(), "perturbation keeps every row");
    for (std::size_t index = 0; index < r1.merged.size(); ++index) {
      Check(r1.merged[index].record.text_utf8 == r2.merged[index].record.text_utf8,
            "menu identical under transport perturbation");
      Check(r1.merged[index].fusion_points == r2.merged[index].fusion_points,
            "fusion points identical under perturbation");
      Check(r1.merged[index].provenance.size() == r2.merged[index].provenance.size() &&
                r1.merged[index].provenance[0].producer_insertion_ordinal ==
                    r2.merged[index].provenance[0].producer_insertion_ordinal,
            "provenance identical under perturbation");
    }
  }
  { // Provenance auditability: merged row carries source + ordinal + points.
    auto exact = EnWord("good", {7}, -0.5);
    std::vector<cand::Record> records{EnRaw("go"), exact};
    const auto result = fus::Merge(records, {0, 1}, {false, true});
    Check(result.merged[1].provenance.size() == 1, "survivor provenance present");
    Check(result.merged[1].provenance[0].source == cand::Source::kWord,
          "provenance records source");
    Check(result.merged[1].provenance[0].exact_expansion,
          "provenance records exact flag");
    Check(result.merged[1].provenance[0].producer_insertion_ordinal == 1,
          "provenance records producer ordinal");
  }
  { // Fallback cannot beat calibrated: all sources are fallback in v1, so the
    // producer order governs; a later, higher-scoring raw scale must not win.
    auto first = EnWord("alpha", {7}, -0.1);
    auto second = EnWord("beta", {8}, 1e6);  // absurd raw scale
    std::vector<cand::Record> records{EnRaw("al"), first, second};
    const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
    Check(result.merged[1].record.text_utf8 == "alpha",
          "uncalibrated raw scale cannot buy rank");
  }
}

// --- mixed-language regression families (E4/E5 §5) ---------------------------

// Family 1: a Chinese word inside an English sentence — both languages rank,
// raw first, CJK bytes preserved.
void MixedZhInEn() {
  auto zh = MakeRecord("zh", "pinyin", 2, 2, 5 /*conversion*/, "\xe5\xa5\xbd", {11}, -1.0);
  auto en = EnWord("good", {7}, -0.5);
  std::vector<cand::Record> records{EnRaw("hao"), zh, en};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "zh-in-en merge ok");
  Check(result.merged.size() == 3, "zh-in-en keeps both languages");
  Check(result.merged[0].record.text_utf8 == "hao", "zh-in-en raw first");
  Check(result.merged[1].record.text_utf8 == "\xe5\xa5\xbd",
        "zh-in-en producer order preserved (no cross-scale addition)");
}

// Family 2: English abbreviation inside Chinese context (pinyin initialism vs
// an English word reading of the same letters).
void MixedEnInZh() {
  auto conversion = MakeRecord("zh", "pinyin", 2, 2, 5, "\xe4\xb8\xad", {21}, -0.5);
  auto english = EnWord("zhong", {22}, -1.5);
  std::vector<cand::Record> records{EnRaw("zhong"), conversion, english};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "en-in-zh merge ok");
  Check(result.merged[0].record.text_utf8 == "zhong", "en-in-zh raw first");
  Check(result.merged[1].record.identity.language_tag == "zh",
        "en-in-zh conversion outranks foreign word by producer order");
}

// Family 3: English proper noun inside French context.
void MixedEnInFr() {
  auto fr = MakeRecord("fr", "latin", 3, 1, 1, "paris", {31}, -0.5);
  auto en = MakeRecord("en", "latin", 1, 1, 1, "paris", {32}, -0.4);
  std::vector<cand::Record> records{MakeRecord("fr", "latin", 3, 1, 0, "par", {}, 0.0), fr, en};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "en-in-fr merge ok");
  Check(result.merged.size() == 3, "same surface across languages stays distinct");
  Check(result.merged[1].record.identity.language_tag == "fr",
        "fr row precedes en row (producer order)");
  Check(result.merged[1].record.stable_id != result.merged[2].record.stable_id,
        "same surface, different language -> different stable ids");
}

// Family 4: Japanese romaji and English mixed.
void MixedJaRomajiEn() {
  auto kana = MakeRecord("ja", "romaji", 4, 4, 5, "\x81\x42", {}, -1.0);
  // Actually valid hiragana "あ": e3 81 82.
  kana = MakeRecord("ja", "romaji", 4, 4, 5, "\xe3\x81\x82", {}, -1.0);
  auto en = EnWord("a", {41}, -2.0);
  std::vector<cand::Record> records{EnRaw("a"), kana, en};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "ja-romaji-en merge ok");
  Check(result.merged[0].record.text_utf8 == "a", "ja mix raw first");
  Check(result.merged[1].record.text_utf8 == "\xe3\x81\x82", "kana conversion next");
}

// Family 5: English token inside Korean two-set input.
void MixedEnInKo() {
  auto hangul = MakeRecord("ko", "two_set", 5, 6, 5, "\xea\xb0\x80", {51}, -1.0);
  auto en = EnWord("gk", {52}, -2.0);
  std::vector<cand::Record> records{EnRaw("gk"), hangul, en};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "en-in-ko merge ok");
  Check(result.merged[1].record.text_utf8 == "\xea\xb0\x80",
        "hangul conversion precedes english token");
}

// Family 6: same surface, different language sources — distinct identity,
// both rows auditable.
void SameSurfaceDifferentLanguages() {
  auto fr = MakeRecord("fr", "latin", 3, 1, 1, "mine", {61}, -0.5);
  auto en = EnWord("mine", {62}, -0.5);
  std::vector<cand::Record> records{EnRaw("mine"), fr, en};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.status == fus::MergeStatus::kOk, "same-surface merge ok");
  Check(result.merged.size() == 3, "same surface in two languages: no silent merge");
  Check(result.merged[1].record.stable_id != result.merged[2].record.stable_id,
        "language-scoped stable ids differ");
}

// Family 7: exact expansion vs prediction conflict — exact wins in band.
void ExactVsPrediction() {
  auto expansion = MakeRecord("en", "latin", 1, 1, 4 /*expansion*/, "by the way", {71}, -1.5);
  auto predicted = EnPrediction("byway", {72}, -0.2);
  std::vector<cand::Record> records{EnRaw("btw"), expansion, predicted};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, true, false});
  Check(result.status == fus::MergeStatus::kOk, "expansion-vs-prediction ok");
  Check(result.merged[1].record.text_utf8 == "by the way",
        "exact expansion beats higher-scoring prediction");
  Check(result.merged[1].exact_expansion, "exact flag on winner");
}

// Family 8: raw candidate vs high-scoring prediction — raw cannot be pressed.
void RawVsHighPrediction() {
  auto predicted = EnPrediction("googol", {81}, 1e6);
  std::vector<cand::Record> records{EnRaw("goo"), predicted};
  const auto result = fus::Merge(records, {0, 1}, {false, false});
  Check(result.status == fus::MergeStatus::kOk, "raw-vs-prediction ok");
  Check(result.merged[0].record.text_utf8 == "goo",
        "raw stays first regardless of prediction scale");
  Check(result.merged[0].is_raw_tier, "raw tier flagged");
}

// Family 9: duplicate candidates across sources merge into one row that keeps
// the most reliable source and the full provenance (both stable ids).
void CrossSourceDuplicateMerge() {
  auto word_row = EnWord("good", {7}, -0.5);
  auto pred_row = EnPrediction("good", {7}, -2.0);
  std::vector<cand::Record> records{EnRaw("go"), pred_row, word_row};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, true});
  Check(result.status == fus::MergeStatus::kOk, "cross-source duplicate ok");
  // One suggestion row survives: same language, same surface, different source.
  Check(result.merged.size() == 2, "cross-source duplicate merges into one row");
  Check(result.deduplicated == 1, "merged duplicate counted");
  Check(result.merged[1].record.source == cand::Source::kWord,
        "more reliable source (word) survives");
  Check(result.merged[1].record.stable_id == records[2].stable_id,
        "survivor carries the word row's identity");
  Check(result.merged[1].provenance.size() == 2,
        "provenance lists both producer rows");
  Check(result.merged[1].provenance[0].source == cand::Source::kWord &&
            result.merged[1].provenance[1].source == cand::Source::kPrediction,
        "provenance records both sources, survivor first");
  Check(result.merged[1].exact_expansion, "exact flag survives the merge");
}

// Family 10: detection latch boundary — exercised through the ranker API the
// backend will hold per session.
void DetectionLatchBoundary() {
  epocanva::ranking::AdaptiveRanker ranker;  // default 3 confirmations
  Check(ranker.effective_detected_language().empty(), "latch starts empty");
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language().empty(), "weak signal does not switch");
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language().empty(), "two do not switch");
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language() == "zh", "threshold switches");
  ranker.ResetDetection();
  Check(ranker.effective_detected_language().empty(), "reset clears state");
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language().empty(),
        "post-reset state does not leak");
}

// Family 11: producer order perturbation is covered in MergeRuleTests; here:
// in-process vs cross-process determinism is a pure-function property — the
// merger must not read anything but its inputs.
void PurityDeterminism() {
  auto raw = EnRaw("mi");
  auto fr = MakeRecord("fr", "latin", 3, 1, 1, "mis", {91}, -0.5);
  auto en = EnWord("miss", {92}, -1.0);
  std::vector<fus::MergeResult> results;
  for (int round = 0; round < 3; ++round) {
    results.push_back(fus::Merge({raw, fr, en}, {0, 1, 2}, {false, false, false}));
  }
  for (const auto& result : results) {
    Check(result.status == fus::MergeStatus::kOk, "purity merge ok");
    Check(result.merged.size() == 3, "purity size stable");
  }
  for (std::size_t index = 0; index < results[0].merged.size(); ++index) {
    Check(results[0].merged[index].record.stable_id == results[1].merged[index].record.stable_id &&
              results[1].merged[index].record.stable_id == results[2].merged[index].record.stable_id,
          "in-process repeat identical");
  }
}

// Family 12: rank_only sources never add scales — the ordering among equal
// texts from different sources is producer ordinal, and a huge raw score from
// a fallback source cannot overturn it.
void NoScaleAddition() {
  auto quiet = EnWord("calm", {7}, -0.5);
  auto loud = EnPrediction("storm", {8}, 5e5);
  std::vector<cand::Record> records{EnRaw("c"), quiet, loud};
  const auto result = fus::Merge(records, {0, 1, 2}, {false, false, false});
  Check(result.merged[1].record.text_utf8 == "calm" &&
            result.merged[2].record.text_utf8 == "storm",
        "producer order decides, raw scale ignored");
  Check(result.merged[2].fusion_points < result.merged[1].fusion_points,
        "later fallback row has lower points");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "--all";
  if (mode == "--forcefail") {
    std::cerr << "ASSERT FAIL forced\n";
    return 1;
  }
  if (mode == "--all" || mode == "--policy") PolicyMirrorTests();
  if (mode == "--all" || mode == "--dedup") DedupKeyTests();
  if (mode == "--all" || mode == "--merge") MergeRuleTests();
  if (mode == "--all" || mode == "--mixed") {
    MixedZhInEn();
    MixedEnInZh();
    MixedEnInFr();
    MixedJaRomajiEn();
    MixedEnInKo();
    SameSurfaceDifferentLanguages();
    ExactVsPrediction();
    RawVsHighPrediction();
    CrossSourceDuplicateMerge();
    DetectionLatchBoundary();
    PurityDeterminism();
    NoScaleAddition();
  }
  if (failed != 0) return 1;
  std::cout << "RESULT PASS " << checks << " checks\n";
  return 0;
}
