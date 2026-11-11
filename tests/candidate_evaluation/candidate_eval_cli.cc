// candidate_eval_cli.cc -- E6 candidate-menu evaluation harness.
//
// Reads the staged backend resource index (data/backend/resources.tsv produced
// by tools/backend/stage_resources.py), verifies every pinned resource hash
// with the same reviewed SHA-256 helper the product uses, opens the real
// multi-profile backend once, and replays the committed Tier A corpus cases
// through the public C ABI. Per case it emits one JSON object whose repeats
// array carries the full ordered candidate menu of each in-process query.
//
// Privacy: candidate surfaces and raw inputs are written only to --output
// (which the driver keeps under build/). stderr carries stage names and
// numeric status codes only -- never input bytes, candidate text, or field
// identities. Learning is disabled on every session and no learning root is
// provided, so nothing is persisted. No network I/O exists on this path.
//
// Units: every offset/length at the ABI boundary is UTF-8 code units (bytes),
// matching engine/include/epocanva_backend.h. Latencies are nanoseconds from
// std::chrono::steady_clock around epocanva_backend_query_candidates only.
//
// Exit codes: 0 ok; 2 invalid arguments; 3 resource/backend failure;
// 4 corpus invalid; 5 output write failure.
#include "epocanva_backend.h"
#include "engine/backend/test_observation.h"
#include "platforms/windows/product/product_io.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMaxCandidates = 64;
constexpr std::size_t kMaxRawUtf8Bytes = 4096;
constexpr std::size_t kMaxCases = 4096;
constexpr std::size_t kMaxContextTokens = 2;
constexpr std::size_t kMaxContextTokenUtf8Bytes = 256;
constexpr std::size_t kMaxIdUtf8Bytes = 128;
constexpr std::uint32_t kMaxRepeat = 8;
constexpr std::string_view kResourceMagic = "epocanva-backend-resources\t1";
constexpr char kDefaultRimeVersion[] = "1.17.0";
constexpr char kEmptyUtf8[] = "";

[[noreturn]] void Fail(const char* stage, int code) {
  // Text-free diagnostic: stage name + numeric exit code only.
  std::cerr << "FAIL stage=" << stage << " exit=" << code << '\n';
  std::exit(code);
}

void Require(bool value, const char* stage, int code) {
  if (!value) Fail(stage, code);
}

std::string Slice(const epocanva_utf8_span& span, const std::string& bytes) {
  Require(span.utf8_byte_offset <= bytes.size() &&
              span.utf8_byte_count <= bytes.size() - span.utf8_byte_offset,
          "abi-span", 3);
  return bytes.substr(static_cast<std::size_t>(span.utf8_byte_offset),
                      static_cast<std::size_t>(span.utf8_byte_count));
}

template <class T>
T Init(std::uint32_t abi_version) {
  T value{};
  value.struct_size_bytes = sizeof(T);
  value.abi_version = abi_version;
  return value;
}

epocanva_utf8_view View(const std::string& text) {
  return epocanva_utf8_view{text.data(), static_cast<std::uint64_t>(text.size())};
}

bool StrictUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t continuation_count = 0;
    std::uint32_t scalar = 0;
    if (lead < 0x80) { ++index; continue; }
    if ((lead & 0xe0) == 0xc0) { continuation_count = 1; scalar = lead & 0x1fu; }
    else if ((lead & 0xf0) == 0xe0) { continuation_count = 2; scalar = lead & 0x0fu; }
    else if ((lead & 0xf8) == 0xf0) { continuation_count = 3; scalar = lead & 0x07u; }
    else return false;
    if (text.size() - index - 1 < continuation_count) return false;
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto byte = static_cast<unsigned char>(text[index + offset]);
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 0x3fu);
    }
    if (continuation_count == 1 && scalar < 0x80) return false;
    if (continuation_count == 2 && scalar < 0x800) return false;
    if (continuation_count == 3 && scalar < 0x10000) return false;
    if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    index += continuation_count + 1;
  }
  return true;
}

bool HasControlScalar(std::string_view text) {
  for (const unsigned char byte : text) {
    if (byte < 0x20 || byte == 0x7f) return true;
  }
  return false;
}

// --- strict JSON for the corpus line shape ----------------------------------
// Values are strings, nonnegative integers, or arrays of strings. Unknown keys
// are rejected. The evaluator performs the full corpus validation (duplicates,
// ownership, licensing); this parser only guarantees well-formed records.
struct CorpusCase {
  std::string id;
  std::string language;
  std::uint32_t profile_id = 0;
  std::string raw_input;
  std::string preedit;  // empty means "same as raw_input" (evaluator default)
  std::vector<std::string> context;
};

class JsonCursor {
 public:
  explicit JsonCursor(std::string_view text) : text_(text) {}

