#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva::neural {

// This optional module never invents candidates, learns, persists, or sends data.
// Model token counts below are tokenizer tokens, NOT UTF-8/UTF-16 text offsets.
enum class Status {
  kOk,
  kUnavailable,
  kInvalidArgument,
  kInvalidModel,
  kPrivacyDenied,
  kLimitExceeded,
  kCancelled,
  kDeadlineExceeded,
  kStaleRequest,
  kBusy,
  kInternalError,
};

const char* StatusName(Status status) noexcept;

struct RequestControl {
  // The caller must keep these atomics alive through the synchronous call.
  const std::atomic<bool>* cancelled = nullptr;
  const std::atomic<std::uint64_t>* current_version = nullptr;
  std::uint64_t expected_version = 0;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
};

struct Limits {
  std::uint32_t context_tokens = 512;       // Total resident tokenizer tokens.
  std::uint32_t max_candidate_tokens = 32; // Per candidate continuation tokens.
  std::size_t max_candidates = 8;          // Candidate array elements.
  std::size_t max_context_utf8_bytes = 2048;
  std::size_t max_candidate_utf8_bytes = 128;
  std::uint32_t worker_threads = 4;
  std::chrono::milliseconds max_request_time{200};
};

struct ModelInfo {
  std::string sha256; // Exactly 64 lowercase ASCII UTF-8 bytes.
  std::uint64_t file_bytes = 0;
  std::uint64_t parameter_count = 0;
  std::uint32_t vocabulary_tokens = 0;
  std::uint32_t training_context_tokens = 0;
};

class LexicalReranker;

// Only local, pinned, read-only Qwen2.5-0.5B GGUF resources are accepted. This
// Windows implementation holds a non-write/non-delete-shared file handle for
// the model lifetime. No LoadLibrary/DLL search is used by this module.
class Model final {
 public:
  static std::shared_ptr<const Model> Load(
      const std::filesystem::path& absolute_local_gguf,
      std::string_view expected_sha256, Status* status,
      const RequestControl& control = {});
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;
  const ModelInfo& info() const noexcept;

 private:
  struct Impl;
  explicit Model(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class LexicalReranker;
};

struct ScoredCandidate {
  std::size_t candidate_index = 0; // Element index in the caller's candidate array.
  double log_probability = 0;     // Sum of natural-log token probabilities.
  std::uint32_t scored_tokens = 0;
};

struct Result {
  Status status = Status::kUnavailable;
  std::uint64_t version = 0;
  // Populated ONLY on kOk. Ties retain caller order. All indices refer to
  // unchanged caller-provided lexical candidates; no generated text is returned.
  std::vector<ScoredCandidate> ranking;
  std::uint64_t elapsed_microseconds = 0;
};

class LexicalReranker final {
 public:
  static std::unique_ptr<LexicalReranker> Create(
      std::shared_ptr<const Model> model, const Limits& limits, Status* status);
  ~LexicalReranker();
  LexicalReranker(const LexicalReranker&) = delete;
  LexicalReranker& operator=(const LexicalReranker&) = delete;

  // prefix_utf8 is the admitted text immediately BEFORE insertion, with exact
  // spacing supplied by the caller. Candidate UTF-8 bytes are appended verbatim
  // for joint tokenization. The caller validates lexical eligibility and must
  // re-check its request/permission version before applying the result.
  // privacy_permitted defaults to denial in callers; password, OTP, and unknown
  // privacy-classified fields MUST NOT be admitted. A false value is checked
  // before reading prefix/candidate bytes. Non-kOk means use lexical fallback.
  // One call per instance; simultaneous calls return kBusy rather than block.
  Result Rerank(std::string_view prefix_utf8,
                const std::vector<std::string_view>& lexical_candidates_utf8,
                bool privacy_permitted,
                const RequestControl& control = {});

 private:
  struct Impl;
  explicit LexicalReranker(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace epocanva::neural
