#include "session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace epocanva::backend_session {
namespace {
std::atomic<std::uint64_t> next_token{1};
std::uint64_t Token() {
  auto value = next_token.load(std::memory_order_relaxed);
  while (value != 0 && value != (std::numeric_limits<std::uint64_t>::max)()) {
    if (next_token.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
      return value;
  }
  return 0;
}
bool Utf8(std::string_view text) {
  if (text.size() > kMaxTextUtf8Bytes) return false;
  for (std::size_t utf8_byte = 0; utf8_byte < text.size();) {
    const auto lead = static_cast<unsigned char>(text[utf8_byte++]);
    std::uint32_t scalar = lead;
    unsigned remaining = 0;
    if (lead == 0 || lead < 0x20 || lead == 0x7f) return false;
    if (lead >= 0xc2 && lead <= 0xdf) { remaining = 1; scalar &= 0x1f; }
    else if (lead >= 0xe0 && lead <= 0xef) { remaining = 2; scalar &= 0x0f; }
    else if (lead >= 0xf0 && lead <= 0xf4) { remaining = 3; scalar &= 7; }
    else if (lead >= 0x80) return false;
    const auto count = remaining;
    if (remaining > text.size() - utf8_byte) return false;
    while (remaining--) {
      const auto byte = static_cast<unsigned char>(text[utf8_byte++]);
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 63);
    }
    if ((count == 1 && scalar < 0x80) || (count == 2 && scalar < 0x800) ||
        (count == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) || (scalar >= 0x80 && scalar <= 0x9f))
      return false;
  }
  return true;
}
bool Boundary(std::string_view text, std::size_t utf8_byte) {
  return utf8_byte <= text.size() && (utf8_byte == text.size() ||
      (static_cast<unsigned char>(text[utf8_byte]) & 0xc0) != 0x80);
}
Access ReadPermission(const PrivacyCallback& callback) {
  try { return callback ? callback() : Access{}; } catch (...) { return {}; }
}
bool Sha(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}
}  // namespace

