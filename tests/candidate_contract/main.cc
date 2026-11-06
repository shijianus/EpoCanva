// Unified candidate contract tests.
//
// Every assertion can fail: --forcefail proves the harness reports red, and the
// stable-ID vectors are pinned so a hash/namespace change cannot pass silently.
#include "candidate_contract.h"
#include "epocanva_backend.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace cand = epocanva::candidates;

int failed = 0;

void Check(bool value, const char* label) {
  if (!value) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}

cand::Identity Identity(std::string tag, std::string scheme,
                        std::vector<std::uint64_t> ids = {},
                        std::string normalized = {}) {
  cand::Identity identity;
  identity.language_tag = std::move(tag);
  identity.scheme_id = std::move(scheme);
  identity.language = 1;
  identity.input_mode = 1;
  identity.lexeme_ids = std::move(ids);
  identity.normalized_identity = std::move(normalized);
  return identity;
}

void NormalizationTests() {
  Check(cand::NormalizeIdentity("en", "Good") == "good", "English ASCII case folds");
  Check(cand::NormalizeIdentity("en", "GOOD morning") == "good morning",
        "English multi-word case folds");
  // Other languages are preserved scalar for scalar. No Unicode casefold table
  // exists in this layer, and NFC/NFD equivalence is explicitly not claimed.
  Check(cand::NormalizeIdentity("fr", "\xc3\x89" "cole") == "\xc3\x89" "cole",
        "French precomposed text is preserved exactly");
  const std::string composed = "\xc3\xa9";          // U+00E9
  const std::string decomposed = "e\xcc\x81";       // e + U+0301
  Check(cand::NormalizeIdentity("fr", composed) != cand::NormalizeIdentity("fr", decomposed),
        "NFC and NFD are distinct, not silently equal");

  Check(cand::NormalizeIdentity("en", "").empty(), "empty text rejected");
  Check(cand::NormalizeIdentity("en", std::string("\0", 1)).empty(), "embedded NUL rejected");
  Check(cand::NormalizeIdentity("en", "\x01").empty(), "C0 control rejected");
  Check(cand::NormalizeIdentity("en", "\xc2\x85").empty(), "C1 control rejected");
  Check(cand::NormalizeIdentity("en", "\xc0\xaf").empty(), "overlong UTF-8 rejected");
  Check(cand::NormalizeIdentity("en", "\xed\xa0\x80").empty(), "surrogate rejected");
  Check(cand::NormalizeIdentity("en", "\xe2\x82").empty(), "truncated UTF-8 rejected");
  Check(cand::NormalizeIdentity("EN", "good").empty(), "non-canonical language tag rejected");
  Check(cand::NormalizeIdentity("", "good").empty(), "empty language tag rejected");
  Check(cand::NormalizeIdentity("en", std::string(4097, 'a')).empty(),
        "identity over the byte bound rejected");
}

void StableIdTests() {
  Check(cand::StableId(Identity("en", "latin", {42}, "good")) ==
            "cand:v1:en:latin:lex:42",
        "lexical stable id is canonical");
  Check(cand::StableId(Identity("en", "latin", {42, 7}, "good")) ==
            "cand:v1:en:latin:lex:42+7",
        "multi-lexeme order is preserved");
  // Pinned FNV-1a-64 vector for the no-lexeme fallback. If the namespace prefix
  // or hash changes, existing non-learning IDs change too and this fails.
  Check(cand::StableId(Identity("en", "latin", {}, "good")) ==
            "cand:v1:en:latin:text:6fa98b597d8a696a",
        "English text fallback id matches pinned vector");
  Check(cand::StableId(Identity("fr", "latin", {}, "hello")) ==
            "cand:v1:fr:latin:text:9c5f46fb4c1f662a",
        "French text fallback id matches pinned vector");

  // Same surface, same lexeme id, different language/scheme: no collision.
  const std::string english = cand::StableId(Identity("en", "latin", {42}, "good"));
  const std::string french = cand::StableId(Identity("fr", "latin", {42}, "good"));
  const std::string other_scheme = cand::StableId(Identity("en", "two_set", {42}, "good"));
  Check(english != french, "same text in two languages has different ids");
  Check(english != other_scheme, "same text under two schemes has different ids");
  Check(cand::StableId(Identity("en", "latin", {42}, "good")) == english,
        "id is stable across calls");
  Check(cand::StableId(Identity("en", "latin", {43}, "good")) != english,
        "different lexeme identity produces a different id");
  Check(cand::StableId(Identity("en", "latin", {}, "goods")) !=
            cand::StableId(Identity("en", "latin", {}, "good")),
        "different word forms are not merged");
  // The persistable key and the ephemeral text identity are different
  // namespaces; neither can be mistaken for the other.
  Check(cand::StableId(Identity("en", "latin", {}, "good")).find(":lex:") ==
            std::string::npos,
        "text fallback id never uses the lexeme namespace");
  Check(cand::StableId(Identity("en", "latin", {42}, "good")).find(":text:") ==
            std::string::npos,
        "lexeme id never uses the text namespace");
  Check(cand::LearningCandidateId({42}) !=
            cand::StableId(Identity("en", "latin", {42}, "good")),
        "persistable key is not the public stable id");
  Check(cand::StableId(Identity("en", "latin", {}, "good")) !=
            cand::StableId(Identity("fr", "latin", {}, "good")),
        "same non-learnable surface in two languages has different ids");

  // Learning compatibility: the schema-v3 key is unchanged and is not the
  // display-text hash.
  Check(cand::LearningCandidateId({42}) == "lex:42", "learning key is lex:id");
  Check(cand::LearningCandidateId({42, 7}) == "lex:42+7", "learning key preserves order");
  Check(cand::LearningCandidateId({}).empty(), "no lexeme ids means not learnable");
  Check(cand::LearningCandidateId({0}).empty(), "zero lexeme id is not learnable");
  Check(cand::LearningCandidateId(std::vector<std::uint64_t>(9, 1)).empty(),
        "more than eight ids is rejected");
}