  void SkipSpace() {
    while (index_ < text_.size() &&
           (text_[index_] == ' ' || text_[index_] == '\t' || text_[index_] == '\r' ||
            text_[index_] == '\n')) {
      ++index_;
    }
  }
  char Peek() { SkipSpace(); return index_ < text_.size() ? text_[index_] : '\0'; }
  bool AtEnd() { SkipSpace(); return index_ >= text_.size(); }
  void Expect(char expected) {
    Require(Peek() == expected, "corpus-json-structure", 4);
    ++index_;
  }
  std::string String() {
    Require(Peek() == '"', "corpus-json-string", 4);
    ++index_;
    std::string value;
    while (index_ < text_.size()) {
      const unsigned char byte = static_cast<unsigned char>(text_[index_]);
      if (byte == '"') { ++index_; return value; }
      if (byte < 0x20) Fail("corpus-json-control", 4);
      if (byte != '\\') { value.push_back(static_cast<char>(byte)); ++index_; continue; }
      ++index_;
      Require(index_ < text_.size(), "corpus-json-escape", 4);
      const char escape = text_[index_++];
      switch (escape) {
        case '"': case '\\': case '/': value.push_back(escape); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: Fail("corpus-json-escape", 4);
      }
    }
    Fail("corpus-json-terminator", 4);
  }
  std::uint32_t Unsigned() {
    SkipSpace();
    Require(index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9',
            "corpus-json-number", 4);
    std::uint64_t value = 0;
    while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
      value = value * 10 + static_cast<std::uint64_t>(text_[index_] - '0');
      Require(value <= 0xffffffffu, "corpus-json-number", 4);
      ++index_;
    }
    return static_cast<std::uint32_t>(value);
  }
  // Skips any well-formed JSON value. The harness parses only the fields it
  // needs; the evaluator owns the strict corpus schema, so documented corpus
  // fields the harness does not use are skipped, not rejected.
  void SkipValue() {
    SkipSpace();
    Require(index_ < text_.size(), "corpus-json-value", 4);
    const char lead = text_[index_];
    if (lead == '"') { (void)String(); return; }
    if (lead == '-' || (lead >= '0' && lead <= '9')) {
      while (index_ < text_.size() && (text_[index_] == '-' || text_[index_] == '+' ||
             text_[index_] == '.' || text_[index_] == 'e' || text_[index_] == 'E' ||
             (text_[index_] >= '0' && text_[index_] <= '9'))) {
        ++index_;
      }
      return;
    }
    if (lead == '[') {
      ++index_;
      if (Peek() == ']') { ++index_; return; }
      for (;;) {
        SkipValue();
        if (Peek() == ',') { ++index_; continue; }
        break;
      }
      Expect(']');
      return;
    }
    if (lead == '{') {
      ++index_;
      if (Peek() == '}') { ++index_; return; }
      for (;;) {
        (void)String();
        Expect(':');
        SkipValue();
        if (Peek() == ',') { ++index_; continue; }
        break;
      }
      Expect('}');
      return;
    }
    for (const char* literal : {"true", "false", "null"}) {
      const std::size_t length = std::char_traits<char>::length(literal);
      if (text_.compare(index_, length, literal) == 0) {
        index_ += length;
        return;
      }
    }
    Fail("corpus-json-value", 4);
  }

 private:
  std::string_view text_;
  std::size_t index_ = 0;
};

CorpusCase ParseCase(const std::string& line) {
  JsonCursor cursor(line);
  CorpusCase parsed;
  bool has_id = false, has_language = false, has_profile = false, has_raw = false;
  cursor.Expect('{');
  Require(cursor.Peek() != '}', "corpus-json-empty-object", 4);
  for (;;) {
    const std::string key = cursor.String();
    cursor.Expect(':');
    if (key == "id") {
      Require(!has_id, "corpus-json-duplicate-key", 4);
      has_id = true;
      parsed.id = cursor.String();
    } else if (key == "language") {
      Require(!has_language, "corpus-json-duplicate-key", 4);
      has_language = true;
      parsed.language = cursor.String();
    } else if (key == "profile_id") {
      Require(!has_profile, "corpus-json-duplicate-key", 4);
      has_profile = true;
      parsed.profile_id = cursor.Unsigned();
    } else if (key == "raw_input") {
      Require(!has_raw, "corpus-json-duplicate-key", 4);
      has_raw = true;
      parsed.raw_input = cursor.String();
    } else if (key == "preedit") {
      parsed.preedit = cursor.String();
    } else if (key == "context") {
      cursor.Expect('[');
      if (cursor.Peek() != ']') {
        for (;;) {
          parsed.context.push_back(cursor.String());
          if (cursor.Peek() == ',') { cursor.Expect(','); continue; }
          break;
        }
      }
      cursor.Expect(']');
    } else {
      cursor.SkipValue();
    }
    if (cursor.Peek() == ',') { cursor.Expect(','); continue; }
    break;
  }
  cursor.Expect('}');
  Require(cursor.AtEnd(), "corpus-json-trailing", 4);
  Require(has_id && has_language && has_profile && has_raw, "corpus-json-required-field", 4);
  return parsed;
}

void ValidateCaseShape(const CorpusCase& parsed) {
  Require(parsed.id.size() >= 1 && parsed.id.size() <= kMaxIdUtf8Bytes, "corpus-id-bound", 4);
  for (const char byte : parsed.id) {
    const bool ok = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                    (byte >= 'A' && byte <= 'Z') || byte == '.' || byte == '_' || byte == '-';
    Require(ok, "corpus-id-charset", 4);
  }
  Require(!parsed.language.empty(), "corpus-language-nonempty", 4);
  Require(!parsed.raw_input.empty() && parsed.raw_input.size() <= kMaxRawUtf8Bytes,
          "corpus-raw-input-bound", 4);
  Require(StrictUtf8(parsed.raw_input), "corpus-raw-input-utf8", 4);
  Require(!HasControlScalar(parsed.raw_input), "corpus-raw-input-control", 4);
  Require(parsed.preedit.empty() ||
              (StrictUtf8(parsed.preedit) && !HasControlScalar(parsed.preedit) &&
               parsed.preedit.size() <= kMaxRawUtf8Bytes),
          "corpus-preedit-utf8", 4);
  Require(parsed.context.size() <= kMaxContextTokens, "corpus-context-count", 4);
  for (const auto& token : parsed.context) {
    Require(!token.empty() && token.size() <= kMaxContextTokenUtf8Bytes,
            "corpus-context-bound", 4);
    Require(StrictUtf8(token) && !HasControlScalar(token), "corpus-context-utf8", 4);
  }
}

