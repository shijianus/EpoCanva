#include "dictionary.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

// These private headers and the native API never escape dictionary.h.
#include "hunspell.hxx"

namespace epocanva::french_dictionary {
namespace {

constexpr char kHunspellRevision[] = "7d725f1929a4189a3b8188649af6a6b3e302f469";
constexpr std::size_t kMaxAffUtf8Bytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxDicUtf8Bytes = 64u * 1024u * 1024u;
// FileMgr uses a 65586-byte line buffer. Reject overlong physical lines before
// the upstream reader could silently stop. This is an envelope, not an affix parser.
constexpr std::size_t kMaxLineUtf8Bytes = 65535;
constexpr std::size_t kMaxResultUtf8Bytes = 4096;

bool Decode(std::string_view utf8, std::size_t* cursor_utf8_byte,
            char32_t* scalar) {
  const auto begin_utf8_byte = *cursor_utf8_byte;
  if (begin_utf8_byte >= utf8.size()) return false;
  const auto lead = static_cast<unsigned char>(utf8[begin_utf8_byte]);
  std::size_t scalar_utf8_bytes = 0;
  std::uint32_t value = 0, minimum = 0;
  if (lead < 0x80) {
    scalar_utf8_bytes = 1; value = lead;
  } else if (lead >= 0xc2 && lead <= 0xdf) {
    scalar_utf8_bytes = 2; value = lead & 0x1fu; minimum = 0x80;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    scalar_utf8_bytes = 3; value = lead & 0x0fu; minimum = 0x800;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    scalar_utf8_bytes = 4; value = lead & 0x07u; minimum = 0x10000;
  } else {
    return false;
  }
  if (scalar_utf8_bytes > utf8.size() - begin_utf8_byte) return false;
  for (std::size_t continuation_utf8_byte = 1;
       continuation_utf8_byte < scalar_utf8_bytes; ++continuation_utf8_byte) {
    const auto next = static_cast<unsigned char>(
        utf8[begin_utf8_byte + continuation_utf8_byte]);
    if ((next & 0xc0u) != 0x80u) return false;
    value = (value << 6u) | (next & 0x3fu);
  }
  if (value < minimum || value > 0x10ffffu ||
      (value >= 0xd800u && value <= 0xdfffu)) return false;
  *scalar = static_cast<char32_t>(value);
  *cursor_utf8_byte += scalar_utf8_bytes;
  return true;
}

bool ValidText(std::string_view utf8, bool file_envelope, bool morphology_record = false) {
  std::size_t cursor_utf8_byte = 0;
  while (cursor_utf8_byte < utf8.size()) {
    char32_t scalar = 0;
    if (!Decode(utf8, &cursor_utf8_byte, &scalar)) return false;
    if (scalar < 0x20 &&
        !((file_envelope || morphology_record) && scalar == U'\t') &&
        !(file_envelope && (scalar == U'\r' || scalar == U'\n'))) return false;
    if (scalar >= 0x7f && scalar <= 0x9f) return false;
  }
  return true;
}

Status ValidateQuery(std::string_view utf8) {
  if (utf8.empty()) return Status::kEmptyInput;
  if (utf8.size() > kMaxQueryUtf8Bytes) return Status::kInputTooLong;
  // Validate the entire encoding first, including a suffix after punctuation.
  std::size_t cursor_utf8_byte = 0;
  while (cursor_utf8_byte < utf8.size()) {
    char32_t scalar = 0;
    if (!Decode(utf8, &cursor_utf8_byte, &scalar)) return Status::kInvalidUtf8;
  }
  bool has_letter = false;
  bool previous_letter_or_mark = false;
  bool invalid_word = false;
  cursor_utf8_byte = 0;
  while (cursor_utf8_byte < utf8.size()) {
    char32_t scalar = 0;
    Decode(utf8, &cursor_utf8_byte, &scalar);
    if ((scalar >= U'a' && scalar <= U'z') ||
        (scalar >= U'A' && scalar <= U'Z')) {
      has_letter = true;
      previous_letter_or_mark = true;
      continue;
    }
    // Hyphens/elision and the dictionary's apostrophe BREAK rules are native.
    if (scalar == U'-' || scalar == U'\'' || scalar == U'\u2019' ||
        scalar == U'\u2018' || scalar == U'\u02bc') {
      previous_letter_or_mark = false;
      continue;
    }
    // All other ASCII punctuation, whitespace, digits and controls protect the
    // WHOLE token (URLs, email, filenames, paths, numbers and punctuated code).
    if (scalar < 0xa0) return Status::kProtectedText;
    // Hunspell's UTF-8 case tables/decoder are BMP-only at this exact revision.
    if (scalar > 0xffff) { invalid_word = true; continue; }
    const wchar_t unit_utf16 = static_cast<wchar_t>(scalar);
    WORD type1 = 0, type3 = 0;
    if (!GetStringTypeW(CT_CTYPE1, &unit_utf16, 1, &type1) ||
        !GetStringTypeW(CT_CTYPE3, &unit_utf16, 1, &type3)) {
      return Status::kEngineError;
    }
    if ((type1 & (C1_DIGIT | C1_SPACE | C1_CNTRL)) != 0)
      return Status::kProtectedText;
    // Windows marks some PRECOMPOSED accented letters with C3_DIACRITIC and
    // C3_NONSPACING as well. C1_ALPHA must take precedence: an initial é is a
    // letter, not an unattached combining mark. U+0301 has no C1_ALPHA bit.
    if ((type1 & C1_ALPHA) != 0) {
      has_letter = true;
      previous_letter_or_mark = true;
    } else if ((type3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0) {
      if (!previous_letter_or_mark) invalid_word = true;
    } else {
      invalid_word = true;
      previous_letter_or_mark = false;
    }
  }
  return has_letter && !invalid_word ? Status::kOk : Status::kInvalidWord;
}

bool IsHash(std::string_view hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}

struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
bool Sha256(const std::string& bytes, std::string* result) {
  Algorithm algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM,
                                   nullptr, 0) < 0) return false;
  std::array<unsigned char, 32> digest{};
  // The SDK's input buffer parameter is non-const; BCryptHash does not mutate it.
  if (BCryptHash(algorithm.handle, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), digest.data(),
      static_cast<ULONG>(digest.size())) < 0) return false;
  constexpr char hex[] = "0123456789abcdef";
  result->clear();
  result->reserve(64);
  for (const auto byte : digest) {
    result->push_back(hex[byte >> 4u]);
    result->push_back(hex[byte & 15u]);
  }
  return true;
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

Status FileError() {
  const auto error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
      ? Status::kMissingFile : Status::kIoError;
}

// Keep each read handle open without write/delete sharing until Hunspell has
// finished loading. Thus ordinary writers cannot change a verified leaf file
// between our hash and Hunspell's path-based open. No copied dictionary files.
struct FileSnapshot {
  Handle handle;
  std::string bytes;
  std::string native_path_utf8;

