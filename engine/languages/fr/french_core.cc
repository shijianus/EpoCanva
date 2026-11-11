#include "french_core.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <utility>

namespace epocanva {
namespace fr {
namespace {

struct Scalar {
  char32_t value;
  std::size_t begin_utf8_byte;
  std::size_t end_utf8_byte;
};

Utf8Validation Decode(std::string_view text_utf8, std::vector<Scalar>* scalars) {
  if (text_utf8.size() > kMaxTextUtf8Bytes) {
    return {Status::kInputTooLong, kNoUtf8Byte};
  }
  for (std::size_t begin_utf8_byte = 0; begin_utf8_byte < text_utf8.size();) {
    const auto lead = static_cast<unsigned char>(text_utf8[begin_utf8_byte]);
    std::size_t sequence_utf8_bytes = 1;
    char32_t value = lead;
    char32_t minimum = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
      sequence_utf8_bytes = 2;
      value = lead & 0x1f;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      sequence_utf8_bytes = 3;
      value = lead & 0x0f;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      sequence_utf8_bytes = 4;
      value = lead & 0x07;
      minimum = 0x10000;
    } else if (lead > 0x7f) {
      return {Status::kInvalidUtf8, begin_utf8_byte};
    }
    if (sequence_utf8_bytes > text_utf8.size() - begin_utf8_byte) {
      return {Status::kInvalidUtf8, begin_utf8_byte};
    }
    for (std::size_t continuation_utf8_byte = 1;
         continuation_utf8_byte < sequence_utf8_bytes; ++continuation_utf8_byte) {
      const auto next = static_cast<unsigned char>(
          text_utf8[begin_utf8_byte + continuation_utf8_byte]);
      if ((next & 0xc0) != 0x80) {
        return {Status::kInvalidUtf8, begin_utf8_byte};
      }
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
      return {Status::kInvalidUtf8, begin_utf8_byte};
    }
    const std::size_t end_utf8_byte = begin_utf8_byte + sequence_utf8_bytes;
    if (scalars != nullptr) {
      scalars->push_back({value, begin_utf8_byte, end_utf8_byte});
    }
    begin_utf8_byte = end_utf8_byte;
  }
  return {};
}

void AppendScalar(char32_t value, std::string* output_utf8) {
  if (value <= 0x7f) {
    output_utf8->push_back(static_cast<char>(value));
  } else if (value <= 0x7ff) {
    output_utf8->push_back(static_cast<char>(0xc0 | (value >> 6)));
    output_utf8->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else if (value <= 0xffff) {
    output_utf8->push_back(static_cast<char>(0xe0 | (value >> 12)));
    output_utf8->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    output_utf8->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else {
    output_utf8->push_back(static_cast<char>(0xf0 | (value >> 18)));
    output_utf8->push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
    output_utf8->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    output_utf8->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  }
}

struct AccentRule {
  DeadAccent accent;
  char32_t base_lower;
  char32_t accented_lower;
  char32_t accented_upper;
};
// Linguistic character mappings, not dictionary candidates.
constexpr std::array<AccentRule, 16> kAccentRules{{
    {DeadAccent::kAcute, U'e', U'\u00e9', U'\u00c9'},
    {DeadAccent::kGrave, U'a', U'\u00e0', U'\u00c0'},
    {DeadAccent::kGrave, U'e', U'\u00e8', U'\u00c8'},
    {DeadAccent::kGrave, U'u', U'\u00f9', U'\u00d9'},
    {DeadAccent::kCircumflex, U'a', U'\u00e2', U'\u00c2'},
    {DeadAccent::kCircumflex, U'e', U'\u00ea', U'\u00ca'},
    {DeadAccent::kCircumflex, U'i', U'\u00ee', U'\u00ce'},
    {DeadAccent::kCircumflex, U'o', U'\u00f4', U'\u00d4'},
    {DeadAccent::kCircumflex, U'u', U'\u00fb', U'\u00db'},
    {DeadAccent::kDiaeresis, U'a', U'\u00e4', U'\u00c4'},
    {DeadAccent::kDiaeresis, U'e', U'\u00eb', U'\u00cb'},
    {DeadAccent::kDiaeresis, U'i', U'\u00ef', U'\u00cf'},
    {DeadAccent::kDiaeresis, U'o', U'\u00f6', U'\u00d6'},
    {DeadAccent::kDiaeresis, U'u', U'\u00fc', U'\u00dc'},
    {DeadAccent::kDiaeresis, U'y', U'\u00ff', U'\u0178'},
    {DeadAccent::kCedilla, U'c', U'\u00e7', U'\u00c7'},
}};

DeadAccent AccentForMark(char32_t value) {
  switch (value) {
    case U'\u0301': return DeadAccent::kAcute;
    case U'\u0300': return DeadAccent::kGrave;
    case U'\u0302': return DeadAccent::kCircumflex;
    case U'\u0308': return DeadAccent::kDiaeresis;
    case U'\u0327': return DeadAccent::kCedilla;
    default: return DeadAccent::kNone;
  }
}

char32_t LowerScalar(char32_t value) {
  if (value >= U'A' && value <= U'Z') return value + (U'a' - U'A');
  for (const auto& rule : kAccentRules) {
    if (value == rule.accented_upper) return rule.accented_lower;
  }
  if (value == U'\u0152') return U'\u0153';
  if (value == U'\u00c6') return U'\u00e6';
  return value;
}

char32_t UpperScalar(char32_t value) {
  if (value >= U'a' && value <= U'z') return value - (U'a' - U'A');
  for (const auto& rule : kAccentRules) {
    if (value == rule.accented_lower) return rule.accented_upper;
  }
  if (value == U'\u0153') return U'\u0152';
  if (value == U'\u00e6') return U'\u00c6';
  return value;
}

char32_t UnaccentedLower(char32_t value) {
  value = LowerScalar(value);
  for (const auto& rule : kAccentRules) {
    if (value == rule.accented_lower) return rule.base_lower;
  }
  return value;
}

bool IsLetter(char32_t value) { return LowerScalar(value) != UpperScalar(value); }
bool IsCombining(char32_t value) { return value >= 0x0300 && value <= 0x036f; }
bool IsJoiner(char32_t value) {
  return value == U'\'' || value == U'\u2019' || value == U'-' ||
         value == U'\u2010' || value == U'\u2011';
}
bool IsWhitespace(char32_t value) {
  return value == U' ' || (value >= U'\t' && value <= U'\r') ||
         value == U'\u00a0' || value == U'\u202f';
}
bool IsAsciiLetter(char32_t value) {
  return (value >= U'a' && value <= U'z') || (value >= U'A' && value <= U'Z');
}
bool IsDigit(char32_t value) { return value >= U'0' && value <= U'9'; }

bool IsWord(const std::vector<Scalar>& scalars) {
  if (scalars.empty() || !IsLetter(scalars.front().value) ||
      !IsLetter(scalars.back().value)) return false;
  bool previous_letter = false;
  for (const auto& scalar : scalars) {
    if (IsLetter(scalar.value)) {
      previous_letter = true;
    } else if (IsJoiner(scalar.value) && previous_letter) {
      previous_letter = false;
    } else {
      return false;
    }
  }
  return previous_letter;
}

bool IsProtected(TokenKind kind) {
  return kind == TokenKind::kUrl || kind == TokenKind::kEmail ||
         kind == TokenKind::kNumber;
}

bool StartsWithAsciiIgnoreCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t utf8_byte = 0; utf8_byte < prefix.size(); ++utf8_byte) {
    if (LowerScalar(static_cast<unsigned char>(text[utf8_byte])) !=
        static_cast<unsigned char>(prefix[utf8_byte])) return false;
  }
  return true;
}

bool LooksLikeUrl(std::string_view text) {
  if (StartsWithAsciiIgnoreCase(text, "www.") ||
      StartsWithAsciiIgnoreCase(text, "mailto:")) return true;
  const auto scheme_end_utf8_byte = text.find("://");
  if (scheme_end_utf8_byte == std::string_view::npos || scheme_end_utf8_byte == 0 ||
      !IsAsciiLetter(static_cast<unsigned char>(text.front()))) return false;
  for (std::size_t utf8_byte = 1; utf8_byte < scheme_end_utf8_byte; ++utf8_byte) {
    const auto value = static_cast<unsigned char>(text[utf8_byte]);
    if (!IsAsciiLetter(value) && !IsDigit(value) && value != '+' &&
        value != '-' && value != '.') return false;
  }
  return true;
}

bool LooksLikeEmail(std::string_view text) {
  const auto at_utf8_byte = text.find('@');
  // Protect in-progress domains too; this does not certify deliverability.
  return at_utf8_byte != std::string_view::npos && at_utf8_byte > 0 &&
         at_utf8_byte + 1 < text.size() &&
         text.find('@', at_utf8_byte + 1) == std::string_view::npos;
}

bool LooksLikeNumber(const std::vector<Scalar>& scalars) {
  // Protection grammar: optional sign; digits with dot/comma, NBSP or NNBSP
  // between digits; optional scientific exponent with sign; optional % suffix.
  // Group widths/decimal semantics deliberately not inferred.
  enum class NumberState { kStart, kSign, kDigits, kSeparator, kExponent,
                           kExponentSign, kExponentDigits, kPercent };
  NumberState state = NumberState::kStart;
  for (const auto& scalar : scalars) {
    const auto value = scalar.value;
    const bool digit = IsDigit(value);
    switch (state) {
      case NumberState::kStart:
        if (value == U'+' || value == U'-' || value == U'\u2212') {
          state = NumberState::kSign;
        } else if (digit) {
          state = NumberState::kDigits;
        } else return false;
        break;
      case NumberState::kSign:
      case NumberState::kSeparator:
        if (!digit) return false;
        state = NumberState::kDigits;
        break;
      case NumberState::kDigits:
        if (digit) break;
        if (value == U'.' || value == U',' || value == U'\u00a0' ||
            value == U'\u202f') state = NumberState::kSeparator;
        else if (value == U'e' || value == U'E') state = NumberState::kExponent;
        else if (value == U'%') state = NumberState::kPercent;
        else return false;
        break;
      case NumberState::kExponent:
        if (value == U'+' || value == U'-') state = NumberState::kExponentSign;
        else if (digit) state = NumberState::kExponentDigits;
        else return false;
        break;
      case NumberState::kExponentSign:
        if (!digit) return false;
        state = NumberState::kExponentDigits;
        break;
      case NumberState::kExponentDigits:
        if (digit) break;
        if (value != U'%') return false;
        state = NumberState::kPercent;
        break;
      case NumberState::kPercent:
        return false;
    }
  }
  return state == NumberState::kDigits || state == NumberState::kExponentDigits ||
         state == NumberState::kPercent;
}

bool IsEdgePunctuation(char32_t value) {
  switch (value) {
    case U'.': case U',': case U';': case U':': case U'!': case U'?':
    case U'(': case U')': case U'[': case U']': case U'{': case U'}':
    case U'"': return true;
    default: return false;
  }
}

TextResult ChangeCase(std::string_view text_utf8, bool upper) {
  const auto normalized = NormalizeFrench(text_utf8);
  if (normalized.status != Status::kOk) return normalized;
  std::vector<Scalar> scalars;
  Decode(normalized.text_utf8, &scalars);
  TextResult result;
  for (const auto& scalar : scalars) {
    AppendScalar(upper ? UpperScalar(scalar.value) : LowerScalar(scalar.value),
                 &result.text_utf8);
  }
  if (result.text_utf8.size() > kMaxTextUtf8Bytes) return {Status::kInputTooLong, {}};
  return result;
}

enum class CaseStyle { kLower, kTitle, kUpper, kMixed };
CaseStyle WordCase(const std::vector<Scalar>& scalars) {
  bool any_upper = false;
  bool any_lower = false;
  bool first_letter = true;
  bool title = true;
  for (const auto& scalar : scalars) {
    if (!IsLetter(scalar.value)) continue;
    const bool upper = scalar.value == UpperScalar(scalar.value);
    any_upper = any_upper || upper;
    any_lower = any_lower || !upper;
    if (upper != first_letter) title = false;
    first_letter = false;
  }
  if (!any_upper) return CaseStyle::kLower;
  if (!any_lower) return CaseStyle::kUpper;
  return title ? CaseStyle::kTitle : CaseStyle::kMixed;
}

std::string ApplyCase(const std::vector<Scalar>& scalars, CaseStyle style) {
  std::string output;
  bool first_letter = true;
  for (const auto& scalar : scalars) {
    const bool upper = style == CaseStyle::kUpper ||
                       (style == CaseStyle::kTitle && first_letter);
    AppendScalar(upper ? UpperScalar(scalar.value) : LowerScalar(scalar.value),
                 &output);
    if (IsLetter(scalar.value)) first_letter = false;
  }
  return output;
}

bool IsCaretBoundary(const ClusterSegmentation& segmentation,
                     std::size_t caret_utf8_byte) {
  if (caret_utf8_byte == 0) return true;
  return std::any_of(segmentation.clusters.begin(), segmentation.clusters.end(),
                     [caret_utf8_byte](const Utf8ByteSpan& span) {
                       return span.end_utf8_byte == caret_utf8_byte;
                     });
}

bool HasSuffix(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

bool ValidSourceField(std::string_view field) {
  if (field.empty() || field.size() > kMaxWordUtf8Bytes ||
      ValidateUtf8(field).status != Status::kOk) return false;
  bool nonblank = false;
  for (const auto byte : field) {
    const auto value = static_cast<unsigned char>(byte);
    if (value < 0x20 || value == 0x7f) return false;
    nonblank = nonblank || value != 0x20;
  }
  return nonblank;
}

}  // namespace

Utf8Validation ValidateUtf8(std::string_view text_utf8) {
  return Decode(text_utf8, nullptr);
}

std::optional<char32_t> ComposeLetter(DeadAccent accent, char32_t letter) {
  for (const auto& rule : kAccentRules) {
    if (accent != rule.accent) continue;
    if (letter == rule.base_lower) return rule.accented_lower;
    if (letter == rule.base_lower - (U'a' - U'A')) return rule.accented_upper;
  }
  return std::nullopt;
}

std::optional<char32_t> SpacingAccent(DeadAccent accent) {
  switch (accent) {
    case DeadAccent::kAcute: return U'\u00b4';
    case DeadAccent::kGrave: return U'`';
    case DeadAccent::kCircumflex: return U'^';
    case DeadAccent::kDiaeresis: return U'\u00a8';
    case DeadAccent::kCedilla: return U'\u00b8';
    default: return std::nullopt;
  }
}

TextResult NormalizeFrench(std::string_view text_utf8) {
  std::vector<Scalar> scalars;
  const auto validation = Decode(text_utf8, &scalars);
  if (validation.status != Status::kOk) return {validation.status, {}};
  TextResult result;
  // Hold the previous scalar instead of indexing decoded Unicode scalars.
  std::optional<char32_t> previous;
  for (const auto& scalar : scalars) {
    const auto composed = previous ? ComposeLetter(AccentForMark(scalar.value),
                                                  *previous) : std::nullopt;
    if (composed) {
      previous = *composed;
    } else {
      if (previous) AppendScalar(*previous, &result.text_utf8);
      previous = scalar.value;
    }
  }
  if (previous) AppendScalar(*previous, &result.text_utf8);
  return result;
}

TextResult ToLowerFrench(std::string_view text_utf8) { return ChangeCase(text_utf8, false); }
TextResult ToUpperFrench(std::string_view text_utf8) { return ChangeCase(text_utf8, true); }

ClusterSegmentation SegmentCombiningClusters(std::string_view text_utf8) {
  std::vector<Scalar> scalars;
  ClusterSegmentation result;
  result.status = Decode(text_utf8, &scalars).status;
  if (result.status != Status::kOk) return result;
  for (const auto& scalar : scalars) {
    if (IsCombining(scalar.value) && !result.clusters.empty()) {
      result.clusters.back().end_utf8_byte = scalar.end_utf8_byte;
    } else {
      result.clusters.push_back({scalar.begin_utf8_byte, scalar.end_utf8_byte});
    }
  }
  return result;
}

TokenAnalysis ClassifyToken(std::string_view token_utf8) {
  const auto normalized = NormalizeFrench(token_utf8);
  if (normalized.status != Status::kOk) return {normalized.status, TokenKind::kOther};
  if (normalized.text_utf8.empty()) return {Status::kOk, TokenKind::kOther};
  std::vector<Scalar> scalars;
  Decode(normalized.text_utf8, &scalars);
  if (std::all_of(scalars.begin(), scalars.end(), [](const Scalar& scalar) {
        return IsWhitespace(scalar.value);
      })) return {Status::kOk, TokenKind::kWhitespace};
  if (LooksLikeNumber(scalars)) return {Status::kOk, TokenKind::kNumber};
  const bool has_space_or_control = std::any_of(
      scalars.begin(), scalars.end(), [](const Scalar& scalar) {
        return IsWhitespace(scalar.value) || scalar.value < 0x20 || scalar.value == 0x7f;
      });
  if (!has_space_or_control) {
    if (LooksLikeUrl(normalized.text_utf8)) return {Status::kOk, TokenKind::kUrl};
    if (LooksLikeEmail(normalized.text_utf8)) return {Status::kOk, TokenKind::kEmail};
  }
  return {Status::kOk, IsWord(scalars) ? TokenKind::kFrenchWord : TokenKind::kOther};
}

TokenSegmentation SegmentTokens(std::string_view text_utf8) {
  TokenSegmentation result;
  std::vector<Scalar> scalars;
  result.status = Decode(text_utf8, &scalars).status;
  if (result.status != Status::kOk) return result;
  auto current = scalars.begin();
  while (current != scalars.end()) {
    const bool whitespace = IsWhitespace(current->value);
    auto end = current;
    while (end != scalars.end() && IsWhitespace(end->value) == whitespace) ++end;
    const auto begin_utf8_byte = current->begin_utf8_byte;
    const auto end_utf8_byte = std::prev(end)->end_utf8_byte;
    const auto chunk = text_utf8.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte);
    const auto kind = ClassifyToken(chunk).kind;
    if (whitespace || IsProtected(kind)) {
      result.tokens.push_back({{begin_utf8_byte, end_utf8_byte}, kind});
    } else {
      auto word_begin = current;
      auto word_end = end;
      while (word_begin != word_end && IsEdgePunctuation(word_begin->value)) ++word_begin;
      while (word_end != word_begin && IsEdgePunctuation(std::prev(word_end)->value)) --word_end;
      const auto core_begin_utf8_byte = word_begin == end ? end_utf8_byte : word_begin->begin_utf8_byte;
      const auto core_end_utf8_byte = word_end == word_begin ? core_begin_utf8_byte :
                                     std::prev(word_end)->end_utf8_byte;
      if (begin_utf8_byte < core_begin_utf8_byte) {
        result.tokens.push_back({{begin_utf8_byte, core_begin_utf8_byte}, TokenKind::kOther});
      }
      if (core_begin_utf8_byte < core_end_utf8_byte) {
        const auto core = text_utf8.substr(core_begin_utf8_byte,
                                          core_end_utf8_byte - core_begin_utf8_byte);
        result.tokens.push_back({{core_begin_utf8_byte, core_end_utf8_byte}, ClassifyToken(core).kind});
      }
      if (core_end_utf8_byte < end_utf8_byte) {
        result.tokens.push_back({{core_end_utf8_byte, end_utf8_byte}, TokenKind::kOther});
      }
    }
    current = end;
  }
  return result;
}

Status Composition::SetText(std::string_view text_utf8, std::size_t caret_utf8_byte) {
  const auto segmentation = SegmentCombiningClusters(text_utf8);
  if (segmentation.status != Status::kOk) return segmentation.status;
  if (!IsCaretBoundary(segmentation, caret_utf8_byte)) return Status::kInvalidCaretUtf8Byte;
  // Own the copy before changing state, including when text_utf8 aliases our buffer.
  std::string replacement(text_utf8);
  text_utf8_ = std::move(replacement);
  caret_utf8_byte_ = caret_utf8_byte;
  pending_accent_ = DeadAccent::kNone;
  return Status::kOk;
}

Status Composition::SetCaret(std::size_t caret_utf8_byte) {
  if (!IsCaretBoundary(SegmentCombiningClusters(text_utf8_), caret_utf8_byte)) {
    return Status::kInvalidCaretUtf8Byte;
  }
  caret_utf8_byte_ = caret_utf8_byte;
  return Status::kOk;
}

Status Composition::InsertResolvedUtf8(std::string_view text_utf8) {
  if (text_utf8.size() > kMaxTextUtf8Bytes - text_utf8_.size()) return Status::kInputTooLong;
  std::string combined = text_utf8_.substr(0, caret_utf8_byte_);
  combined.append(text_utf8);
  const std::size_t insertion_end_utf8_byte = combined.size();
  combined.append(text_utf8_, caret_utf8_byte_, std::string::npos);
  const auto clusters = SegmentCombiningClusters(combined);
  if (clusters.status != Status::kOk) return clusters.status;
  std::size_t cluster_end_utf8_byte = insertion_end_utf8_byte;
  for (const auto& cluster : clusters.clusters) {
    if (cluster.begin_utf8_byte < insertion_end_utf8_byte &&
        insertion_end_utf8_byte <= cluster.end_utf8_byte) {
      cluster_end_utf8_byte = cluster.end_utf8_byte;
      break;
    }
  }
  const auto prefix = NormalizeFrench(std::string_view(combined).substr(0, cluster_end_utf8_byte));
  auto normalized = NormalizeFrench(combined);
  if (normalized.status != Status::kOk) return normalized.status;
  text_utf8_ = std::move(normalized.text_utf8);
  caret_utf8_byte_ = prefix.text_utf8.size();
  return Status::kOk;
}

Status Composition::InsertUtf8(std::string_view text_utf8) {
  std::vector<Scalar> scalars;
  const auto status = Decode(text_utf8, &scalars).status;
  if (status != Status::kOk) return status;
  if (scalars.empty()) return Status::kOk;
  std::string resolved;
  if (pending_accent_ == DeadAccent::kNone) {
    resolved.assign(text_utf8);
  } else {
    const auto first = scalars.front();
    const auto composed = ComposeLetter(pending_accent_, first.value);
    if (composed) {
      AppendScalar(*composed, &resolved);
      resolved.append(text_utf8.substr(first.end_utf8_byte));
    } else {
      AppendScalar(*SpacingAccent(pending_accent_), &resolved);
      resolved.append(first.value == U' ' ? text_utf8.substr(first.end_utf8_byte) : text_utf8);
    }
  }
  const auto inserted = InsertResolvedUtf8(resolved);
  if (inserted == Status::kOk) pending_accent_ = DeadAccent::kNone;
  return inserted;
}

Status Composition::BeginDeadAccent(DeadAccent accent) {
  if (!SpacingAccent(accent)) return Status::kInvalidDeadAccent;
  if (pending_accent_ != DeadAccent::kNone) {
    std::string spacing;
    AppendScalar(*SpacingAccent(pending_accent_), &spacing);
    const auto inserted = InsertResolvedUtf8(spacing);
    if (inserted != Status::kOk) return inserted;
    pending_accent_ = pending_accent_ == accent ? DeadAccent::kNone : accent;
  } else {
    pending_accent_ = accent;
  }
  return Status::kOk;
}

Status Composition::Backspace() {
  if (pending_accent_ != DeadAccent::kNone) {
    pending_accent_ = DeadAccent::kNone;
    return Status::kOk;
  }
  if (caret_utf8_byte_ == 0) return Status::kOk;
  const auto segmentation = SegmentCombiningClusters(text_utf8_);
  for (const auto& cluster : segmentation.clusters) {
    if (cluster.end_utf8_byte == caret_utf8_byte_) {
      text_utf8_.erase(cluster.begin_utf8_byte, cluster.end_utf8_byte - cluster.begin_utf8_byte);
      caret_utf8_byte_ = cluster.begin_utf8_byte;
      return Status::kOk;
    }
  }
  return Status::kInvalidCaretUtf8Byte;
}

void Composition::Cancel() {
  text_utf8_.clear();
  caret_utf8_byte_ = 0;
  pending_accent_ = DeadAccent::kNone;
}

AccentSuggestionResult SuggestAccents(std::string_view query_utf8,
                                     const DictionaryInput& dictionary,
                                     std::size_t max_suggestions) {
  AccentSuggestionResult result;
  result.source_status = dictionary.source.status;
  const auto reject = [&result](Status status) {
    result.status = status;
    result.suggestions.clear();
    return result;
  };
  const auto query = NormalizeFrench(query_utf8);
  if (query.status != Status::kOk) return reject(query.status);
  if (query_utf8.size() > kMaxWordUtf8Bytes) return reject(Status::kInputTooLong);
  if (query.text_utf8.empty()) return reject(Status::kEmptyInput);
  const auto kind = ClassifyToken(query.text_utf8).kind;
  if (IsProtected(kind)) return reject(Status::kProtectedToken);
  if (kind != TokenKind::kFrenchWord) return reject(Status::kNotFrenchWord);
  std::vector<Scalar> query_scalars;
  Decode(query.text_utf8, &query_scalars);
  const auto style = WordCase(query_scalars);
  if (style == CaseStyle::kMixed) return reject(Status::kUnsupportedCase);
  if (max_suggestions > kMaxAccentSuggestions) return reject(Status::kInvalidLimit);
  const auto& source = dictionary.source;
  if (source.status == DictionarySourceStatus::kUnavailable) {
    return reject(dictionary.entries.empty() && source.name.empty() &&
                          source.version.empty() && source.licence.empty()
                      ? Status::kDictionaryUnavailable : Status::kInvalidDictionary);
  }
  if ((source.status != DictionarySourceStatus::kTestFixture &&
       source.status != DictionarySourceStatus::kCallerSuppliedUnverified) ||
      !ValidSourceField(source.name) || !ValidSourceField(source.version) ||
      !ValidSourceField(source.licence) || dictionary.entries.empty() ||
      dictionary.entries.size() > kMaxDictionaryEntries) {
    return reject(Status::kInvalidDictionary);
  }
  std::map<std::string, AccentSuggestion> unique;
  std::size_t dictionary_utf8_bytes = 0;
  for (const auto& entry : dictionary.entries) {
    if (entry.word_utf8.size() > kMaxWordUtf8Bytes ||
        entry.word_utf8.size() > kMaxDictionaryUtf8Bytes - dictionary_utf8_bytes) {
      return reject(Status::kInvalidDictionary);
    }
    dictionary_utf8_bytes += entry.word_utf8.size();
    const auto normalized = NormalizeFrench(entry.word_utf8);
    if (normalized.status != Status::kOk ||
        ClassifyToken(normalized.text_utf8).kind != TokenKind::kFrenchWord) {
      return reject(Status::kInvalidDictionary);
    }
    std::vector<Scalar> candidate_scalars;
    Decode(normalized.text_utf8, &candidate_scalars);
    if (max_suggestions == 0) continue;
    auto query_scalar = query_scalars.begin();
    auto candidate_scalar = candidate_scalars.begin();
    std::size_t changed_grapheme_clusters = 0;
    for (; query_scalar != query_scalars.end() && candidate_scalar != candidate_scalars.end();
         ++query_scalar, ++candidate_scalar) {
      const auto original = LowerScalar(query_scalar->value);
      const auto proposed = LowerScalar(candidate_scalar->value);
      if (original == proposed) continue;
      if (original < U'a' || original > U'z' || UnaccentedLower(proposed) != original) break;
      ++changed_grapheme_clusters;
    }
    if (query_scalar != query_scalars.end() || candidate_scalar != candidate_scalars.end() ||
        changed_grapheme_clusters == 0) continue;
    const auto output = ApplyCase(candidate_scalars, style);
    if (output.size() > kMaxWordUtf8Bytes) continue;
    const auto existing = unique.find(output);
    if (existing == unique.end() || existing->second.rank < entry.rank) {
      unique[output] = {output, entry.rank, changed_grapheme_clusters};
    }
  }
  for (auto& entry : unique) result.suggestions.push_back(std::move(entry.second));
  std::sort(result.suggestions.begin(), result.suggestions.end(),
            [](const AccentSuggestion& left, const AccentSuggestion& right) {
              if (left.rank != right.rank) return left.rank > right.rank;
              if (left.changed_grapheme_clusters != right.changed_grapheme_clusters) {
                return left.changed_grapheme_clusters < right.changed_grapheme_clusters;
              }
              return left.text_utf8 < right.text_utf8;
            });
  if (result.suggestions.size() > max_suggestions) result.suggestions.resize(max_suggestions);
  return result;
}

TextResult InflectPlural(std::string_view lemma_utf8, PluralRule rule) {
  const auto normalized = NormalizeFrench(lemma_utf8);
  if (normalized.status != Status::kOk) return normalized;
  if (lemma_utf8.size() > kMaxWordUtf8Bytes) return {Status::kInputTooLong, {}};
  if (normalized.text_utf8.empty()) return {Status::kEmptyInput, {}};
  const auto kind = ClassifyToken(normalized.text_utf8).kind;
  if (IsProtected(kind)) return {Status::kProtectedToken, {}};
  if (kind != TokenKind::kFrenchWord) return {Status::kNotFrenchWord, {}};
  std::vector<Scalar> scalars;
  Decode(normalized.text_utf8, &scalars);
  if (std::any_of(scalars.begin(), scalars.end(), [](const Scalar& scalar) {
        return IsJoiner(scalar.value);
      })) return {Status::kUnsupportedInflection, {}};
  const auto style = WordCase(scalars);
  if (style == CaseStyle::kMixed) return {Status::kUnsupportedCase, {}};
  std::string output = ApplyCase(scalars, CaseStyle::kLower);
  switch (rule) {
    case PluralRule::kRegularS:
      if (!HasSuffix(output, "s") && !HasSuffix(output, "x") && !HasSuffix(output, "z")) output += 's';
      break;
    case PluralRule::kEauX:
      if (!HasSuffix(output, "eau")) return {Status::kRuleNotApplicable, {}};
      output += 'x';
      break;
    case PluralRule::kAuEuX:
      if (!HasSuffix(output, "au") && !HasSuffix(output, "eu")) return {Status::kRuleNotApplicable, {}};
      output += 'x';
      break;
    case PluralRule::kOuX:
      if (!HasSuffix(output, "ou")) return {Status::kRuleNotApplicable, {}};
      output += 'x';
      break;
    case PluralRule::kAlAux:
      if (!HasSuffix(output, "al")) return {Status::kRuleNotApplicable, {}};
      output.replace(output.size() - 2, 2, "aux");  // suffix sizes in UTF-8 bytes
      break;
    case PluralRule::kAilAux:
      if (!HasSuffix(output, "ail")) return {Status::kRuleNotApplicable, {}};
      output.replace(output.size() - 3, 3, "aux");  // suffix sizes in UTF-8 bytes
      break;
    case PluralRule::kInvariant:
      break;
    default:
      return {Status::kUnsupportedInflection, {}};
  }
  std::vector<Scalar> inflected;
  Decode(output, &inflected);
  output = ApplyCase(inflected, style);
  if (output.size() > kMaxWordUtf8Bytes) return {Status::kInputTooLong, {}};
  return {Status::kOk, std::move(output)};
}

}  // namespace fr
}  // namespace epocanva