// --- staged resource index ---------------------------------------------------
struct StagedProfile {
  std::uint32_t profile_id = 0;
  std::uint32_t language = 0;
  std::uint32_t input_mode = 0;
  std::uint32_t output_mode = 0;
  std::uint32_t keyboard_layout = 0;
  std::string variant_id;
  std::string model_sha256, conversion_sha256, auxiliary_sha256, abbreviation_sha256;
  fs::path model_abs, conversion_abs, auxiliary_abs, abbreviation_abs;
};

std::vector<std::string> SplitTsv(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  for (;;) {
    const auto separator = line.find('\t', begin);
    fields.push_back(line.substr(begin, separator == line.npos ? line.npos : separator - begin));
    if (separator == line.npos) return fields;
    begin = separator + 1;
  }
}

std::string ReadFileUtf8(const fs::path& path, const char* stage) {
  std::ifstream stream(path, std::ios::binary);
  Require(stream.is_open(), stage, 3);
  // istreambuf_iterator never sets eofbit, so success is judged by the bytes
  // themselves; every caller validates the content it expects.
  std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  Require(!bytes.empty(), stage, 3);
  return bytes;
}

void VerifySha256(const fs::path& path, const std::string& expected, const char* stage) {
  Require(epocanva::product::IsLowercaseHexSha256(expected), stage, 3);
  std::string actual;
  std::uint64_t size = 0;
  std::wstring reason;
  Require(epocanva::product::Sha256File(path, &actual, &size, &reason), stage, 3);
  Require(actual == expected, stage, 3);
}

std::vector<StagedProfile> ReadStagedProfiles(const fs::path& root) {
  const auto index_path = root / "data" / "backend" / "resources.tsv";
  const std::string bytes = ReadFileUtf8(index_path, "resource-index-readable");
  Require(bytes.size() >= kResourceMagic.size() &&
              bytes.compare(0, kResourceMagic.size(), kResourceMagic.data(),
                            kResourceMagic.size()) == 0,
          "resource-index-magic", 3);
  Require(bytes.size() <= 64 * 1024, "resource-index-size", 3);
  std::vector<StagedProfile> profiles;
  std::size_t line_begin = kResourceMagic.size();
  bool header_seen = false;
  while (line_begin < bytes.size()) {
    std::size_t line_end = bytes.find('\n', line_begin);
    if (line_end == std::string::npos) line_end = bytes.size();
    std::string line = bytes.substr(line_begin, line_end - line_begin);
    line_begin = line_end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto fields = SplitTsv(line);
    if (!header_seen) {
      Require(fields.size() == 14 && fields.front() == "profile_id",
              "resource-index-header", 3);
      header_seen = true;
      continue;
    }
    Require(fields.size() == 14, "resource-index-row-width", 3);
    StagedProfile profile;
    const auto number = [&fields](std::size_t ordinal) {
      const std::string& field = fields[ordinal];
      Require(!field.empty() && field.size() <= 10 &&
                  std::all_of(field.begin(), field.end(), [](char byte) {
                    return byte >= '0' && byte <= '9';
                  }),
              "resource-index-number", 3);
      const std::uint64_t value = std::stoull(field);
      Require(value <= 0xffffffffu, "resource-index-number", 3);
      return static_cast<std::uint32_t>(value);
    };
    profile.profile_id = number(0);
    Require(profile.profile_id != 0, "resource-index-profile-id", 3);
    profile.language = number(1);
    profile.input_mode = number(2);
    profile.output_mode = number(3);
    profile.keyboard_layout = number(4);
    profile.variant_id = fields[5];
    Require(!profile.variant_id.empty() && profile.variant_id.size() <= 64,
            "resource-index-variant", 3);
    const auto pair = [&fields, &root](std::size_t ordinal, std::string* digest,
                                       fs::path* absolute) {
      *digest = fields[ordinal + 1];
      if (fields[ordinal] == "-") {
        Require(*digest == "-", "resource-index-pair", 3);
        digest->clear();
        absolute->clear();
        return;
      }
      Require(!digest->empty(), "resource-index-pair", 3);
      *absolute = root / fs::u8path(fields[ordinal]);
    };
    pair(6, &profile.model_sha256, &profile.model_abs);
    pair(8, &profile.conversion_sha256, &profile.conversion_abs);
    pair(10, &profile.auxiliary_sha256, &profile.auxiliary_abs);
    pair(12, &profile.abbreviation_sha256, &profile.abbreviation_abs);
    profiles.push_back(std::move(profile));
    Require(profiles.size() <= 32, "resource-index-rows", 3);
  }
  Require(header_seen && !profiles.empty(), "resource-index-rows", 3);
  return profiles;
}

void VerifyStagedResources(const std::vector<StagedProfile>& profiles) {
  for (const auto& profile : profiles) {
    Require(!profile.model_abs.empty(), "resource-model-present", 3);
    VerifySha256(profile.model_abs, profile.model_sha256, "resource-model-hash");
    if (!profile.conversion_abs.empty()) {
      VerifySha256(profile.conversion_abs, profile.conversion_sha256, "resource-conversion-hash");
    }
    if (!profile.auxiliary_abs.empty()) {
      VerifySha256(profile.auxiliary_abs, profile.auxiliary_sha256, "resource-auxiliary-hash");
    }
    if (!profile.abbreviation_abs.empty()) {
      VerifySha256(profile.abbreviation_abs, profile.abbreviation_sha256,
                   "resource-abbreviation-hash");
    }
  }
}