  Status Open(const std::filesystem::path& input, std::size_t max_utf8_bytes) {
    if (input.empty()) return Status::kInvalidOptions;
    auto native = input.native();
    if (native.find(L'\0') != std::wstring::npos) return Status::kInvalidOptions;
    std::replace(native.begin(), native.end(), L'/', L'\\');
    // Admit ordinary drive paths (and extended drive paths), never UNC, device,
    // URL, network mapped drives or alternate data streams.
    if (native.compare(0, 4, L"\\\\?\\") == 0) native.erase(0, 4);
    if (!native.empty() && native.front() == L'\\') return Status::kInvalidOptions;
    auto path = std::filesystem::absolute(std::filesystem::path(native));
    path = path.lexically_normal().make_preferred();
    native = path.native();
    if (native.size() < 4 || native[1] != L':' || native[2] != L'\\' ||
        !((native[0] >= L'A' && native[0] <= L'Z') ||
          (native[0] >= L'a' && native[0] <= L'z')) ||
        native.find(L':', 2) != std::wstring::npos) return Status::kInvalidOptions;
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_RAMDISK &&
        drive != DRIVE_REMOVABLE && drive != DRIVE_CDROM) return Status::kInvalidOptions;
    // Check ancestors before traversing them. OneDrive's locally resident,
    // non-name-surrogate cloud tags are not symlinks; reject placeholders that
    // would trigger recall, and all symlink/junction redirectors.
    auto component_path = path.root_path();
    for (const auto& component : path.relative_path()) {
      component_path /= component;
      const std::wstring extended = L"\\\\?\\" + component_path.native();
      const DWORD attributes = GetFileAttributesW(extended.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) return FileError();
      if ((attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                         FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0) return Status::kIoError;
      if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        Handle link(CreateFileW(extended.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (link.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(link.value, FileAttributeTagInfo,
                                          &tag, sizeof(tag))) return Status::kIoError;
        if (IsReparseTagNameSurrogate(tag.ReparseTag)) return Status::kInvalidOptions;
      }
    }
    const std::wstring extended = L"\\\\?\\" + native;
    handle.value = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) return FileError();
    if (GetFileType(handle.value) != FILE_TYPE_DISK) return Status::kInvalidOptions;
    LARGE_INTEGER file_bytes{};
    if (!GetFileSizeEx(handle.value, &file_bytes)) return Status::kIoError;
    if (file_bytes.QuadPart <= 0 ||
        static_cast<std::uint64_t>(file_bytes.QuadPart) > max_utf8_bytes)
      return Status::kInvalidDictionary;
    bytes.resize(static_cast<std::size_t>(file_bytes.QuadPart));
    std::size_t cursor_utf8_byte = 0;
    while (cursor_utf8_byte < bytes.size()) {
      const DWORD request_bytes = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - cursor_utf8_byte, 1024u * 1024u));
      DWORD read_bytes = 0;
      if (!ReadFile(handle.value, bytes.data() + cursor_utf8_byte,
                    request_bytes, &read_bytes, nullptr) || read_bytes == 0)
        return Status::kIoError;
      cursor_utf8_byte += read_bytes;
    }
    native_path_utf8 = std::filesystem::path(extended).u8string();
    return Status::kOk;
  }
};

