// Full Hunspell .aff/.dic execution, not a second French affix parser.
#ifndef EPOCANVA_FRENCH_DICTIONARY_DICTIONARY_H_
#define EPOCANVA_FRENCH_DICTIONARY_DICTIONARY_H_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva::french_dictionary {

constexpr std::size_t kMaxQueryUtf8Bytes = 256;
constexpr std::size_t kMaxResults = 64;  // result records, not text units

// Only fixed status names may be logged. Never log query/result text or paths.
enum class Status {
  kOk = 0, kUnavailable, kInvalidOptions, kMissingFile, kIoError,
  kHashMismatch, kInvalidDictionary, kUnsupportedEncoding, kEmptyInput,
  kInputTooLong, kInvalidUtf8, kProtectedText, kInvalidWord, kEngineError,
};
const char* StatusName(Status status);

struct ExpectedHashes {
  // Mandatory 64 lowercase ASCII hex bytes each. Trusted native packaging/main
  // supplies BOTH hashes from its verified manifest, never from query input.
  // Load independently checks both files. A hash is integrity, not authenticity.
  std::string aff_sha256;
  std::string dic_sha256;
};
struct LoadInfo {
  Status status = Status::kUnavailable;
  bool aff_hash_verified = false;
  bool dic_hash_verified = false;
};
struct SourceInfo {
  bool available = false;
  bool hashes_verified = false;
  ExpectedHashes hashes;
  // Hunspell's first .dic line is an allocation hint, NOT an exact row count.
  std::size_t dictionary_entry_hint = 0;  // dictionary records
  std::size_t dictionary_entry_records = 0;  // nonempty dictionary records
  std::string hunspell_revision;
};
struct SpellResult {
  Status status = Status::kUnavailable;
  bool correct = false;
  bool forbidden = false;
  bool compound = false;
  std::string root_utf8;
};
struct ListResult {
  Status status = Status::kUnavailable;
  bool input_correct = false;
  bool truncated = false;  // our output cap, NOT Hunspell search completeness
  // Suggest/Stem/Generate: words. Analyze: opaque Hunspell morphology records.
  // All text uses UTF-8 code units (bytes); order is Hunspell's, not frequency.
  std::vector<std::string> values_utf8;
};

class Dictionary {
 public:
  // Atomic construction; returns nullptr on failure and sets info if supplied.
  // Explicit paired LOCAL paths, never an inferred .dic sibling or network path.
  // Full file hashes and a bounded UTF-8 file envelope are checked before calling
  // Hunspell. The hashes must name reviewed dictionaries: this is NOT a general
  // untrusted-affix validator, and Hunspell exposes no complete parse status.
  static std::shared_ptr<const Dictionary> Load(
      const std::filesystem::path& aff_path,
      const std::filesystem::path& dic_path,
      const ExpectedHashes& hashes, LoadInfo* info = nullptr);
  static std::shared_ptr<const Dictionary> Unavailable();
  ~Dictionary();
  Dictionary(const Dictionary&) = delete;
  Dictionary& operator=(const Dictionary&) = delete;

  bool available() const;
  const SourceInfo& source() const;

  // Native caller MUST gate sensitive/password/OTP fields before EVERY call.
  // No learning, history, persistence, logging, network or dictionary mutation.
  // A shared const instance is logically immutable, NOT lock-free: all Hunspell
  // calls are serialized by an internal mutex. No Hunspell types cross this API.
  // Whole-token guards refuse URLs/mail/paths/code/digits; no correcting a suffix
  // of protected input. Hunspell handles French case/ICONV/OCONV, not ASCII fold.
  SpellResult Spell(std::string_view word_utf8) const;
  ListResult Suggest(std::string_view query_utf8,
                     std::size_t max_results = 16) const;
  ListResult Analyze(std::string_view word_utf8,
                     std::size_t max_results = 16) const;
  ListResult Stem(std::string_view word_utf8,
                  std::size_t max_results = 16) const;
  // Morphological generation by a valid example, NOT blind suffix expansion.
  // Coverage depends on actual dictionary morphology tags; an empty result is
  // honest. Does not invent tags, frequencies, phrases or conjugation paradigms.
  ListResult Generate(std::string_view word_utf8,
                      std::string_view example_utf8,
                      std::size_t max_results = 16) const;
  // All list caps count records, clamp to kMaxResults; zero makes no engine call.
  // No API promises exhaustive suggestions or a hard wall-clock deadline.

 private:
  struct Impl;
  explicit Dictionary(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace epocanva::french_dictionary
#endif  // EPOCANVA_FRENCH_DICTIONARY_DICTIONARY_H_