void DescribeTests() {
  cand::Record word;
  Check(cand::Describe("en", "latin", 1, 1, 1, "Good", {42}, 1.5, &word),
        "English word candidate describes");
  Check(word.source == cand::Source::kWord, "word source mapped");
  Check(word.match == cand::Match::kPrefix, "word match is prefix");
  Check(word.commit == cand::Commit::kReplaceInput, "word replaces input");
  Check(word.availability == cand::Availability::kAvailable, "produced candidate is available");
  Check(word.source_confidence == 0.0, "confidence is not fabricated");
  Check(word.identity.normalized_identity == "good", "word identity normalizes case");
  Check(word.learnable, "word with admitted ids is learnable");
  Check(word.stable_id == "cand:v1:en:latin:lex:42", "word stable id");
  std::string reason;
  word.input_begin_utf8_byte = 0;
  word.input_end_utf8_byte = 4;
  Check(cand::Validate(word, &reason), "described word validates");

  cand::Record prediction;
  Check(cand::Describe("en", "latin", 1, 1, 6, "good", {42}, 0.5, &prediction),
        "prediction candidate describes");
  Check(prediction.commit == cand::Commit::kInsert, "prediction inserts at caret");
  Check(prediction.match == cand::Match::kContext, "prediction is a context match");
  Check(prediction.stable_id == word.stable_id,
        "same admitted lexeme has the same id across sources");

  cand::Record raw;
  Check(cand::Describe("zh", "pinyin", 2, 2, 0, "\xe4\xbd\xa0\xe5\xa5\xbd", {}, 0.0, &raw),
        "raw candidate describes");
  Check(raw.source == cand::Source::kRaw, "raw source mapped");
  Check(raw.commit == cand::Commit::kReplaceInput, "raw commits the original input");
  Check(raw.identity.lexeme_ids.empty() && cand::LearningCandidateId({}).empty(),
        "raw candidate is not learnable");
  Check(!raw.learnable, "raw candidate learnable flag is false");
  Check(raw.stable_id.rfind("cand:v1:zh:pinyin:text:", 0) == 0,
        "raw id uses the scoped text fallback");

  cand::Record invalid;
  Check(!cand::Describe("en", "latin", 1, 1, 0, "good", {42}, 1.0, &invalid),
        "raw candidate with lexeme ids rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 7, "good", {}, 1.0, &invalid),
        "unknown backend candidate kind rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 1, "", {}, 1.0, &invalid),
        "empty candidate text rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 1, "good", {0}, 1.0, &invalid),
        "zero lexeme id rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 1, "good",
                        std::vector<std::uint64_t>(9, 1), 1.0, &invalid),
        "nine lexeme ids rejected");

  cand::Record nan_score;
  Check(cand::Describe("en", "latin", 1, 1, 1, "good", {42},
                       std::nan(""), &nan_score) == false,
        "NaN score rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 1, "good", {42},
                        std::numeric_limits<double>::infinity(), &invalid),
        "positive infinite score rejected");
  Check(!cand::Describe("en", "latin", 1, 1, 1, "good", {42},
                        -std::numeric_limits<double>::infinity(), &invalid),
        "negative infinite score rejected");
  Check(!cand::Describe("EN", "latin", 1, 1, 1, "good", {42}, 1.0, &invalid),
        "invalid language tag rejected");
  Check(!cand::Describe("en", "Latin", 1, 1, 1, "good", {42}, 1.0, &invalid),
        "invalid scheme id rejected");
  Check(!cand::Describe("en", "latin", 0, 1, 1, "good", {42}, 1.0, &invalid),
        "zero language value rejected");
  Check(!cand::Describe("en", "latin", 6, 1, 1, "good", {42}, 1.0, &invalid),
        "language value outside the admitted ABI range rejected");
  Check(!cand::Describe("en", "latin", 1, 0, 1, "good", {42}, 1.0, &invalid),
        "zero input mode rejected");
  Check(!cand::Describe("en", "latin", 1, 8, 1, "good", {42}, 1.0, &invalid),
        "input mode outside the admitted ABI range rejected");

  // Tampering must be detected; these are the serialization/migration guards.
  auto tampered = word;
  tampered.stable_id = "cand:v1:en:latin:lex:43";
  Check(!cand::Validate(tampered, &reason) && reason == "stable-id-mismatch",
        "tampered stable id rejected");
  tampered = word;
  tampered.commit = cand::Commit::kInsert;
  Check(!cand::Validate(tampered, &reason) && reason == "commit-semantics-mismatch",
        "wrong commit semantics rejected");
  tampered = word;
  tampered.match = cand::Match::kContext;
  Check(!cand::Validate(tampered, &reason) && reason == "match-relation-mismatch",
        "wrong match relation rejected");
  tampered = word;
  tampered.identity.lexeme_ids.push_back(0);
  Check(!cand::Validate(tampered, &reason) && reason == "zero-lexeme-id",
        "zero id in an existing record rejected");
  tampered = word;
  tampered.source_confidence = 1.5;
  Check(!cand::Validate(tampered, &reason) && reason == "invalid-source-confidence",
        "out-of-range confidence rejected");
  for (const double bad : {std::nan(""), std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), -0.01}) {
    tampered = word;
    tampered.source_confidence = bad;
    Check(!cand::Validate(tampered, &reason) && reason == "invalid-source-confidence",
          "non-finite or negative confidence rejected");
  }
  tampered = word;
  tampered.source_confidence = 1.0;
  Check(cand::Validate(tampered, &reason), "confidence upper bound is inclusive");
  tampered = word;
  tampered.identity.language = 6;
  Check(!cand::Validate(tampered, &reason) && reason == "unknown-language-value",
        "unknown language value rejected by an existing record");
  tampered = word;
  tampered.identity.input_mode = 8;
  Check(!cand::Validate(tampered, &reason) && reason == "unknown-input-mode",
        "unknown input mode rejected by an existing record");
  tampered = word;
  tampered.learnable = false;
  Check(!cand::Validate(tampered, &reason) && reason == "learnable-flag-mismatch",
        "false learnable flag rejected");
  tampered = word;
  tampered.input_begin_utf8_byte = 5;
  tampered.input_end_utf8_byte = 4;
  Check(!cand::Validate(tampered, &reason) && reason == "invalid-input-range",
        "reversed input range rejected");
}