std::string_view Trim(std::string_view line) {
  const auto begin_utf8_byte = line.find_first_not_of(" \t\r");
  if (begin_utf8_byte == std::string_view::npos) return {};
  const auto end_utf8_byte = line.find_last_not_of(" \t\r");
  return line.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte + 1);
}
bool NextLine(std::string_view text, std::size_t* cursor_utf8_byte,
              std::string_view* line) {
  if (*cursor_utf8_byte >= text.size()) return false;
  auto end_utf8_byte = text.find('\n', *cursor_utf8_byte);
  if (end_utf8_byte == std::string_view::npos) end_utf8_byte = text.size();
  *line = text.substr(*cursor_utf8_byte, end_utf8_byte - *cursor_utf8_byte);
  *cursor_utf8_byte = end_utf8_byte == text.size() ? end_utf8_byte : end_utf8_byte + 1;
  return true;
}
std::string_view WithoutBom(std::string_view text) {
  if (text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
  return text;
}
Status Envelope(std::string_view aff, std::string_view dic, SourceInfo* source) {
  if (!ValidText(aff, true) || !ValidText(dic, true)) return Status::kInvalidDictionary;
  std::size_t cursor_utf8_byte = 0;
  std::string_view line;
  bool encoding_seen = false;
  aff = WithoutBom(aff);
  while (NextLine(aff, &cursor_utf8_byte, &line)) {
    if (line.size() > kMaxLineUtf8Bytes) return Status::kInvalidDictionary;
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    const auto separator_utf8_byte = line.find_first_of(" \t");
    if (line.substr(0, separator_utf8_byte) != "SET") continue;
    if (encoding_seen || separator_utf8_byte == std::string_view::npos)
      return Status::kInvalidDictionary;
    if (Trim(line.substr(separator_utf8_byte)) != "UTF-8") return Status::kUnsupportedEncoding;
    encoding_seen = true;
  }
  if (!encoding_seen) return Status::kInvalidDictionary;
  cursor_utf8_byte = 0;
  dic = WithoutBom(dic);
  if (!NextLine(dic, &cursor_utf8_byte, &line)) return Status::kInvalidDictionary;
  line = Trim(line);
  const auto converted = std::from_chars(line.data(), line.data() + line.size(),
                                         source->dictionary_entry_hint);
  if (converted.ec != std::errc{} || converted.ptr != line.data() + line.size() ||
      source->dictionary_entry_hint == 0 || source->dictionary_entry_hint >= 10000000)
    return Status::kInvalidDictionary;
  while (NextLine(dic, &cursor_utf8_byte, &line)) {
    if (line.size() > kMaxLineUtf8Bytes) return Status::kInvalidDictionary;
    if (!Trim(line).empty()) ++source->dictionary_entry_records;
  }
  if (source->dictionary_entry_records == 0 ||
      (source->dictionary_entry_hint > 8192 &&
       source->dictionary_entry_hint > source->dictionary_entry_records * 10))
    return Status::kInvalidDictionary;
  return Status::kOk;
}

ListResult ListFailure(Status status) {
  ListResult result;
  result.status = status;
  return result;
}
// Does not re-rank. Hunspell may emit split-word corrections; those remain
// dictionary correction output, never a corpus probability/phrase prediction.
void CopyResults(std::vector<std::string> native, std::size_t max_results,
                 bool morphology_record, ListResult* result) {
  const auto cap = std::min(max_results, kMaxResults);
  for (auto& value : native) {
    if (value.empty() || value.size() > kMaxResultUtf8Bytes ||
        !ValidText(value, false, morphology_record)) {
      result->truncated = true;
      continue;
    }
    if (std::find(result->values_utf8.begin(), result->values_utf8.end(), value) !=
        result->values_utf8.end()) continue;
    if (result->values_utf8.size() == cap) { result->truncated = true; break; }
    result->values_utf8.push_back(std::move(value));
  }
}

}  // namespace

