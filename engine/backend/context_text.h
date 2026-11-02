#ifndef EPOCANVA_BACKEND_CONTEXT_TEXT_H_
#define EPOCANVA_BACKEND_CONTEXT_TEXT_H_
#include <windows.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include "engine/prediction/model.h"

namespace epocanva::backend {
// Transient context segmentation mirrors the training grammar: letter/mark runs
// for Latin, whitespace words for Korean, longest catalog match for Chinese and
// Japanese. This is not Unicode grapheme segmentation. Unknown words break context.
inline prediction::Context ResolveContext(const prediction::Model& model,
                                           std::string_view language,
                                           std::string_view text_utf8,
                                           prediction::Context ids = {},
                                           std::vector<prediction::LexemeId>* tokens = nullptr,
                                           bool* all_known = nullptr) {
  if (tokens) tokens->clear();
  if (all_known) *all_known = true;
  const auto admit = [&](std::string_view token_utf8) {
    std::string normalized;
    const auto* row = prediction::NormalizeLexemeUtf8(token_utf8, &normalized)
        ? model.Find(language, normalized) : nullptr;
    if (!row) {
      ids.clear();
      if (all_known) *all_known = false;
    } else {
      if (tokens) tokens->push_back(row->id);
      ids.push_back(row->id);
      if (ids.size() > 2) ids.erase(ids.begin());
    }
  };
  const auto next = [&](std::size_t begin_utf8_byte) {
    auto end_utf8_byte = begin_utf8_byte + 1;
    while (end_utf8_byte < text_utf8.size() &&
        (static_cast<unsigned char>(text_utf8[end_utf8_byte]) & 0xc0) == 0x80) ++end_utf8_byte;
    return end_utf8_byte;
  };
  const auto scalar = [&](std::size_t begin_utf8_byte, std::size_t end_utf8_byte) {
    auto lead = static_cast<unsigned char>(text_utf8[begin_utf8_byte]);
    std::uint32_t value = lead;
    if (lead >= 0xc0) {
      value &= end_utf8_byte - begin_utf8_byte == 2 ? 31u :
               end_utf8_byte - begin_utf8_byte == 3 ? 15u : 7u;
      for (auto byte = begin_utf8_byte + 1; byte < end_utf8_byte; ++byte)
        value = (value << 6) | (static_cast<unsigned char>(text_utf8[byte]) & 63u);
    }
    return value;
  };
  const auto space = [](std::uint32_t value) {
    return value == 0x20 || (value >= 9 && value <= 13) || value == 0x85 ||
        value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a) ||
        value == 0x2028 || value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
  };
  const auto word = [&](std::uint32_t value) {
    if (value > 0xffff) return language == "ko";
    const wchar_t unit = static_cast<wchar_t>(value);
    WORD first = 0, third = 0;
    if (!::GetStringTypeW(CT_CTYPE1, &unit, 1, &first) ||
        !::GetStringTypeW(CT_CTYPE3, &unit, 1, &third)) return false;
    return (first & C1_ALPHA) || (third & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) ||
        (language == "ko" && (first & C1_DIGIT));
  };
  if (language == "zh" || language == "ja") {
    for (std::size_t begin_utf8_byte = 0; begin_utf8_byte < text_utf8.size();) {
      const auto first_end_utf8_byte = next(begin_utf8_byte);
      if (space(scalar(begin_utf8_byte, first_end_utf8_byte))) {
        begin_utf8_byte = first_end_utf8_byte;
        continue;
      }
      auto best_end_utf8_byte = first_end_utf8_byte;
      for (auto end_utf8_byte = first_end_utf8_byte;
           end_utf8_byte <= text_utf8.size() && end_utf8_byte - begin_utf8_byte <= prediction::kMaxLexemeUtf8Bytes;) {
        std::string normalized;
        if (prediction::NormalizeLexemeUtf8(text_utf8.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte), &normalized) &&
            model.Find(language, normalized)) best_end_utf8_byte = end_utf8_byte;
        if (end_utf8_byte == text_utf8.size()) break;
        const auto following_utf8_byte = next(end_utf8_byte);
        if (space(scalar(end_utf8_byte, following_utf8_byte))) break;
        end_utf8_byte = following_utf8_byte;
      }
      admit(text_utf8.substr(begin_utf8_byte, best_end_utf8_byte - begin_utf8_byte));
      begin_utf8_byte = best_end_utf8_byte;
    }
    return ids;
  }
  std::size_t word_begin_utf8_byte = 0;
  bool in_word = false;
  const auto flush = [&](std::size_t end_utf8_byte) {
    if (in_word) admit(text_utf8.substr(word_begin_utf8_byte, end_utf8_byte - word_begin_utf8_byte));
    in_word = false;
  };
  for (std::size_t begin_utf8_byte = 0; begin_utf8_byte < text_utf8.size();) {
    const auto end_utf8_byte = next(begin_utf8_byte);
    const auto value = scalar(begin_utf8_byte, end_utf8_byte);
    const bool connector = value == '\'' || value == 0x2019 || value == '-' || value == 0x2010 || value == 0x2011;
    const bool joins = connector && in_word && end_utf8_byte < text_utf8.size() &&
        word(scalar(end_utf8_byte, next(end_utf8_byte)));
    if (word(value) || joins) {
      if (!in_word) { word_begin_utf8_byte = begin_utf8_byte; in_word = true; }
    } else {
      flush(begin_utf8_byte);
      if (!space(value)) admit(text_utf8.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte));
      if (value == '\n' || value == '\r' || value == 0x2028 || value == 0x2029) ids.clear();
    }
    begin_utf8_byte = end_utf8_byte;
  }
  flush(text_utf8.size());
  return ids;
}
}
#endif
