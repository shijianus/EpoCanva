#ifndef EPOCANVA_JAPANESE_DECODER_H_
#define EPOCANVA_JAPANESE_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace japanese_decoder {

// All offsets and lengths in this API are UTF-8 code units (bytes). The caller
// supplies already-composed kana reading; romaji and kana composition are outside
// this decoder.
enum class Status {
  kOk = 0,
  kUnavailable,
  kInvalidArgument,
  kInvalidUtf8,
  kInvalidResource,
  kResourceHashMismatch,
  kResourceTooLarge,
  kSearchBudgetExceeded,
  kPathRejected,
  kFileReadFailed,
  kHeaderInvalid,
  kRecordInvalid,
};

struct Segment {
  std::string surface_utf8;
  std::string reading_utf8;
  std::size_t input_begin_utf8_bytes = 0;
  std::size_t input_end_utf8_bytes = 0;
  std::uint32_t left_pos_id = 0;
  std::uint32_t right_pos_id = 0;
  bool unknown = false;
};

struct Candidate {
  std::string text_utf8;
  std::vector<Segment> segments;
  std::int64_t cost = 0;
  bool complete = false;
};

struct LoadOptions {
  // Maximum resident lexicon bytes, including trie, lexemes, POS and connection
  // data. The decoder rejects a resource whose measured representation exceeds it.
  std::size_t max_memory_bytes = 256u * 1024u * 1024u;
  // Maximum source dictionary input bytes accepted while loading. This bounds
  // temporary parser memory and excludes the generated output from this limit.
  std::size_t max_source_bytes = 256u * 1024u * 1024u;
  // The expected SHA-256 is lowercase hexadecimal over the exact compiled binary.
  // An empty value means the caller did not bind a hash and is rejected by Load.
  std::string expected_sha256;
};

class Decoder {
 public:
  Decoder();
  ~Decoder();
  Decoder(Decoder&&) noexcept;
  Decoder& operator=(Decoder&&) noexcept;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // Opens an offline compiler-produced binary lexicon. No network, directory
  // search, learning, persistence, or content logging occurs. A failed load
  // leaves the decoder unavailable and does not retain partial data.
  Status Load(const std::string& binary_path, const LoadOptions& options = {});

  // Converts a UTF-8 kana reading to at most max_candidates bounded k-best paths.
  // Search state is (reading UTF-8 byte boundary, right POS ID); no greedy
  // longest-match assumption is made. Unknown kana fallback is explicit in each
  // affected segment and is used only for valid kana scalars not covered by the
  // lexicon. Partial input may therefore produce complete=false candidates.
  Status Convert(std::string_view reading_utf8, std::size_t max_candidates,
                 std::vector<Candidate>* out_candidates) const;

  bool available() const noexcept;
  std::size_t resident_bytes() const noexcept;
  std::uint32_t connection_dimension() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* StatusName(Status status) noexcept;

}  // namespace japanese_decoder
}  // namespace epocanva

#endif  // EPOCANVA_JAPANESE_DECODER_H_