struct Dictionary::Impl {
  SourceInfo source;
  std::unique_ptr<Hunspell> native;
  mutable std::mutex mutex;

  enum class Operation { kSuggest, kAnalyze, kStem, kGenerate };
  ListResult Query(Operation operation, std::string_view word_utf8,
                   std::string_view example_utf8, std::size_t max_results) const {
    if (!native) return ListFailure(Status::kUnavailable);
    auto status = ValidateQuery(word_utf8);
    if (status != Status::kOk) return ListFailure(status);
    if (operation == Operation::kGenerate) {
      status = ValidateQuery(example_utf8);
      if (status != Status::kOk) return ListFailure(status);
    }
    ListResult result;
    result.status = Status::kOk;
    if (max_results == 0) return result;
    try {
      std::lock_guard<std::mutex> lock(mutex);
      const std::string word(word_utf8);
      result.input_correct = native->spell(word);
      std::vector<std::string> values;
      switch (operation) {
        case Operation::kSuggest:
          values = native->suggest(word);
          break;
        case Operation::kAnalyze:
          if (result.input_correct) values = native->analyze(word);
          break;
        case Operation::kStem:
          if (result.input_correct) values = native->stem(word);
          break;
        case Operation::kGenerate: {
          const std::string example(example_utf8);
          if (!result.input_correct || !native->spell(example)) {
            result.status = Status::kInvalidWord;
            return result;
          }
          values = native->generate(word, example);
          // Upstream generation's own spell filter has a time budget and can
          // stop early. Verify every returned form, not merely its affix shape.
          values.erase(std::remove_if(values.begin(), values.end(), [&](const std::string& value) {
            return ValidateQuery(value) != Status::kOk || !native->spell(value);
          }), values.end());
          break;
        }
      }
      CopyResults(std::move(values), max_results, operation == Operation::kAnalyze, &result);
      return result;
    } catch (...) {
      // No exception message: upstream exceptions can contain user/static text.
      return ListFailure(Status::kEngineError);
    }
  }
};

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kUnavailable: return "unavailable";
    case Status::kInvalidOptions: return "invalid_options";
    case Status::kMissingFile: return "missing_file";
    case Status::kIoError: return "io_error";
    case Status::kHashMismatch: return "hash_mismatch";
    case Status::kInvalidDictionary: return "invalid_dictionary";
    case Status::kUnsupportedEncoding: return "unsupported_encoding";
    case Status::kEmptyInput: return "empty_input";
    case Status::kInputTooLong: return "input_too_long";
    case Status::kInvalidUtf8: return "invalid_utf8";
    case Status::kProtectedText: return "protected_text";
    case Status::kInvalidWord: return "invalid_word";
    case Status::kEngineError: return "engine_error";
  }
  return "unknown";
}