std::string Json(std::string_view text) {
  std::string value = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char byte : text) {
    if (byte == '"' || byte == '\\') {
      value.push_back('\\');
      value.push_back(static_cast<char>(byte));
    } else if (byte < 0x20) {
      value += "\\u00";
      value.push_back(hex[byte >> 4]);
      value.push_back(hex[byte & 15]);
    } else {
      value.push_back(static_cast<char>(byte));
    }
  }
  return value + '"';
}

std::string SurfaceFingerprint(std::string_view text) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= UINT64_C(1099511628211);
  }
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

std::string Utf8BytesHex(std::string_view text) {
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(text.size() * 2);
  for (const unsigned char byte : text) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 15u]);
  }
  return hex;
}

bool ArtifactInsideBuild(const fs::path& path) {
  if (path.empty()) return false;
  for (const auto& component : path) if (component == "..") return false;
  std::error_code error;
  const auto root = fs::weakly_canonical(fs::u8path(EPOCANVA_EVAL_REPO_ROOT) / "build", error);
  if (error) return false;
  const auto target = fs::weakly_canonical(path, error);
  if (error || target == root) return false;
  auto current = target.begin();
  for (const auto& component : root) {
    if (current == target.end() || *current != component) return false;
    ++current;
  }
  return true;
}

const char* SourceName(epocanva::candidates::Source source) {
  using Source = epocanva::candidates::Source;
  switch (source) {
    case Source::kRaw: return "raw";
    case Source::kWord: return "word";
    case Source::kSpelling: return "spelling";
    case Source::kMorphology: return "morphology";
    case Source::kExpansion: return "expansion";
    case Source::kConversion: return "conversion";
    case Source::kPrediction: return "prediction";
  }
  Require(false, "unknown-contract-source", 3);
  return "";
}

const char* MatchName(epocanva::candidates::Match match) {
  using Match = epocanva::candidates::Match;
  switch (match) {
    case Match::kRaw: return "raw";
    case Match::kPrefix: return "prefix";
    case Match::kFuzzy: return "fuzzy";
    case Match::kAbbreviation: return "abbreviation";
    case Match::kContext: return "context";
    case Match::kConversion: return "conversion";
  }
  Require(false, "unknown-contract-match", 3);
  return "";
}

const char* CommitName(epocanva::candidates::Commit commit) {
  using Commit = epocanva::candidates::Commit;
  switch (commit) {
    case Commit::kInsert: return "insert";
    case Commit::kReplaceInput: return "replace_input";
  }
  Require(false, "unknown-contract-commit", 3);
  return "";
}

struct RepeatRow {
  std::uint32_t kind = 0;
  std::string text_utf8;
  std::vector<std::uint64_t> lexeme_ids;
  std::uint64_t input_begin_utf8_byte = 0;
  std::uint64_t input_end_utf8_byte = 0;
  epocanva::backend::testing::CandidateObservation observation;
};

struct RepeatResult {
  std::uint64_t latency_ns = 0;
  bool search_complete = false;
  std::string snapshot_raw_utf8;
  std::string preedit_utf8;
  std::vector<RepeatRow> candidates;
};

struct CaseResult {
  std::string id;
  std::uint32_t profile_id = 0;
  std::string status = "ok";
  std::vector<RepeatResult> repeats;
};

std::string JoinContext(const std::vector<std::string>& tokens, std::uint32_t language) {
  // Mirrors the backend's confirmed-context token joining: spaces between
  // tokens except for Chinese and Japanese.
  const bool spaced = language != EPOCANVA_BACKEND_ZH && language != EPOCANVA_BACKEND_JA;
  std::string text;
  for (const auto& token : tokens) {
    if (!text.empty() && spaced) text.push_back(' ');
    text += token;
  }
  return text;
}

int usage();

