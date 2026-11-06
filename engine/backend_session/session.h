#ifndef EPOCANVA_BACKEND_SESSION_SESSION_H_
#define EPOCANVA_BACKEND_SESSION_SESSION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "candidate_contract.h"
#include "learning_store.h"

namespace epocanva::backend_session {
using LexemeId = learning::LexemeId;
using Access = learning::Access;
using PrivacyCallback = std::function<Access()>;
constexpr std::size_t kMaxTextUtf8Bytes = 4096;
constexpr std::size_t kMaxCandidates = 64;

enum class Status {
  kOk = 0, kDenied, kStale, kInvalid, kBusy, kNoCommit,
  kBufferTooSmall, kTokenExhausted, kWrongThread
};
struct Candidate {
  std::uint64_t token = 0;
  std::uint32_t kind = 0;
  std::string text_utf8;
  std::vector<LexemeId> lexeme_ids;
  double score = 0;
  std::size_t input_begin_utf8_byte = 0;
  std::size_t input_end_utf8_byte = 0;
  bool exact_expansion = false;
  // Unified identity/contract for this candidate. Populated by the backend
  // adapter; the ABI still reads the v1 fields above unchanged.
  candidates::Record contract;
};
struct Snapshot {
  std::uint64_t token = 0;
  std::string raw_utf8;
  std::string preedit_utf8;
  std::vector<Candidate> candidates;
  bool complete = true;
};
struct Commit {
  std::uint64_t token = 0;
  std::string text_utf8;
  std::string remaining_preedit_utf8;
  std::string remaining_raw_utf8;
  std::vector<LexemeId> lexeme_ids;
  bool copied = false;
};
struct AckResult {
  Status status = Status::kInvalid;
  bool consumed = false;
  bool context_advanced = false;
  bool learned = false;
  learning::Status persistence_status = learning::Status::kOk;
};
// Called only for an acknowledged edit, with a current provider-owned permission
// lock. A failed persistence attempt never changes the already-applied host edit.
using Persist = std::function<learning::Status(const learning::Selection&, const PrivacyCallback&)>;

class Session {
 public:
  static std::unique_ptr<Session> Create(learning::Catalog catalog,
                                         std::string model_sha256, Access access);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Status Check(const Access& current) const;
  Status SetInput(std::string raw_utf8, std::string preedit_utf8, const Access& current);
  Status Publish(std::vector<Candidate> candidates, bool complete, const Access& current);
  Status ReadSnapshot(const Access& current, Snapshot* out) const;
  Status Select(std::uint64_t snapshot_token, std::uint64_t candidate_token,
                std::string remaining_preedit_utf8, const Access& current);
  Status ReadCommit(const Access& current, Commit* out) const;
  Status AcceptExternal(std::uint64_t snapshot_token, std::uint64_t candidate_token,
                        std::string committed_utf8, std::string remaining_raw_utf8,
                        std::string remaining_preedit_utf8, const Access& current);
  Status MarkCommitCopied(std::uint64_t token, const Access& current);
  AckResult Acknowledge(std::uint64_t token, bool applied,
                       const PrivacyCallback& permission, const Persist& persist = {});
  // Explicit discard accepts Sensitive/Unknown so callers can clear transient
  // text without pretending the field is Normal. Target/generation must match.
  Status Reset(const Access& current);
  // Imported host context is transient and breaks the personal-learning chain.
  // IDs must already belong to this catalog. No text or statistics are stored.
  Status SetContext(std::vector<LexemeId> ids, const Access& current,
                    bool append = false, bool clear_input = false);
  std::vector<LexemeId> Context(const Access& current) const;
  learning::Status ReadLearning(const Access& current, learning::Snapshot* out) const;
  learning::Status ImportLearning(const learning::Snapshot& snapshot, const Access& current);
  const learning::Catalog& catalog() const;
  const std::string& model_sha256() const;
 private:
  struct Impl;
  explicit Session(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace epocanva::backend_session
#endif