void AbiMirrorTests() {
  // The contract mirrors the public candidate-kind and language/mode enum
  // values. If epocanva_backend.h drifts, these checks fail even when the
  // contract's own numeric literals were changed to match a stale copy.
  Check(static_cast<std::uint32_t>(cand::Source::kRaw) == EPOCANVA_BACKEND_RAW,
        "raw kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kWord) == EPOCANVA_BACKEND_WORD,
        "word kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kSpelling) == EPOCANVA_BACKEND_SPELLING,
        "spelling kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kMorphology) == EPOCANVA_BACKEND_MORPHOLOGY,
        "morphology kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kExpansion) == EPOCANVA_BACKEND_EXPANSION,
        "expansion kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kConversion) == EPOCANVA_BACKEND_CONVERSION,
        "conversion kind matches the ABI");
  Check(static_cast<std::uint32_t>(cand::Source::kPrediction) == EPOCANVA_BACKEND_PREDICTION,
        "prediction kind matches the ABI");
  Check(cand::kFirstLanguageValue == EPOCANVA_BACKEND_EN, "first language value matches the ABI");
  Check(cand::kLastLanguageValue == EPOCANVA_BACKEND_KO, "last language value matches the ABI");
  Check(cand::kFirstInputModeValue == EPOCANVA_BACKEND_LATIN,
        "first input mode value matches the ABI");
  Check(cand::kLastInputModeValue == EPOCANVA_BACKEND_KANA,
        "last input mode value matches the ABI");
}

void Run() {
  AbiMirrorTests();
  NormalizationTests();
  StableIdTests();
  DescribeTests();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "--forcefail") {
    Check(false, "intentional falsifiability control");
  } else if (mode == "--unit") {
    Run();
  } else {
    return 2;
  }
  if (failed != 0) return 1;
  std::cout << "RESULT PASS\n";
  return 0;
}
