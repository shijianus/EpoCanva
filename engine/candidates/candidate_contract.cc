#include "candidate_contract.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace epocanva {
namespace candidates {
namespace {

constexpr std::size_t kMaxTextUtf8Bytes = 4096;

bool IsContinuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

// Strict scalar UTF-8: rejects overlong encodings, surrogates, truncated
// sequences, embedded NUL, C0 and C1 controls.
bool IsValidText(std::string_view text, std::size_t max_bytes) {
  if (text.empty() || text.size() > max_bytes) return false;
  for (std::size_t byte = 0; byte < text.size();) {
    const auto lead = static_cast<unsigned char>(text[byte++]);
    std::uint32_t scalar = 0;
    std::size_t remaining = 0;
    if (lead < 0x80) {
      scalar = lead;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
      scalar = lead & 0x1fu;
      remaining = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      scalar = lead & 0x0fu;
      remaining = 2;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      scalar = lead & 0x07u;
      remaining = 3;
    } else {
      return false;
    }
    if (remaining > text.size() - byte) return false;
    for (std::size_t index = 0; index < remaining; ++index) {
      const auto value = static_cast<unsigned char>(text[byte++]);
      if (!IsContinuation(value)) return false;
      scalar = (scalar << 6) | (value & 0x3fu);
    }
    if ((remaining == 1 && scalar < 0x80) ||
        (remaining == 2 && scalar < 0x800) ||
        (remaining == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) || scalar < 0x20 ||
        (scalar >= 0x7f && scalar <= 0x9f)) {
      return false;
    }
  }
  return true;
}

bool IsAsciiId(std::string_view text, std::size_t max_bytes) {
  if (text.empty() || text.size() > max_bytes) return false;
  if (text.front() < 'a' || text.front() > 'z') return false;
  for (const char byte : text) {
    const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                    byte == '_' || byte == '-';
    if (!ok) return false;
  }
  return true;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (const char byte : text) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= UINT64_C(1099511628211);
  }
  return hash == 0 ? 1 : hash;
}

std::string Hex64(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

Source SourceFromKind(std::uint32_t kind, bool* known) {
  *known = true;
  switch (kind) {
    case 0: return Source::kRaw;
    case 1: return Source::kWord;
    case 2: return Source::kSpelling;
    case 3: return Source::kMorphology;
    case 4: return Source::kExpansion;
    case 5: return Source::kConversion;
    case 6: return Source::kPrediction;
    default: *known = false; return Source::kRaw;
  }
}

Match MatchFromSource(Source source) {
  switch (source) {
    case Source::kRaw: return Match::kRaw;
    case Source::kWord:
    case Source::kMorphology: return Match::kPrefix;
    case Source::kSpelling: return Match::kFuzzy;
    case Source::kExpansion: return Match::kAbbreviation;
    case Source::kConversion: return Match::kConversion;
    case Source::kPrediction: return Match::kContext;
  }
  return Match::kRaw;
}

Commit CommitFromSource(Source source) {
  // A predicted next word is inserted at the caret; everything else replaces
  // the current input token. Raw is the original input committed unchanged.
  return source == Source::kPrediction ? Commit::kInsert : Commit::kReplaceInput;
}

}  // namespace

std::string NormalizeIdentity(std::string_view language_tag, std::string_view text_utf8) {
  if (!IsAsciiId(language_tag, kMaxLanguageTagBytes)) return {};
  if (!IsValidText(text_utf8, kMaxIdentityUtf8Bytes)) return {};
  std::string identity(text_utf8);
  if (language_tag == "en") {
    for (char& byte : identity) {
      if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    }
  }
  return identity;
}

std::string LearningCandidateId(const std::vector<std::uint64_t>& lexeme_ids) {
  if (lexeme_ids.empty() || lexeme_ids.size() > kMaxLexemeIds) return {};
  std::string key = "lex:";
  for (std::size_t ordinal = 0; ordinal < lexeme_ids.size(); ++ordinal) {
    if (lexeme_ids[ordinal] == 0) return {};
    if (ordinal != 0) key.push_back('+');
    key += std::to_string(lexeme_ids[ordinal]);
  }
  return key;
}

std::string StableId(const Identity& identity) {
  if (!IsAsciiId(identity.language_tag, kMaxLanguageTagBytes) ||
      !IsAsciiId(identity.scheme_id, kMaxSchemeIdBytes)) {
    return {};
  }
  std::string id = "cand:v1:" + identity.language_tag + ":" + identity.scheme_id + ":";
  const std::string lexical = LearningCandidateId(identity.lexeme_ids);
  if (!lexical.empty()) return id + lexical;
  if (!IsValidText(identity.normalized_identity, kMaxIdentityUtf8Bytes)) return {};
  const std::string scope = "epocanva.candidate.identity/v1/" + identity.language_tag +
                            "/" + identity.scheme_id + "/" + identity.normalized_identity;
  return id + "text:" + Hex64(Fnv1a64(scope));
}

bool Describe(std::string language_tag, std::string scheme_id,
              std::uint32_t language_value, std::uint32_t input_mode_value,
              std::uint32_t abi_kind, std::string_view text_utf8,
              std::vector<std::uint64_t> lexeme_ids, double score,
              Record* out) {
  if (out == nullptr) return false;
  *out = {};
  bool known = false;
  const Source source = SourceFromKind(abi_kind, &known);
  if (!known) return false;
  if (!IsAsciiId(language_tag, kMaxLanguageTagBytes) ||
      !IsAsciiId(scheme_id, kMaxSchemeIdBytes)) {
    return false;
  }
  if (!IsValidText(text_utf8, kMaxTextUtf8Bytes)) return false;
  const std::string normalized = NormalizeIdentity(language_tag, text_utf8);
  if (normalized.empty()) return false;
  if (lexeme_ids.size() > kMaxLexemeIds) return false;

  Record record;
  record.identity.language_tag = std::move(language_tag);
  record.identity.scheme_id = std::move(scheme_id);
  record.identity.language = language_value;
  record.identity.input_mode = input_mode_value;
  record.identity.lexeme_ids = std::move(lexeme_ids);
  record.identity.normalized_identity = normalized;
  record.text_utf8.assign(text_utf8);
  record.source = source;
  record.match = MatchFromSource(source);
  record.commit = CommitFromSource(source);
  record.availability = Availability::kAvailable;
  record.score = score;
  record.source_confidence = 0.0;
  record.learnable = source != Source::kRaw && !record.identity.lexeme_ids.empty();
  record.stable_id = StableId(record.identity);
  std::string reason;
  if (record.stable_id.empty() || !Validate(record, &reason)) return false;
  *out = std::move(record);
  return true;
}

bool Validate(const Record& record, std::string* reason) {
  auto fail = [&](const char* why) {
    if (reason != nullptr) *reason = why;
    return false;
  };
  if (!IsAsciiId(record.identity.language_tag, kMaxLanguageTagBytes))
    return fail("invalid-language-tag");
  if (!IsAsciiId(record.identity.scheme_id, kMaxSchemeIdBytes))
    return fail("invalid-scheme-id");
  if (record.identity.language < kFirstLanguageValue ||
      record.identity.language > kLastLanguageValue)
    return fail("unknown-language-value");
  if (record.identity.input_mode < kFirstInputModeValue ||
      record.identity.input_mode > kLastInputModeValue)
    return fail("unknown-input-mode");
  if (!IsValidText(record.text_utf8, kMaxTextUtf8Bytes))
    return fail("invalid-text");
  if (!IsValidText(record.identity.normalized_identity, kMaxIdentityUtf8Bytes))
    return fail("invalid-normalized-identity");
  if (record.identity.lexeme_ids.size() > kMaxLexemeIds)
    return fail("too-many-lexeme-ids");
  for (const auto id : record.identity.lexeme_ids) {
    if (id == 0) return fail("zero-lexeme-id");
  }
  if (!std::isfinite(record.score)) return fail("non-finite-score");
  if (!std::isfinite(record.source_confidence) || record.source_confidence < 0.0 ||
      record.source_confidence > 1.0) {
    return fail("invalid-source-confidence");
  }
  if (record.source == Source::kRaw && !record.identity.lexeme_ids.empty())
    return fail("raw-candidate-cannot-learn");
  if (record.learnable != (record.source != Source::kRaw &&
                           !record.identity.lexeme_ids.empty()))
    return fail("learnable-flag-mismatch");
  if (record.input_begin_utf8_byte > record.input_end_utf8_byte)
    return fail("invalid-input-range");
  if (record.commit != CommitFromSource(record.source))
    return fail("commit-semantics-mismatch");
  if (record.match != MatchFromSource(record.source))
    return fail("match-relation-mismatch");
  const std::string expected = StableId(record.identity);
  if (expected.empty() || expected != record.stable_id)
    return fail("stable-id-mismatch");
  return true;
}

}  // namespace candidates
}  // namespace epocanva