Dictionary::Dictionary(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Dictionary::~Dictionary() = default;
std::shared_ptr<const Dictionary> Dictionary::Unavailable() {
  return std::shared_ptr<const Dictionary>(new Dictionary(std::make_unique<Impl>()));
}
std::shared_ptr<const Dictionary> Dictionary::Load(
    const std::filesystem::path& aff_path, const std::filesystem::path& dic_path,
    const ExpectedHashes& hashes, LoadInfo* info) {
  LoadInfo local;
  const auto fail = [&](Status status) -> std::shared_ptr<const Dictionary> {
    local.status = status;
    if (info) *info = local;
    return nullptr;
  };
  if (!IsHash(hashes.aff_sha256) || !IsHash(hashes.dic_sha256))
    return fail(Status::kInvalidOptions);
  try {
    FileSnapshot aff, dic;
    auto status = aff.Open(aff_path, kMaxAffUtf8Bytes);
    if (status != Status::kOk) return fail(status);
    std::string fingerprint;
    if (!Sha256(aff.bytes, &fingerprint)) return fail(Status::kEngineError);
    if (fingerprint != hashes.aff_sha256) return fail(Status::kHashMismatch);
    local.aff_hash_verified = true;
    status = dic.Open(dic_path, kMaxDicUtf8Bytes);
    if (status != Status::kOk) return fail(status);
    if (!Sha256(dic.bytes, &fingerprint)) return fail(Status::kEngineError);
    if (fingerprint != hashes.dic_sha256) return fail(Status::kHashMismatch);
    local.dic_hash_verified = true;
    auto impl = std::make_unique<Impl>();
    status = Envelope(aff.bytes, dic.bytes, &impl->source);
    if (status != Status::kOk) return fail(status);
    impl->native = std::make_unique<Hunspell>(aff.native_path_utf8.c_str(),
                                             dic.native_path_utf8.c_str());
    impl->native->set_trace_callback(nullptr, nullptr);
    if (impl->native->get_dict_encoding() != "UTF-8")
      return fail(Status::kUnsupportedEncoding);
    impl->source.available = true;
    impl->source.hashes_verified = true;
    impl->source.hashes = hashes;
    impl->source.hunspell_revision = kHunspellRevision;
    const auto dictionary = std::shared_ptr<const Dictionary>(new Dictionary(std::move(impl)));
    local.status = Status::kOk;
    if (info) *info = local;
    return dictionary;
  } catch (const std::filesystem::filesystem_error&) {
    return fail(Status::kIoError);
  } catch (...) {
    return fail(Status::kEngineError);
  }
}

bool Dictionary::available() const { return impl_->source.available; }
const SourceInfo& Dictionary::source() const { return impl_->source; }
SpellResult Dictionary::Spell(std::string_view word_utf8) const {
  SpellResult result;
  if (!available()) return result;
  result.status = ValidateQuery(word_utf8);
  if (result.status != Status::kOk) return result;
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    int native_info = 0;
    result.correct = impl_->native->spell(std::string(word_utf8), &native_info, &result.root_utf8);
    result.forbidden = (native_info & SPELL_FORBIDDEN) != 0;
    result.compound = (native_info & SPELL_COMPOUND) != 0;
    return result;
  } catch (...) {
    result = {};
    result.status = Status::kEngineError;
    return result;
  }
}
ListResult Dictionary::Suggest(std::string_view query_utf8, std::size_t max_results) const {
  return impl_->Query(Impl::Operation::kSuggest, query_utf8, {}, max_results);
}
ListResult Dictionary::Analyze(std::string_view word_utf8, std::size_t max_results) const {
  return impl_->Query(Impl::Operation::kAnalyze, word_utf8, {}, max_results);
}
ListResult Dictionary::Stem(std::string_view word_utf8, std::size_t max_results) const {
  return impl_->Query(Impl::Operation::kStem, word_utf8, {}, max_results);
}
ListResult Dictionary::Generate(std::string_view word_utf8, std::string_view example_utf8,
                                std::size_t max_results) const {
  return impl_->Query(Impl::Operation::kGenerate, word_utf8, example_utf8, max_results);
}

}  // namespace epocanva::french_dictionary
