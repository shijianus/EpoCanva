// EpoCanva P2.1 -- English lexicon implementation.
//
// See english_lexicon.h for the line grammar, the status taxonomy and the unit
// convention. Everything here is locale-independent: comparisons use
// std::string's byte order and digits are tested by range, not by std::isdigit.
#include "english_lexicon.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace epocanva {
namespace en {
namespace {

constexpr char kTab = '\t';
constexpr char kLineFeed = '\n';
constexpr char kCarriageReturn = '\r';

bool IsLowercaseAsciiLetter(char c) {
  return c >= 'a' && c <= 'z';
}

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Returns the status for one weight field. `value` is only written on kOk.
LexiconStatus ParseWeight(std::string_view field, std::uint32_t* value) {
  if (field.empty()) {
    return LexiconStatus::kIllegalWeight;
  }
  // A leading zero would make "007" and "7" two spellings of one weight, which
  // is exactly the kind of ambiguity a data file should not be allowed to hide.
  if (field.size() > 1 && field.front() == '0') {
    return LexiconStatus::kIllegalWeight;
  }
  std::uint64_t accumulator = 0;
  for (const char c : field) {
    if (!IsAsciiDigit(c)) {
      return LexiconStatus::kIllegalWeight;
    }
    accumulator = accumulator * 10u + static_cast<std::uint64_t>(c - '0');
    if (accumulator > 0xFFFFFFFFull) {
      return LexiconStatus::kWeightOverflow;
    }
  }
  *value = static_cast<std::uint32_t>(accumulator);
  return LexiconStatus::kOk;
}

}  // namespace

const char* LexiconStatusName(LexiconStatus status) {
  switch (status) {
    case LexiconStatus::kOk:
      return "ok";
    case LexiconStatus::kFileNotFound:
      return "file-not-found";
    case LexiconStatus::kFileUnreadable:
      return "file-unreadable";
    case LexiconStatus::kEmptyFile:
      return "empty-file";
    case LexiconStatus::kBlankLine:
      return "blank-line";
    case LexiconStatus::kMissingField:
      return "missing-field";
    case LexiconStatus::kTooManyFields:
      return "too-many-fields";
    case LexiconStatus::kIllegalWord:
      return "illegal-word";
    case LexiconStatus::kWordTooLong:
      return "word-too-long";
    case LexiconStatus::kDuplicateWord:
      return "duplicate-word";
    case LexiconStatus::kIllegalWeight:
      return "illegal-weight";
    case LexiconStatus::kWeightOverflow:
      return "weight-overflow";
    case LexiconStatus::kIllegalWordForm:
      return "illegal-word-form";
  }
  return "unknown-status";
}

// Splits the two ways a word field can be wrong, because the fix is different and
// a test must be able to tell them apart: a byte the word alphabet never contained
// (an uppercase letter, a digit, a curly quote, any byte >= 0x80) versus bytes that
// are all in the alphabet but arranged in a shape no entry may have ("'hello",
// "hello'", "rock'n'roll", "co--operate", "don't-x").
// Precondition: `text` is non-empty.
bool WordBytesAreInAlphabet(std::string_view text) {
  for (const char c : text) {
    const ByteClass klass = ClassifyTokenByte(c);
    if (klass == ByteClass::kDelimiter) {
      continue;
    }
    if (klass != ByteClass::kLetter || !IsLowercaseAsciiLetter(c)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<EnglishLexicon> EnglishLexicon::FromText(const std::string& text,
                                                         LexiconLoadInfo* info) {
  LexiconLoadInfo local;
  if (info == nullptr) {
    info = &local;
  }
  info->status = LexiconStatus::kOk;
  info->line = 0;
  info->entry_count = 0;

  if (text.empty()) {
    info->status = LexiconStatus::kEmptyFile;
    return nullptr;
  }
  // ONE newline at end of file terminates the last record; it is not a blank
  // line. A lone CR at end of file is not a terminator, so it falls through to
  // the per-line checks and is rejected as part of a weight field.
  std::string_view body(text);
  if (body.back() == kLineFeed) {
    body.remove_suffix(1);
  }
  if (body.empty()) {
    info->status = LexiconStatus::kEmptyFile;
    return nullptr;
  }

  std::vector<LexiconEntry> parsed;
  std::vector<std::size_t> origin_line;  // parallel to parsed; used only for errors
  std::size_t line_number = 0;
  std::size_t offset = 0;
  while (offset <= body.size()) {
    ++line_number;
    std::size_t next = body.find(kLineFeed, offset);
    const bool last = (next == std::string_view::npos);
    std::string_view line =
        last ? body.substr(offset) : body.substr(offset, next - offset);
    if (!line.empty() && line.back() == kCarriageReturn) {
      line.remove_suffix(1);
    }

    if (line.empty()) {
      info->status = LexiconStatus::kBlankLine;
      info->line = line_number;
      return nullptr;
    }
    const std::size_t tab = line.find(kTab);
    if (tab == std::string_view::npos) {
      info->status = LexiconStatus::kMissingField;
      info->line = line_number;
      return nullptr;
    }
    if (line.find(kTab, tab + 1) != std::string_view::npos) {
      info->status = LexiconStatus::kTooManyFields;
      info->line = line_number;
      return nullptr;
    }
    const std::string_view word = line.substr(0, tab);
    const std::string_view weight_field = line.substr(tab + 1);
    if (word.empty()) {
      info->status = LexiconStatus::kIllegalWord;
      info->line = line_number;
      return nullptr;
    }
    if (word.size() > kMaxWordBytes) {
      info->status = LexiconStatus::kWordTooLong;
      info->line = line_number;
      return nullptr;
    }
    // The entry grammar is the QUERY grammar, called from english_token.h rather
    // than restated here: a word that loads is a word a prefix query can find, and
    // the two lists cannot drift apart. kWordTooLong was already answered above, and
    // kMaxWordBytes is kMaxTokenBytes, so the form check adds no second limit.
    if (!IsDictionaryEntryForm(word)) {
      info->status = WordBytesAreInAlphabet(word)
                         ? LexiconStatus::kIllegalWordForm
                         : LexiconStatus::kIllegalWord;
      info->line = line_number;
      return nullptr;
    }
    std::uint32_t weight = 0;
    const LexiconStatus weight_status = ParseWeight(weight_field, &weight);
    if (weight_status != LexiconStatus::kOk) {
      info->status = weight_status;
      info->line = line_number;
      return nullptr;
    }
    parsed.push_back(LexiconEntry{std::string(word), weight});
    origin_line.push_back(line_number);

    if (last) {
      break;
    }
    offset = next + 1;
  }

  // The index layout. Words are unique-checked after sorting so the scan is
  // O(n) and the ordering a prefix search relies on is established here once.
  std::vector<std::size_t> order(parsed.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&parsed](std::size_t a, std::size_t b) {
    return parsed[a].word < parsed[b].word;
  });
  for (std::size_t i = 1; i < order.size(); ++i) {
    if (parsed[order[i - 1]].word == parsed[order[i]].word) {
      info->status = LexiconStatus::kDuplicateWord;
      info->line = origin_line[order[i]];
      return nullptr;
    }
  }

  auto lexicon = std::unique_ptr<EnglishLexicon>(new EnglishLexicon());
  lexicon->entries_.reserve(parsed.size());
  for (const std::size_t i : order) {
    lexicon->entries_.push_back(std::move(parsed[i]));
  }
  info->status = LexiconStatus::kOk;
  info->line = 0;
  info->entry_count = lexicon->size();
  return lexicon;
}

std::unique_ptr<EnglishLexicon> EnglishLexicon::FromFile(
    const std::filesystem::path& path, LexiconLoadInfo* info) {
  LexiconLoadInfo local;
  if (info == nullptr) {
    info = &local;
  }
  std::error_code ec;
  // Distinguish "absent" from "present but not a regular file" from "open
  // failed" so a test can name the condition it injected.
  if (!std::filesystem::exists(path, ec) || ec) {
    info->status = LexiconStatus::kFileNotFound;
    info->line = 0;
    info->entry_count = 0;
    return nullptr;
  }
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    info->status = LexiconStatus::kFileUnreadable;
    info->line = 0;
    info->entry_count = 0;
    return nullptr;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    info->status = LexiconStatus::kFileUnreadable;
    info->line = 0;
    info->entry_count = 0;
    return nullptr;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (stream.bad()) {
    info->status = LexiconStatus::kFileUnreadable;
    info->line = 0;
    info->entry_count = 0;
    return nullptr;
  }
  return FromText(buffer.str(), info);
}

const LexiconEntry* EnglishLexicon::Find(std::string_view canonical_word) const {
  // Binary search over the order FromText just established. The comparator
  // compares entry.word against the queried view in std::string_view's byte order,
  // which is the same order the sort used -- no locale, no collation, no case
  // folding, and no assumption that the key is short.
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), canonical_word,
      [](const LexiconEntry& entry, std::string_view value) {
        return std::string_view(entry.word) < value;
      });
  if (it == entries_.end() || std::string_view(it->word) != canonical_word) {
    return nullptr;
  }
  return &(*it);
}

}  // namespace en
}  // namespace epocanva