int Run(int argc, char** argv) {
  fs::path resources_root, cases_path, output_path, rime_dll, rime_shared, work_root;
  std::string language_arg, rime_dll_sha256, rime_version = kDefaultRimeVersion;
  std::uint32_t repeat = 2;
  std::uint32_t warmup_queries = 0;
  bool has_warmup_queries = false;
  bool load_only = false;
  for (int ordinal = 1; ordinal < argc; ++ordinal) {
    const std::string_view argument = argv[ordinal];
    const auto value = [&]() -> std::string {
      Require(ordinal + 1 < argc, "argument-missing", 2);
      return argv[++ordinal];
    };
    if (argument == "--resources") resources_root = fs::u8path(value());
    else if (argument == "--cases") cases_path = fs::u8path(value());
    else if (argument == "--language") language_arg = value();
    else if (argument == "--output") output_path = fs::u8path(value());
    else if (argument == "--repeat") {
      const std::string raw = value();
      Require(!raw.empty() && raw.size() <= 2 &&
                  std::all_of(raw.begin(), raw.end(), [](char byte) {
                    return byte >= '0' && byte <= '9';
                  }),
              "argument-repeat", 2);
      const std::uint64_t parsed = std::stoull(raw);
      Require(parsed >= 1 && parsed <= kMaxRepeat, "argument-repeat", 2);
      repeat = static_cast<std::uint32_t>(parsed);
    } else if (argument == "--warmup-queries") {
      const std::string raw = value();
      Require(!raw.empty() && raw.size() <= 6 &&
                  std::all_of(raw.begin(), raw.end(), [](char byte) {
                    return byte >= '0' && byte <= '9';
                  }), "argument-warmup-queries", 2);
      warmup_queries = static_cast<std::uint32_t>(std::stoul(raw));
      has_warmup_queries = true;
    } else if (argument == "--load-only") load_only = true;
    else if (argument == "--rime-dll") rime_dll = fs::u8path(value());
    else if (argument == "--rime-dll-sha256") rime_dll_sha256 = value();
    else if (argument == "--rime-shared") rime_shared = fs::u8path(value());
    else if (argument == "--rime-version") rime_version = value();
    else if (argument == "--work") work_root = fs::u8path(value());
    else return usage();
  }
  Require(!resources_root.empty() && !output_path.empty() && !language_arg.empty() &&
              (!cases_path.empty() || load_only),
          "argument-required", 2);
  // The evaluator reads the single policy file and passes its value. Requiring
  // it explicitly prevents a second hardcoded policy default in this harness.
  Require(has_warmup_queries || load_only, "argument-warmup-required", 2);
  Require(ArtifactInsideBuild(output_path) &&
              (work_root.empty() || ArtifactInsideBuild(work_root)),
          "artifact-outside-build", 2);
  // The backend's LocalResource loader pins absolute drive paths only, and the
  // engine options require absolute paths; resolve every argument against the
  // current directory so a relative invocation is still a correct one.
  const auto to_absolute = [](const fs::path& value) {
    std::error_code error;
    const auto canonical = fs::weakly_canonical(value, error);
    if (!error) return canonical;
    error.clear();
    const auto plain = fs::absolute(value, error);
    return error ? value : plain;
  };
  resources_root = to_absolute(resources_root);
  cases_path = to_absolute(cases_path);
  output_path = to_absolute(output_path);
  rime_dll = to_absolute(rime_dll);
  rime_shared = to_absolute(rime_shared);
  work_root = to_absolute(work_root);
  const std::uint32_t language = language_arg == "en" ? EPOCANVA_BACKEND_EN
      : language_arg == "zh" ? EPOCANVA_BACKEND_ZH
      : language_arg == "fr" ? EPOCANVA_BACKEND_FR
      : language_arg == "ja" ? EPOCANVA_BACKEND_JA
      : language_arg == "ko" ? EPOCANVA_BACKEND_KO : 0;
  Require(language != 0, "argument-language", 2);

  const auto staged = ReadStagedProfiles(resources_root);
  VerifyStagedResources(staged);

  // --- backend open ----------------------------------------------------------
  // deque keeps every std::string object address stable: short (SSO) strings
  // borrow their bytes from the object itself, and the ABI views below must
  // outlive the whole run.
  std::deque<std::string> path_storage;
  std::vector<epocanva_backend_profile_resource> bindings;
  bindings.reserve(staged.size());
  for (const auto& profile : staged) {
    epocanva_backend_profile_resource binding{};
    binding.resource = Init<epocanva_backend_resource>(EPOCANVA_BACKEND_ABI_VERSION);
    binding.resource.language = profile.language;
    binding.resource.model_path = View(path_storage.emplace_back(profile.model_abs.u8string()));
    binding.resource.model_sha256 = View(path_storage.emplace_back(profile.model_sha256));
    if (!profile.conversion_abs.empty()) {
      binding.resource.conversion_path =
          View(path_storage.emplace_back(profile.conversion_abs.u8string()));
      binding.resource.conversion_sha256 =
          View(path_storage.emplace_back(profile.conversion_sha256));
    }
    if (!profile.auxiliary_abs.empty()) {
      binding.resource.auxiliary_path =
          View(path_storage.emplace_back(profile.auxiliary_abs.u8string()));
      binding.resource.auxiliary_sha256 =
          View(path_storage.emplace_back(profile.auxiliary_sha256));
    }
    binding.profile = Init<epocanva_backend_profile>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    binding.profile.profile_id = profile.profile_id;
    binding.profile.language = profile.language;
    binding.profile.input_mode = profile.input_mode;
    binding.profile.output_mode = profile.output_mode;
    binding.profile.keyboard_layout = profile.keyboard_layout;
    binding.profile.variant_id = View(path_storage.emplace_back(profile.variant_id));
    if (!profile.abbreviation_abs.empty()) {
      binding.profile.abbreviation_path =
          View(path_storage.emplace_back(profile.abbreviation_abs.u8string()));
      binding.profile.abbreviation_sha256 =
          View(path_storage.emplace_back(profile.abbreviation_sha256));
    }
    bindings.push_back(binding);
  }
  Require(bindings.size() == staged.size(), "binding-parallel-array", 3);

  // Rime runtime: required to replay zh cases (the zh COMPLETION path reads its
  // menu through the Rime session). The ranked shared data is copied into an
  // isolated work tree so the staged inputs stay immutable and concurrent runs
  // never share Rime user data.
  const bool need_rime = std::any_of(staged.begin(), staged.end(),
                                     [](const StagedProfile& profile) {
                                       return profile.language == EPOCANVA_BACKEND_ZH;
                                     }) && language == EPOCANVA_BACKEND_ZH;
  epocanva_engine_options rime = Init<epocanva_engine_options>(1u);
  std::deque<std::string> rime_path_storage;
  std::vector<epocanva_utf8_view> schema_views;
  if (need_rime) {
    Require(!rime_dll.empty() && !rime_shared.empty() && !work_root.empty(),
            "argument-rime-required", 2);
    Require(!rime_dll_sha256.empty(), "argument-rime-sha256", 2);
    VerifySha256(rime_dll, rime_dll_sha256, "rime-dll-hash");
    for (const char* name : {"shared", "user", "logs"}) {
      std::error_code error;
      fs::create_directories(work_root / "rime" / name, error);
      Require(!error, "rime-work-directory", 3);
    }
    const auto shared_dir = work_root / "rime" / "shared";
    for (const char* name : {"default.yaml", "epocanva_backend_pinyin.schema.yaml",
                             "epocanva_backend_terra.dict.yaml",
                             "epocanva_backend_zhuyin.schema.yaml", "zhuyin.yaml"}) {
      const auto source = rime_shared / name;
      const auto destination = shared_dir / name;
      Require(fs::is_regular_file(source), "rime-shared-file-present", 3);
      std::error_code error;
      fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
      Require(!error, "rime-shared-copy", 3);
      std::string source_hash, destination_hash;
      std::uint64_t size = 0;
      std::wstring reason;
      Require(epocanva::product::Sha256File(source, &source_hash, &size, &reason),
              "rime-shared-source-hash", 3);
      Require(epocanva::product::Sha256File(destination, &destination_hash, &size, &reason),
              "rime-shared-copy-hash", 3);
      Require(source_hash == destination_hash, "rime-shared-copy-integrity", 3);
    }
    rime_path_storage.emplace_back(rime_dll.u8string());
    rime_path_storage.emplace_back(shared_dir.u8string());
    rime_path_storage.emplace_back((work_root / "rime" / "user").u8string());
    rime_path_storage.emplace_back((work_root / "rime" / "logs").u8string());
    rime_path_storage.emplace_back(rime_version);
    rime_path_storage.emplace_back(
        (shared_dir / "epocanva_backend_pinyin.schema.yaml").u8string());
    rime_path_storage.emplace_back(
        (shared_dir / "epocanva_backend_zhuyin.schema.yaml").u8string());
    rime.rime_dll_path = View(rime_path_storage[0]);
    rime.shared_data_dir = View(rime_path_storage[1]);
    rime.user_data_dir = View(rime_path_storage[2]);
    rime.log_dir = View(rime_path_storage[3]);
    rime.expected_rime_version = View(rime_path_storage[4]);
    schema_views.push_back(View(rime_path_storage[5]));
    schema_views.push_back(View(rime_path_storage[6]));
    rime.deployment_schema_paths = schema_views.data();
    rime.deployment_schema_count = static_cast<std::uint32_t>(schema_views.size());
  }

  epocanva_backend_profile_options options =
      Init<epocanva_backend_profile_options>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  options.profiles = bindings.data();
  options.profile_count = static_cast<std::uint32_t>(bindings.size());
  options.allow_test_fixture = 0;
  options.rime_options = need_rime ? &rime : nullptr;
  // No learning root at all: the harness never persists or learns.
  options.learning_root = epocanva_utf8_view{kEmptyUtf8, 0};
  const auto open_begin = std::chrono::steady_clock::now();
  epocanva_backend_handle backend = 0;
  const epocanva_status open_status = epocanva_backend_open_profiles(&options, &backend);
  const auto load_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - open_begin);
  if (open_status != EPOCANVA_OK) {
    // Text-free: the numeric ABI status identifies the failing layer.
    std::cerr << "FAIL stage=backend-open status=" << static_cast<long>(open_status) << '\n';
    std::exit(3);
  }
  Require(backend != 0, "backend-handle", 3);
  struct BackendGuard {
    epocanva_backend_handle handle;
    ~BackendGuard() { if (handle) (void)epocanva_backend_close(handle); }
  } backend_guard{backend};

  PROCESS_MEMORY_COUNTERS memory{};
  memory.cb = sizeof(memory);
  std::uint64_t peak_working_set_bytes = 0;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
    peak_working_set_bytes = static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
  }
  const double load_ms = std::chrono::duration<double, std::milli>(load_ns).count();

  if (load_only) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    Require(output.is_open(), "output-open", 5);
    output << "{\"load_ms\":" << load_ms
           << ",\"peak_working_set_bytes\":" << peak_working_set_bytes << "}\n";
    Require(output.good(), "output-write", 5);
    std::cout << "load_only=1 load_ms=" << load_ms
              << " peak_working_set_bytes=" << peak_working_set_bytes << '\n';
    return 0;
  }

  // --- corpus replay ----------------------------------------------------------
  const std::string corpus = ReadFileUtf8(cases_path, "corpus-readable");
  std::vector<CorpusCase> cases;
  {
    std::size_t line_begin = 0;
    while (line_begin < corpus.size()) {
      std::size_t line_end = corpus.find('\n', line_begin);
      if (line_end == std::string::npos) line_end = corpus.size();
      std::string line = corpus.substr(line_begin, line_end - line_begin);
      line_begin = line_end + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;  // the evaluator rejects blank lines; the harness skips them
      auto parsed = ParseCase(line);
      Require(parsed.language == language_arg, "corpus-language-mismatch", 4);
      ValidateCaseShape(parsed);
      cases.push_back(std::move(parsed));
      Require(cases.size() <= kMaxCases, "corpus-case-bound", 4);
    }
  }
  Require(!cases.empty(), "corpus-empty", 4);

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  Require(output.is_open(), "output-open", 5);
  output << std::setprecision(17);

  std::uint64_t context_target = 1000;
  for (const auto& parsed : cases) {
    std::size_t binding_ordinal = staged.size();
    for (std::size_t ordinal = 0; ordinal < staged.size(); ++ordinal) {
      if (staged[ordinal].profile_id == parsed.profile_id) binding_ordinal = ordinal;
    }
    Require(binding_ordinal < staged.size(), "corpus-profile-id", 4);
    const StagedProfile& binding = staged[binding_ordinal];
    Require(binding.language == language, "corpus-profile-language", 4);

    CaseResult result;
    result.id = parsed.id;
    result.profile_id = parsed.profile_id;

    epocanva_backend_session_options session_options =
        Init<epocanva_backend_session_options>(EPOCANVA_BACKEND_ABI_VERSION);
    session_options.context = epocanva_context{++context_target, 1, EPOCANVA_PRIVACY_NORMAL, 0};
    session_options.language = binding.language;
    session_options.input_mode = binding.input_mode;
    session_options.learning_enabled = 0;
    epocanva_backend_request request =
        Init<epocanva_backend_request>(EPOCANVA_BACKEND_ABI_VERSION);
    request.context = session_options.context;
    request.learning_enabled = 0;

    epocanva_backend_session_handle session = 0;
    const epocanva_status session_status = epocanva_backend_profile_session_open(
        backend, &bindings[binding_ordinal].profile, &session_options, &session);
    Require(session_status == EPOCANVA_OK, "session-open", 3);
    struct SessionGuard {
      epocanva_backend_session_handle handle;
      ~SessionGuard() { if (handle) (void)epocanva_backend_session_close(handle); }
    } session_guard{session};

    if (!parsed.context.empty()) {
      const std::string preceding = JoinContext(parsed.context, binding.language);
      const epocanva_status context_status =
          epocanva_backend_set_context(session, &request, View(preceding));
      Require(context_status == EPOCANVA_OK, "set-context", 3);
    }

    for (std::uint32_t query_ordinal = 0; query_ordinal < warmup_queries + repeat;
         ++query_ordinal) {
      const bool is_warmup = query_ordinal < warmup_queries;
      const epocanva_status input_status =
          epocanva_backend_set_input(session, &request, View(parsed.raw_input));
      Require(input_status == EPOCANVA_OK, "set-input", 3);

      RepeatResult repeat_result;
      epocanva_backend_query query = Init<epocanva_backend_query>(EPOCANVA_BACKEND_ABI_VERSION);
      query.context = request.context;
      query.learning_enabled = 0;
      query.query_kind = EPOCANVA_BACKEND_COMPLETION;
      query.max_candidates = kMaxCandidates;
      query.max_continuation_words = 1;
      query.search_budget = 0;
      const auto query_begin = std::chrono::steady_clock::now();
      const epocanva_status query_status = epocanva_backend_query_candidates(session, &query);
      const auto query_end = std::chrono::steady_clock::now();
      repeat_result.latency_ns =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              query_end - query_begin).count());
      if (query_status == EPOCANVA_BACKEND_UNAVAILABLE) {
        result.status = "unavailable";
        if (!is_warmup) result.repeats.push_back(std::move(repeat_result));
        break;
      }
      Require(query_status == EPOCANVA_OK, "query", 3);
      if (is_warmup) continue;

      epocanva_backend_profile_snapshot snapshot =
          Init<epocanva_backend_profile_snapshot>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
      snapshot.snapshot = Init<epocanva_backend_snapshot>(EPOCANVA_BACKEND_ABI_VERSION);
      epocanva_status status = epocanva_backend_read_profile_snapshot(
          session, &request, &snapshot, nullptr, 0);
      Require(status == EPOCANVA_OK || status == EPOCANVA_BUFFER_TOO_SMALL,
              "snapshot-sizing", 3);
      Require(snapshot.snapshot.required_utf8_bytes <= 8192, "snapshot-bound", 3);
      std::string snapshot_bytes(static_cast<std::size_t>(snapshot.snapshot.required_utf8_bytes),
                                 '\0');
      status = epocanva_backend_read_profile_snapshot(session, &request, &snapshot,
                                                      snapshot_bytes.data(),
                                                      snapshot_bytes.size());
      Require(status == EPOCANVA_OK, "snapshot-copy", 3);
      repeat_result.search_complete = snapshot.snapshot.search_complete != 0;
      repeat_result.snapshot_raw_utf8 = Slice(snapshot.snapshot.raw_input, snapshot_bytes);
      repeat_result.preedit_utf8 = Slice(snapshot.snapshot.preedit, snapshot_bytes);
      Require(snapshot.snapshot.candidate_count <= kMaxCandidates, "candidate-bound", 3);
      for (std::uint32_t candidate_ordinal = 0;
           candidate_ordinal < snapshot.snapshot.candidate_count; ++candidate_ordinal) {
        epocanva_backend_candidate candidate =
            Init<epocanva_backend_candidate>(EPOCANVA_BACKEND_ABI_VERSION);
        status = epocanva_backend_read_candidate(session, &request,
                                                 snapshot.snapshot.snapshot_token,
                                                 candidate_ordinal, &candidate, nullptr, 0);
        Require(status == EPOCANVA_BUFFER_TOO_SMALL, "candidate-sizing", 3);
        Require(candidate.required_utf8_bytes > 0 && candidate.required_utf8_bytes <= 4096,
                "candidate-bound", 3);
        std::string candidate_bytes(static_cast<std::size_t>(candidate.required_utf8_bytes), '\0');
        status = epocanva_backend_read_candidate(session, &request,
                                                 snapshot.snapshot.snapshot_token,
                                                 candidate_ordinal, &candidate,
                                                 candidate_bytes.data(),
                                                 candidate_bytes.size());
        Require(status == EPOCANVA_OK, "candidate-copy", 3);
        RepeatRow row;
        row.kind = candidate.kind;
        row.text_utf8 = Slice(candidate.text, candidate_bytes);
        Require(StrictUtf8(row.text_utf8), "candidate-utf8", 3);
        row.lexeme_ids.assign(candidate.lexeme_ids,
                              candidate.lexeme_ids + candidate.lexeme_count);
        row.input_begin_utf8_byte = candidate.input_begin_utf8_byte;
        row.input_end_utf8_byte = candidate.input_end_utf8_byte;
        status = epocanva::backend::testing::ReadCandidateObservation(
            session, &request, snapshot.snapshot.snapshot_token, candidate_ordinal,
            &row.observation);
        Require(status == EPOCANVA_OK, "internal-observation", 3);
        // A second read from the same snapshot must describe the very same
        // published row. The adapter does not manufacture missing metadata.
        const auto& contract = row.observation.contract;
        std::string contract_reason;
        Require(epocanva::candidates::Validate(contract, &contract_reason) &&
                    contract.text_utf8 == row.text_utf8 &&
                    contract.identity.lexeme_ids == row.lexeme_ids &&
                    static_cast<std::uint32_t>(contract.source) == row.kind &&
                    contract.score == candidate.score &&
                    contract.input_begin_utf8_byte == row.input_begin_utf8_byte &&
                    contract.input_end_utf8_byte == row.input_end_utf8_byte,
                "internal-observation-public-consistency", 3);
        repeat_result.candidates.push_back(std::move(row));
      }
      result.repeats.push_back(std::move(repeat_result));
    }
    if (result.repeats.size() < repeat && result.status == "ok") result.status = "incomplete";

    // One JSON object per case; candidate surfaces appear only in this file.
    output << "{\"case_id\":" << Json(result.id)
           << ",\"profile_id\":" << result.profile_id
           << ",\"raw_input_utf8_hex\":" << Json(Utf8BytesHex(parsed.raw_input))
           << ",\"status\":" << Json(result.status) << ",\"repeats\":[";
    for (std::size_t repeat_ordinal = 0; repeat_ordinal < result.repeats.size();
         ++repeat_ordinal) {
      const auto& repeat_result = result.repeats[repeat_ordinal];
      if (repeat_ordinal != 0) output << ',';
      output << "{\"latency_ns\":" << repeat_result.latency_ns
             << ",\"search_complete\":" << (repeat_result.search_complete ? "true" : "false")
             << ",\"snapshot_raw_utf8\":" << Json(repeat_result.snapshot_raw_utf8)
             << ",\"preedit_utf8\":" << Json(repeat_result.preedit_utf8)
             << ",\"candidates\":[";
      for (std::size_t ordinal = 0; ordinal < repeat_result.candidates.size(); ++ordinal) {
        const auto& row = repeat_result.candidates[ordinal];
        if (ordinal != 0) output << ',';
        output << "{\"rank\":" << ordinal << ",\"kind\":" << row.kind
               << ",\"text_utf8\":" << Json(row.text_utf8) << ",\"lexeme_ids\":[";
        for (std::size_t id_ordinal = 0; id_ordinal < row.lexeme_ids.size(); ++id_ordinal) {
          if (id_ordinal != 0) output << ',';
          output << row.lexeme_ids[id_ordinal];
        }
        const auto& contract = row.observation.contract;
        output << "],\"input_begin_utf8_byte\":" << row.input_begin_utf8_byte
               << ",\"input_end_utf8_byte\":" << row.input_end_utf8_byte
               << ",\"score\":" << contract.score
               << ",\"surface_fnv1a64\":" << Json(SurfaceFingerprint(row.text_utf8))
               << ",\"producer_insertion_ordinal\":" << row.observation.producer_insertion_ordinal
               << ",\"stable_id\":" << Json(contract.stable_id)
               << ",\"language_tag\":" << Json(contract.identity.language_tag)
               << ",\"scheme_id\":" << Json(contract.identity.scheme_id)
               << ",\"source\":" << Json(SourceName(contract.source))
               << ",\"match\":" << Json(MatchName(contract.match))
               << ",\"commit\":" << Json(CommitName(contract.commit))
               << ",\"learnable\":" << (contract.learnable ? "true" : "false") << '}';
      }
      output << "]}";
    }
    output << "]}\n";
    Require(output.good(), "output-write", 5);
  }
  if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
    peak_working_set_bytes = static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
  }
  output << "{\"run_meta\":{\"result_schema\":\"epocanva-candidate-results-v3\""
         << ",\"schema_version\":3,\"warmup_queries\":" << warmup_queries
         << ",\"warmup_scope\":\"per_case\",\"language\":" << Json(language_arg)
         << ",\"repeat\":" << repeat
         << ",\"case_count\":" << cases.size()
         << ",\"load_ms\":" << load_ms
         << ",\"peak_working_set_bytes\":" << peak_working_set_bytes
         << ",\"resource_profile_count\":" << staged.size() << "}}\n";
  output.flush();
  Require(output.good(), "output-write", 5);
  std::cout << "evaluated_cases=" << cases.size() << " repeat=" << repeat << '\n';
  return 0;
}

int usage() {
  std::cerr << "usage: candidate_eval_cli --resources <staged-root> --cases <corpus.jsonl>\n"
            << "                         --language <en|zh|fr|ja|ko> --output <results.jsonl>\n"
            << "                         --warmup-queries <policy-value> [--repeat 2] [--load-only]\n"
            << "                         [--rime-dll <path> --rime-dll-sha256 <hex>\n"
            << "                          --rime-shared <dir> --rime-version 1.17.0\n"
            << "                          --work <dir>]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (...) {
    std::cerr << "FAIL stage=unhandled-exception exit=10\n";
    return 10;
  }
}