struct Session::Impl {
  std::unique_ptr<learning::LearningStore> store;
  std::string model_sha256;
  Access identity;
  std::thread::id owner = std::this_thread::get_id();
  Snapshot snapshot;
  Commit commit;
  std::vector<LexemeId> context;
  LexemeId learning_previous_id = 0;
};
Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::~Session() = default;
std::unique_ptr<Session> Session::Create(learning::Catalog catalog,
                                         std::string model_sha256, Access access) {
  if (access.privacy != learning::PrivacyClass::kNormal || access.context_target == 0 ||
      access.generation == 0 || !Sha(model_sha256)) return nullptr;
  auto store = learning::LearningStore::Create(std::move(catalog));
  if (!store) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->store = std::move(store);
  impl->model_sha256 = std::move(model_sha256);
  impl->identity = access;
  return std::unique_ptr<Session>(new Session(std::move(impl)));
}
Status Session::Check(const Access& current) const {
  if (std::this_thread::get_id() != impl_->owner) return Status::kWrongThread;
  if (current.privacy != learning::PrivacyClass::kNormal) return Status::kDenied;
  if (current.context_target != impl_->identity.context_target ||
      current.generation != impl_->identity.generation) return Status::kStale;
  return Status::kOk;
}
Status Session::SetInput(std::string raw, std::string preedit, const Access& current) {
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (impl_->commit.token) return Status::kBusy;
  if (!Utf8(raw) || !Utf8(preedit)) return Status::kInvalid;
  const auto token = Token();
  if (!token) return Status::kTokenExhausted;
  impl_->snapshot = {token, std::move(raw), std::move(preedit), {}, true};
  return Status::kOk;
}
Status Session::Publish(std::vector<Candidate> candidates, bool complete, const Access& current) {
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (impl_->commit.token) return Status::kBusy;
  if (candidates.size() > kMaxCandidates) return Status::kInvalid;
  for (const auto& candidate : candidates) {
    if (candidate.text_utf8.empty() || !Utf8(candidate.text_utf8) || !std::isfinite(candidate.score) ||
        candidate.lexeme_ids.size() > learning::kMaxCommitLexemes ||
        candidate.input_begin_utf8_byte > candidate.input_end_utf8_byte ||
        !Boundary(impl_->snapshot.raw_utf8, candidate.input_begin_utf8_byte) ||
        !Boundary(impl_->snapshot.raw_utf8, candidate.input_end_utf8_byte) ||
        !std::all_of(candidate.lexeme_ids.begin(), candidate.lexeme_ids.end(),
          [&](LexemeId id) { return id != 0 && impl_->store->Admits(id); })) return Status::kInvalid;
  }
  const auto snapshot_token = Token();
  if (!snapshot_token) return Status::kTokenExhausted;
  for (auto& candidate : candidates) {
    candidate.token = Token();
    if (!candidate.token) return Status::kTokenExhausted;
  }
  impl_->snapshot.token = snapshot_token;
  impl_->snapshot.candidates = std::move(candidates);
  impl_->snapshot.complete = complete;
  return Status::kOk;
}
Status Session::ReadSnapshot(const Access& current, Snapshot* out) const {
  if (out) *out = {};
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (!out) return Status::kInvalid;
  *out = impl_->snapshot;
  return Status::kOk;
}
Status Session::Select(std::uint64_t snapshot_token, std::uint64_t candidate_token,
                       std::string remaining, const Access& current) {
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (impl_->commit.token) return Status::kBusy;
  if (!snapshot_token || snapshot_token != impl_->snapshot.token) return Status::kStale;
  if (!Utf8(remaining)) return Status::kInvalid;
  const auto& candidates = impl_->snapshot.candidates;
  auto found = std::find_if(candidates.begin(), candidates.end(),
      [candidate_token](const Candidate& candidate) { return candidate.token == candidate_token; });
  if (found == candidates.end()) return Status::kStale;
  Commit commit;
  commit.token = Token();
  if (!commit.token) return Status::kTokenExhausted;
  commit.text_utf8 = found->text_utf8;
  commit.lexeme_ids = found->lexeme_ids;
  commit.remaining_raw_utf8 = remaining;
  commit.remaining_preedit_utf8 = std::move(remaining);
  impl_->commit = std::move(commit);
  impl_->snapshot.candidates.clear();
  impl_->snapshot.token = 0;
  return Status::kOk;
}
Status Session::AcceptExternal(std::uint64_t snapshot_token, std::uint64_t candidate_token,
                               std::string committed, std::string raw_remaining,
                               std::string remaining, const Access& current) {
  const auto status = Check(current);
  if (status != Status::kOk) return status;
  if (!Utf8(committed) || committed.empty() || !Utf8(raw_remaining)) return Status::kInvalid;
  auto selected = Select(snapshot_token, candidate_token, std::move(remaining), current);
  if (selected != Status::kOk) return selected;
  if (impl_->commit.text_utf8 != committed) impl_->commit.lexeme_ids.clear();
  impl_->commit.text_utf8 = std::move(committed);
  impl_->commit.remaining_raw_utf8 = std::move(raw_remaining);
  return Status::kOk;
}
Status Session::ReadCommit(const Access& current, Commit* out) const {
  if (out) *out = {};
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (!out) return Status::kInvalid;
  if (!impl_->commit.token) return Status::kNoCommit;
  *out = impl_->commit;
  return Status::kOk;
}
Status Session::MarkCommitCopied(std::uint64_t token, const Access& current) {
  auto status = Check(current);
  if (status != Status::kOk) return status;
  if (!token || impl_->commit.token != token) return Status::kStale;
  impl_->commit.copied = true;
  return Status::kOk;
}
AckResult Session::Acknowledge(std::uint64_t token, bool applied,
                               const PrivacyCallback& callback, const Persist& persist) {
  AckResult result;
  if (std::this_thread::get_id() != impl_->owner) { result.status = Status::kWrongThread; return result; }
  if (!token || impl_->commit.token != token) { result.status = Status::kStale; return result; }
  if (!impl_->commit.copied) { result.status = Status::kBusy; return result; }
  const auto current = ReadPermission(callback);
  const auto status = Check(current);
  if (status != Status::kOk || !applied) {
    impl_->context.clear();
    impl_->learning_previous_id = 0;
    impl_->commit = {};
    impl_->snapshot = {};
    result.status = status;
    result.consumed = true;
    return result;
  }
  learning::Selection selected;
  selected.context_target = current.context_target;
  selected.generation = current.generation;
  selected.host_write_confirmed = true;
  selected.previous_id = current.learning_enabled ? impl_->learning_previous_id : 0;
  selected.lexeme_ids = impl_->commit.lexeme_ids;
  Snapshot next_snapshot;
  next_snapshot.raw_utf8 = impl_->commit.remaining_raw_utf8;
  next_snapshot.preedit_utf8 = impl_->commit.remaining_preedit_utf8;
  next_snapshot.token = Token();
  if (!next_snapshot.token) { result.status = Status::kTokenExhausted; return result; }
  auto next_context = impl_->context;
  if (selected.lexeme_ids.empty()) next_context.clear();
  else {
    for (auto id : selected.lexeme_ids) {
      next_context.push_back(id);
      if (next_context.size() > 2) next_context.erase(next_context.begin());
    }
  }
  // Consume before entering persistence: a partial/uncertain I/O result cannot
  // invite replaying the same host acknowledgement and double-counting a commit.
  impl_->commit = {};
  impl_->snapshot = std::move(next_snapshot);
  impl_->context = std::move(next_context);
  result.consumed = true;
  result.context_advanced = !selected.lexeme_ids.empty();
  result.status = Status::kOk;
  impl_->learning_previous_id = 0;
  if (!current.learning_enabled || selected.lexeme_ids.empty()) return result;
  auto fresh = ReadPermission(callback);
  if (Check(fresh) != Status::kOk || !fresh.learning_enabled) {
    result.persistence_status = learning::Status::kDenied;
    impl_->context.clear();
    result.context_advanced = false;
    return result;
  }
  if (persist) {
    try { result.persistence_status = persist(selected, callback); }
    catch (...) { result.persistence_status = learning::Status::kIoError; }
  }
  if (result.persistence_status != learning::Status::kOk) return result;
  fresh = ReadPermission(callback);
  if (Check(fresh) != Status::kOk || !fresh.learning_enabled) {
    result.persistence_status = learning::Status::kDenied;
    impl_->context.clear();
    result.context_advanced = false;
    return result;
  }
  result.persistence_status = impl_->store->Record(selected, fresh);
  result.learned = result.persistence_status == learning::Status::kOk;
  if (result.learned) impl_->learning_previous_id = selected.lexeme_ids.back();
  return result;
}
Status Session::Reset(const Access& current) {
  if (std::this_thread::get_id() != impl_->owner) return Status::kWrongThread;
  if (current.context_target != impl_->identity.context_target ||
      current.generation != impl_->identity.generation) return Status::kStale;
  impl_->snapshot = {};
  impl_->commit = {};
  impl_->context.clear();
  impl_->learning_previous_id = 0;
  return Status::kOk;
}
Status Session::SetContext(std::vector<LexemeId> ids, const Access& current,
                           bool append, bool clear_input) {
  const auto status = Check(current);
  if (status != Status::kOk) return status;
  if (impl_->commit.token) return Status::kBusy;
  if (ids.size() > 2 || !std::all_of(ids.begin(), ids.end(),
      [&](LexemeId id) { return id && impl_->store->Admits(id); })) return Status::kInvalid;
  const auto token = Token();
  if (!token) return Status::kTokenExhausted;
  auto context = append && !ids.empty() ? impl_->context : std::vector<LexemeId>{};
  context.insert(context.end(), ids.begin(), ids.end());
  if (context.size() > 2) context.erase(context.begin(), context.end() - 2);
  impl_->context = std::move(context);
  impl_->learning_previous_id = 0;
  if (clear_input) impl_->snapshot = {};
  impl_->snapshot.token = token;
  impl_->snapshot.candidates.clear();
  return Status::kOk;
}
std::vector<LexemeId> Session::Context(const Access& current) const {
  return Check(current) == Status::kOk ? impl_->context : std::vector<LexemeId>{};
}
learning::Status Session::ReadLearning(const Access& current, learning::Snapshot* out) const {
  if (Check(current) != Status::kOk) { if (out) *out = {}; return learning::Status::kDenied; }
  return impl_->store->Read(current, out);
}
learning::Status Session::ImportLearning(const learning::Snapshot& snapshot, const Access& current) {
  if (Check(current) != Status::kOk) return learning::Status::kDenied;
  if (snapshot.words.size() > learning::kMaxWordRecords || snapshot.pairs.size() > learning::kMaxPairRecords)
    return learning::Status::kLimitExceeded;
  std::string bytes = "epocanva-learning\t2\nnamespace\t" + impl_->store->catalog().name_space +
      "\ncatalog-sha256\t" + impl_->store->catalog().sha256 + "\nrevision\t" +
      std::to_string(snapshot.revision) + "\n";
  for (auto row : snapshot.words) bytes += "w\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  for (auto row : snapshot.pairs) bytes += "b\t" + std::to_string(row.previous_id) + "\t" +
      std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  return impl_->store->Load(bytes, current);
}
const learning::Catalog& Session::catalog() const { return impl_->store->catalog(); }
const std::string& Session::model_sha256() const { return impl_->model_sha256; }
}  // namespace epocanva::backend_session
