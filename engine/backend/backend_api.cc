#include "epocanva_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "engine/backend_session/session.h"
#include "engine/candidates/candidate_contract.h"
#include "engine/lexical_service/lexical_service.h"
#include "engine/prediction/model.h"
#include "engine/prediction/abbreviation.h"
#include "context_text.h"
#include <fstream>
#include "engine/languages/ja/romaji_composer.h"
#include "engine/languages/ko/two_set_composer.h"
#include "engine/languages/zh/pinyin_initialism.h"
#include "engine/japanese_decoder/japanese_decoder.h"
#include "engine/data_store/windows_learning_repository.h"
#include "engine/ranking/adaptive_ranker.h"
#include "product_io.h"
#include "local_resource.h"
#ifdef EPOCANVA_BACKEND_FRENCH
#include "engine/french_dictionary/dictionary.h"
#endif

namespace {
namespace pred = epocanva::prediction;
namespace lex = epocanva::lexical;
namespace flow = epocanva::backend_session;
namespace learn = epocanva::learning;
namespace rk = epocanva::ranking;
namespace cand = epocanva::candidates;
constexpr std::size_t kMaxInputBytes = 4096;
const char* Language(std::uint32_t language) {
  switch (language) {
    case EPOCANVA_BACKEND_EN: return "en";
    case EPOCANVA_BACKEND_ZH: return "zh";
    case EPOCANVA_BACKEND_FR: return "fr";
    case EPOCANVA_BACKEND_JA: return "ja";
    case EPOCANVA_BACKEND_KO: return "ko";
    default: return nullptr;
  }
}
const char* Scheme(std::uint32_t input_mode) {
  switch (input_mode) {
    case EPOCANVA_BACKEND_LATIN: return "latin";
    case EPOCANVA_BACKEND_PINYIN: return "pinyin";
    case EPOCANVA_BACKEND_ZHUYIN: return "zhuyin";
    case EPOCANVA_BACKEND_ROMAJI: return "romaji";
    case EPOCANVA_BACKEND_KATAKANA: return "katakana";
    case EPOCANVA_BACKEND_TWO_SET: return "two_set";
    case EPOCANVA_BACKEND_KANA: return "kana";
    default: return nullptr;
  }
}
template<class T> epocanva_status Structure(const T* value, std::uint32_t version = EPOCANVA_BACKEND_ABI_VERSION) {
  if (!value) return EPOCANVA_INVALID_ARGUMENT;
  if (value->struct_size_bytes < sizeof(T)) return EPOCANVA_STRUCT_TOO_SMALL;
  return value->abi_version == version ? EPOCANVA_OK : EPOCANVA_ABI_VERSION_MISMATCH;
}
template<class T> epocanva_status Output(T* value) {
  auto status = Structure(value);
  if (status != EPOCANVA_OK) return status;
  const auto bytes = value->struct_size_bytes;
  *value = {};
  value->struct_size_bytes = bytes;
  value->abi_version = EPOCANVA_BACKEND_ABI_VERSION;
  return EPOCANVA_OK;
}
template<class F> epocanva_status Guard(F&& operation) noexcept {
  try { return operation(); }
  catch (const std::bad_alloc&) { return EPOCANVA_OUT_OF_MEMORY; }
  catch (const std::invalid_argument&) { return EPOCANVA_INVALID_ARGUMENT; }
  catch (...) { return EPOCANVA_INTERNAL_ERROR; }
}
std::string Text(epocanva_utf8_view view, std::size_t limit = kMaxInputBytes) {
  if ((!view.data && view.utf8_byte_count) || view.utf8_byte_count > limit)
    throw std::invalid_argument("invalid-view");
  std::string text;
  if (view.utf8_byte_count) text.assign(view.data, static_cast<std::size_t>(view.utf8_byte_count));
  if (!pred::IsValidUtf8(text) || text.find('\0') != text.npos) throw std::invalid_argument("invalid-utf8");
  return text;
}
bool Digest(const std::filesystem::path& path, const std::string& expected) {
  if (!path.is_absolute() || expected.size() != 64) return false;
  if (!std::all_of(expected.begin(), expected.end(), [](char byte) {
      return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'); })) return false;
  std::string actual;
  std::uint64_t bytes = 0;
  std::wstring why;
  return epocanva::product::Sha256File(path, &actual, &bytes, &why) && actual == expected;
}
epocanva_status Result(flow::Status status) {
  switch (status) {
    case flow::Status::kOk: return EPOCANVA_OK;
    case flow::Status::kDenied: return EPOCANVA_PRIVACY_DENIED;
    case flow::Status::kStale: return EPOCANVA_STALE_SNAPSHOT;
    case flow::Status::kBusy: return EPOCANVA_BUSY;
    case flow::Status::kNoCommit: return EPOCANVA_NO_COMMIT;
    case flow::Status::kBufferTooSmall: return EPOCANVA_BUFFER_TOO_SMALL;
    case flow::Status::kTokenExhausted: return EPOCANVA_OUT_OF_RANGE;
    case flow::Status::kWrongThread: return EPOCANVA_WRONG_THREAD;
    default: return EPOCANVA_INVALID_ARGUMENT;
  }
}
learn::Access Access(const epocanva_context& context, std::uint32_t enabled) {
  return {context.privacy_class == EPOCANVA_PRIVACY_NORMAL ? learn::PrivacyClass::kNormal :
      context.privacy_class == EPOCANVA_PRIVACY_SENSITIVE ? learn::PrivacyClass::kSensitive :
      learn::PrivacyClass::kUnknown, enabled != 0, context.context_target, context.generation,
      learn::CurrentTickMilliseconds()};
}
struct Resource {
  std::shared_ptr<const pred::Model> model;
  std::unique_ptr<lex::LexicalService> lexical;
  std::string model_sha256;
  std::string identity;
  std::filesystem::path conversion_path;
  std::unique_ptr<epocanva::japanese_decoder::Decoder> japanese;
  std::shared_ptr<const epocanva::zh::PinyinInitialism> pinyin_initialism;
#ifdef EPOCANVA_BACKEND_FRENCH
  std::shared_ptr<const epocanva::french_dictionary::Dictionary> french;
#endif
  learn::Catalog catalog;
};
struct Profile {
  std::uint32_t id = 0;
  std::uint32_t language = 0;
  std::uint32_t input_mode = 0;
  std::uint32_t output_mode = 0;
  std::uint32_t keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
  std::string variant;
  std::string abbreviation_path;
  std::string abbreviation_sha256;
  std::shared_ptr<const pred::abbreviation::Decoder> abbreviations;
};
struct Backend {
  ~Backend() { if (rime) (void)epocanva_engine_close(rime); }
  std::thread::id owner = std::this_thread::get_id();
  std::map<std::uint32_t, Resource> resources;
  std::map<std::uint32_t, std::shared_ptr<Resource>> profile_resources;
  std::map<std::uint32_t, Profile> profiles;
  bool profiles_only = false;
  std::unique_ptr<learn::WindowsLearningRepository> repository;
  epocanva_engine_handle rime = 0;
  std::uint64_t sessions = 0;
};
struct Session {
  ~Session() { if (rime) (void)epocanva_session_close(rime); }
  Backend* backend = nullptr;
  Resource* resource = nullptr;
  std::unique_ptr<flow::Session> flow;
  epocanva_context identity{};
  std::uint32_t language = 0;
  std::uint32_t mode = 0;
  Profile profile;
  std::uint64_t confirmed_event_token = 0;
  std::vector<pred::LexemeId> selected_context_ids;
  std::string raw;
  std::string preedit;
  epocanva::ja::RomajiComposer japanese;
  epocanva::ko::TwoSetComposer korean;
  epocanva_session_handle rime = 0;
  epocanva_context rime_context{};
  std::uint64_t rime_snapshot = 0;
  std::vector<std::uint32_t> rime_ordinals;
};
struct Registry {
  std::mutex mutex;
  std::uint64_t next = 1;
  std::map<std::uint64_t, std::unique_ptr<Backend>> backends;
  std::map<std::uint64_t, std::unique_ptr<Session>> sessions;
  std::uint64_t Token() {
    if (!next || next == (std::numeric_limits<std::uint64_t>::max)()) return 0;
    return next++;
  }
};
Registry& State() { static auto* registry = new Registry; return *registry; }
epocanva_status Request(const epocanva_backend_request* request) {
  auto status = Structure(request);
  if (status != EPOCANVA_OK) return status;
  if (request->reserved || request->context.reserved || request->learning_enabled > 1)
    return EPOCANVA_INVALID_ARGUMENT;
  return EPOCANVA_OK;
}
template<class F> epocanva_status With(std::uint64_t handle, const epocanva_backend_request* request, F&& fn) {
  auto status = Request(request);
  if (status != EPOCANVA_OK) return status;
  auto& registry = State();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const auto found = registry.sessions.find(handle);
  if (found == registry.sessions.end()) return EPOCANVA_INVALID_HANDLE;
  Session& session = *found->second;
  auto access = Access(request->context, request->learning_enabled);
  status = Result(session.flow->Check(access));
  if (status != EPOCANVA_OK) return status;
  return fn(session, access);
}
template<class T> T Init() {
  T value{};
  value.struct_size_bytes = sizeof(T);
  value.abi_version = EPOCANVA_ENGINE_ABI_VERSION;
  return value;
}
epocanva_session_request RimeRequest(const Session& session) {
  auto request = Init<epocanva_session_request>(); request.context = session.rime_context; return request;
}
epocanva_status RimeState(Session& session, epocanva_snapshot* snapshot, std::string* payload) {
  *snapshot = Init<epocanva_snapshot>();
  auto request = RimeRequest(session);
  auto status = epocanva_session_read_snapshot(session.rime, &request, snapshot, nullptr, 0);
  if (status != EPOCANVA_OK && status != EPOCANVA_BUFFER_TOO_SMALL) return status;
  payload->resize(static_cast<std::size_t>(snapshot->required_utf8_bytes));
  status = epocanva_session_read_snapshot(session.rime, &request, snapshot, payload->data(), payload->size());
  if (status == EPOCANVA_OK) session.rime_snapshot = snapshot->snapshot_version;
  return status;
}
std::string Slice(const std::string& bytes, epocanva_utf8_span span) {
  if (span.utf8_byte_offset > bytes.size() || span.utf8_byte_count > bytes.size() - span.utf8_byte_offset)
    throw std::runtime_error("invalid-backend-span");
  return bytes.substr(static_cast<std::size_t>(span.utf8_byte_offset), static_cast<std::size_t>(span.utf8_byte_count));
}
epocanva_status RimeReset(Session& session) {
  auto request = RimeRequest(session);
  auto result = Init<epocanva_operation_result>();
  auto status = epocanva_session_cancel(session.rime, &request, &result);
  if (status == EPOCANVA_OK) session.rime_context.generation = result.generation;
  return status;
}
std::string KanaScript(std::string_view text, bool katakana) {
  std::string result;
  for (std::size_t begin_utf8_byte = 0; begin_utf8_byte < text.size();) {
    auto end_utf8_byte = begin_utf8_byte + 1;
    while (end_utf8_byte < text.size() && (static_cast<unsigned char>(text[end_utf8_byte]) & 0xc0) == 0x80) ++end_utf8_byte;
    if (end_utf8_byte - begin_utf8_byte == 3) {
      const auto scalar = ((static_cast<unsigned char>(text[begin_utf8_byte]) & 15u) << 12) |
          ((static_cast<unsigned char>(text[begin_utf8_byte + 1]) & 63u) << 6) |
          (static_cast<unsigned char>(text[begin_utf8_byte + 2]) & 63u);
      const auto mapped = katakana && scalar >= 0x3041 && scalar <= 0x3096 ? scalar + 0x60 :
          !katakana && scalar >= 0x30a1 && scalar <= 0x30f6 ? scalar - 0x60 : scalar;
      result.push_back(static_cast<char>(0xe0u | (mapped >> 12)));
      result.push_back(static_cast<char>(0x80u | ((mapped >> 6) & 63u)));
      result.push_back(static_cast<char>(0x80u | (mapped & 63u)));
    } else result.append(text.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte));
    begin_utf8_byte = end_utf8_byte;
  }
  return result;
}
std::string JapaneseReading(Session& session) {
  auto composer = session.japanese;
  return KanaScript(session.mode == EPOCANVA_BACKEND_KANA ? session.raw : composer.CommitUtf8(), false);
}
epocanva_status Compose(Session& session, const std::string& raw, const learn::Access& access) {
  flow::Commit pending;
  if (session.flow->ReadCommit(access, &pending) == flow::Status::kOk) return EPOCANVA_BUSY;
  std::string preedit = raw;
  if (session.language == EPOCANVA_BACKEND_JA && session.mode == EPOCANVA_BACKEND_KANA) {
    for (std::size_t byte = 0; byte < raw.size();) {
      if (raw[byte] == ' ') { ++byte; continue; }
      if (byte + 3 > raw.size() || static_cast<unsigned char>(raw[byte]) != 0xe3) return EPOCANVA_INVALID_ARGUMENT;
      const auto scalar = ((static_cast<unsigned char>(raw[byte]) & 15u) << 12) |
          ((static_cast<unsigned char>(raw[byte + 1]) & 63u) << 6) | (static_cast<unsigned char>(raw[byte + 2]) & 63u);
      if (scalar < 0x3041 || scalar > 0x30ff) return EPOCANVA_INVALID_ARGUMENT;
      byte += 3;
    }
    preedit = KanaScript(raw, session.profile.output_mode == EPOCANVA_BACKEND_OUTPUT_KATAKANA);
  } else if (session.language == EPOCANVA_BACKEND_JA) {
    auto composer = epocanva::ja::RomajiComposer((!session.profile.id && session.mode == EPOCANVA_BACKEND_KATAKANA) ||
        session.profile.output_mode == EPOCANVA_BACKEND_OUTPUT_KATAKANA ?
        epocanva::ja::KanaMode::kKatakana : epocanva::ja::KanaMode::kHiragana);
    for (char byte : raw) if (composer.PushAscii(byte) != epocanva::ja::InputResult::kAccepted) return EPOCANVA_INVALID_ARGUMENT;
    preedit = composer.GetPreedit().text_utf8;
    session.japanese = std::move(composer);
  } else if (session.language == EPOCANVA_BACKEND_KO && session.mode == EPOCANVA_BACKEND_TWO_SET) {
    epocanva::ko::TwoSetComposer composer;
    for (char byte : raw) if (composer.PushAscii(byte) != epocanva::ko::InputResult::kAccepted) return EPOCANVA_INVALID_ARGUMENT;
    preedit = composer.GetPreedit().text_utf8;
    session.korean = std::move(composer);
  } else if (session.language == EPOCANVA_BACKEND_ZH) {
    for (unsigned char byte : raw) if (byte < 0x20 || byte >= 0x7f) return EPOCANVA_INVALID_ARGUMENT;
    auto status = RimeReset(session);
    if (status != EPOCANVA_OK) return status;
    for (unsigned char byte : raw) {
      auto request = Init<epocanva_key_request>(); request.context = session.rime_context; request.keysym = byte;
      auto result = Init<epocanva_operation_result>();
      status = epocanva_session_process_key(session.rime, &request, &result);
      if (status != EPOCANVA_OK || !result.backend_reported_handled || result.commit_pending) {
        RimeReset(session);
        return status == EPOCANVA_OK ? EPOCANVA_BACKEND_ERROR : status;
      }
    }
    auto snapshot = Init<epocanva_snapshot>(); std::string bytes;
    status = RimeState(session, &snapshot, &bytes);
    if (status != EPOCANVA_OK) return status;
    preedit = Slice(bytes, snapshot.preedit);
  }
  auto status = Result(session.flow->SetInput(raw, preedit, access));
  if (status != EPOCANVA_OK) return status;
  session.raw = raw; session.preedit = std::move(preedit);
  return EPOCANVA_OK;
}
std::string Join(const std::vector<std::string>& tokens, std::uint32_t language) {
  std::string result;
  for (const auto& token : tokens) {
    const bool punctuation = token.size() == 1 && std::string(".,!?;:").find(token[0]) != std::string::npos;
    if (!result.empty() && language != EPOCANVA_BACKEND_ZH && language != EPOCANVA_BACKEND_JA && !punctuation)
      result.push_back(' ');
    result += token;
  }
  return result;
}
std::uint32_t Kind(lex::CandidateType type) {
  switch (type) {
    case lex::CandidateType::kOriginal: return EPOCANVA_BACKEND_RAW;
    case lex::CandidateType::kSpelling: return EPOCANVA_BACKEND_SPELLING;
    case lex::CandidateType::kMorphology: return EPOCANVA_BACKEND_MORPHOLOGY;
    case lex::CandidateType::kAbbreviation:
    case lex::CandidateType::kContraction: return EPOCANVA_BACKEND_EXPANSION;
    default: return EPOCANVA_BACKEND_WORD;
  }
}
double CandidateModelScore(Session& session, const std::vector<pred::LexemeId>& ids,
                           pred::Context previous, const pred::PersonalCounts* counts) {
  if (ids.empty()) return std::log(1.0 / (static_cast<double>(session.resource->model->metadata().unigram_total) +
      static_cast<double>(session.resource->model->metadata().lexeme_count) + 1.0));
  double score = 0;
  for (const auto id : ids) {
    const auto scored = session.resource->model->Score(previous, id, counts);
    if (scored.status != pred::Status::kOk) return -1.0e100;
    score += scored.log_probability;
    previous.push_back(id); if (previous.size() > 2) previous.erase(previous.begin());
  }
  return score;
}
void MergeCandidate(std::vector<flow::Candidate>* candidates, flow::Candidate candidate) {
  const auto same = std::find_if(candidates->begin(), candidates->end(), [&](const auto& value) {
    return value.kind != EPOCANVA_BACKEND_RAW && value.text_utf8 == candidate.text_utf8;
  });
  if (same == candidates->end()) candidates->push_back(std::move(candidate));
  else {
    const bool exact = same->exact_expansion || candidate.exact_expansion;
    if (candidate.score > same->score || (candidate.score == same->score && candidate.lexeme_ids < same->lexeme_ids))
      *same = std::move(candidate);
    same->exact_expansion = exact;
  }
}
rk::SourceKind RankSource(std::uint32_t kind) {
  switch (kind) {
    case EPOCANVA_BACKEND_SPELLING: return rk::SourceKind::kSpelling;
    case EPOCANVA_BACKEND_MORPHOLOGY: return rk::SourceKind::kMorphology;
    case EPOCANVA_BACKEND_EXPANSION: return rk::SourceKind::kAbbreviation;
    case EPOCANVA_BACKEND_CONVERSION: return rk::SourceKind::kConversion;
    case EPOCANVA_BACKEND_PREDICTION: return rk::SourceKind::kPrediction;
    case EPOCANVA_BACKEND_RAW: return rk::SourceKind::kOriginal;
    default: return rk::SourceKind::kCompletion;
  }
}
// A bounded personal re-rank of candidates the backend already produced. The backend's
// order is the prior (normalised to [0,1] by position, not by the raw model log-probability,
// so unrelated score scales are never added together). Stored preferences can move a
// candidate within that order; they cannot displace the raw candidate, which is kept out
// of the ranking pass and prepended unchanged. A missing/corrupt/empty preference set is
// a no-op, so the model-free path is unchanged.
void ApplyPersonalRanking(Session& session,
                          const std::vector<learn::PreferenceRecord>& preferences,
                          std::vector<flow::Candidate>* candidates) {
  if (preferences.empty() || candidates == nullptr || candidates->size() < 2) return;
  if (session.raw.size() > rk::kMaxInputKeyUtf8Bytes) return;
  const char* language = Language(session.language);
  if (language == nullptr) return;
  const std::size_t first =
      candidates->front().kind == EPOCANVA_BACKEND_RAW ? 1u : 0u;
  // The backend contract puts the raw candidate first. If a future source violates that,
  // skip the personal pass rather than risk displacing the user's own text.
  for (std::size_t ordinal = first; ordinal < candidates->size(); ++ordinal)
    if ((*candidates)[ordinal].kind == EPOCANVA_BACKEND_RAW) return;
  const std::size_t remaining = candidates->size() - first;
  if (remaining < 2) return;

  rk::LearningEventModel learning;
  for (const auto& row : preferences) {
    if (row.candidate_id.empty()) continue;
    rk::Preference value;
    value.positive = static_cast<double>(row.positive_units) / 1000.0;
    value.negative = static_cast<double>(row.negative_units) / 1000.0;
    value.last_tick = row.last_tick_ms;
    value.manual = row.manual;
    (void)learning.SetPreference(language, row.candidate_id, value);
  }
  if (learning.size() == 0) return;

  std::vector<rk::Proposal> proposals;
  proposals.reserve(remaining);
  for (std::size_t ordinal = first; ordinal < candidates->size(); ++ordinal) {
    const auto& candidate = (*candidates)[ordinal];
    if (candidate.text_utf8.empty() ||
        candidate.text_utf8.size() > rk::kMaxDisplayTextUtf8Bytes) return;
    rk::Proposal proposal;
    proposal.text_utf8 = candidate.text_utf8;
    proposal.language = language;
    if (!candidate.lexeme_ids.empty())
      proposal.candidate_id = learn::CandidateKey(candidate.lexeme_ids);
    proposal.static_weight = remaining <= 1 ? 1.0 :
        1.0 - static_cast<double>(ordinal - first) /
                  static_cast<double>(remaining - 1);
    proposal.context_logp = 0.0;  // the backend order is the prior; no second score scale
    proposal.source = RankSource(candidate.kind);
    proposals.push_back(std::move(proposal));
  }

  rk::RankRequest request;
  request.user_language = language;
  request.input_key_utf8 = session.raw;
  request.generation = session.identity.generation;
  request.proposals = std::move(proposals);
  request.weights.prefix = 0.0;
  request.weights.lexical = 1.0;
  request.weights.context = 0.0;
  // Engineering default, not a corpus-calibrated weight. The backend order is a strong
  // prior; one selection adds +0.06 point, so a real context/model gap (for example
  // "good" -> "morning") is not overturned by a single click. Repeated selections and
  // explicit manual words can still move a candidate. This is the same honesty rule as
  // AdaptiveRanker's DefaultWeights: replace through offline evaluation before claiming
  // calibrated ranking.
  request.weights.personal = 0.25;
  request.weights.negative = 0.25;
  request.weights.source = 0.0;
  request.weights.language = 0.0;
  request.learning_enabled = true;  // read existing preferences even while recording is paused
  rk::AdaptiveRanker ranker;
  const rk::RankResult result = ranker.Rank(request, &learning);
  if (result.status != rk::RankStatus::kOk || result.ordered.size() != remaining) return;

  std::vector<flow::Candidate> ordered;
  ordered.reserve(candidates->size());
  if (first == 1) ordered.push_back(std::move(candidates->front()));
  for (const auto& item : result.ordered)
    ordered.push_back(std::move((*candidates)[first + item.proposal_ordinal]));
  *candidates = std::move(ordered);
}
void Personal(Session& session, const learn::Access& access, pred::PersonalCounts* counts,
              lex::RankingInputs* ranking, std::vector<lex::LearnedCount>* words,
              std::vector<lex::ContextCount>* pairs,
              std::vector<learn::PreferenceRecord>* preferences) {
  learn::Snapshot snapshot;
  if (session.backend->repository) {
    auto status = session.backend->repository->LoadSnapshot(session.flow->catalog(), [access] { return access; }, &snapshot);
    if (status != learn::Status::kOk) return;
    session.flow->ImportLearning(snapshot, access);
  } else if (session.flow->ReadLearning(access, &snapshot) != learn::Status::kOk) return;
  for (auto row : snapshot.words) { counts->unigrams.push_back({row.id,row.count}); words->push_back({row.id,row.count}); }
  for (auto row : snapshot.pairs) { counts->bigrams.push_back({row.previous_id,row.id,row.count}); pairs->push_back({row.previous_id,row.id,row.count}); }
  if (preferences != nullptr) *preferences = snapshot.preferences;
  ranking->allow_learning = true; ranking->learned = words; ranking->context = pairs;
  const auto context = session.flow->Context(access);
  ranking->previous_id = context.empty() ? 0 : context.back();
}
std::string ResourceIdentity(const epocanva_backend_resource& value) {
  std::string identity = std::to_string(value.language);
  for (const auto view : {value.model_path, value.model_sha256, value.conversion_path,
                         value.conversion_sha256, value.auxiliary_path, value.auxiliary_sha256}) {
    const auto text = Text(view, 30000);
    identity += ":" + std::to_string(text.size()) + ":" + text;
  }
  return identity;
}
epocanva_status LoadResource(const epocanva_backend_resource& input, bool fixtures, Resource* output) {
  const auto status = Structure(&input);
  if (status != EPOCANVA_OK) return status;
  if (!Language(input.language) || input.reserved) return EPOCANVA_INVALID_ARGUMENT;
  Resource resource;
  resource.identity = ResourceIdentity(input);
  auto path = std::filesystem::u8path(Text(input.model_path, 30000));
  epocanva::backend::LocalResource held;
  if (!held.Open(path)) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  resource.model_sha256 = Text(input.model_sha256, 64);
  if (!Digest(held.path(), resource.model_sha256)) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  pred::LoadOptions loading; loading.allow_test_fixture = fixtures;
  resource.model = pred::Model::Load(held.path(), loading);
  if (!resource.model || !resource.model->available()) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  resource.catalog.name_space = std::string("backend.") + Language(input.language) + ".v1";
  resource.catalog.sha256 = resource.model_sha256;
  for (const auto& row : resource.model->lexemes())
    if (row.language_id == Language(input.language)) resource.catalog.admitted_ids.push_back(row.id);
  if (resource.catalog.admitted_ids.empty()) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  if (input.language == EPOCANVA_BACKEND_EN || input.language == EPOCANVA_BACKEND_FR || input.language == EPOCANVA_BACKEND_KO) {
    resource.lexical = lex::LexicalService::Create(*resource.model);
    if (!resource.lexical) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  }
  if (input.conversion_path.utf8_byte_count) {
    if (input.language != EPOCANVA_BACKEND_JA && input.language != EPOCANVA_BACKEND_FR &&
        input.language != EPOCANVA_BACKEND_ZH)
      return EPOCANVA_INVALID_ARGUMENT;
    resource.conversion_path = std::filesystem::u8path(Text(input.conversion_path, 30000));
  }
  if (input.language == EPOCANVA_BACKEND_ZH && !resource.conversion_path.empty()) {
    // The pinyin dictionary is the same local Terra file Rime consumes. It is
    // pinned by the profile resource hash; the decoder only composes characters
    // that already have an admitted model ID.
    epocanva::backend::LocalResource dictionary;
    if (!dictionary.Open(resource.conversion_path) ||
        !Digest(dictionary.path(), Text(input.conversion_sha256, 64)))
      return EPOCANVA_BACKEND_INVALID_RESOURCE;
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(dictionary.path(), size_error);
    if (size_error || bytes == 0 || bytes > 16ull * 1024 * 1024)
      return EPOCANVA_BACKEND_INVALID_RESOURCE;
    std::string source(static_cast<std::size_t>(bytes), '\0');
    std::ifstream dictionary_stream(dictionary.path(), std::ios::binary);
    if (!dictionary_stream.read(source.data(), static_cast<std::streamsize>(source.size())))
      return EPOCANVA_BACKEND_INVALID_RESOURCE;
    std::string diagnostic;
    resource.pinyin_initialism = epocanva::zh::PinyinInitialism::Build(
        source, *resource.model, &diagnostic);
    if (!resource.pinyin_initialism) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  }
  if (input.language == EPOCANVA_BACKEND_JA && !resource.conversion_path.empty()) {
    resource.japanese = std::make_unique<epocanva::japanese_decoder::Decoder>();
    epocanva::japanese_decoder::LoadOptions options;
    options.expected_sha256 = Text(input.conversion_sha256, 64);
    if (resource.japanese->Load(resource.conversion_path.u8string(), options) != epocanva::japanese_decoder::Status::kOk)
      return EPOCANVA_BACKEND_INVALID_RESOURCE;
  }
  if (input.language == EPOCANVA_BACKEND_FR && !resource.conversion_path.empty()) {
#ifdef EPOCANVA_BACKEND_FRENCH
    epocanva::french_dictionary::ExpectedHashes expected;
    expected.aff_sha256 = Text(input.conversion_sha256, 64);
    expected.dic_sha256 = Text(input.auxiliary_sha256, 64);
    resource.french = epocanva::french_dictionary::Dictionary::Load(resource.conversion_path,
        std::filesystem::u8path(Text(input.auxiliary_path, 30000)), expected);
    if (!resource.french) return EPOCANVA_BACKEND_INVALID_RESOURCE;
#else
    return EPOCANVA_BACKEND_UNAVAILABLE;
#endif
  }
  *output = std::move(resource);
  return EPOCANVA_OK;
}
bool ModeValid(std::uint32_t language, std::uint32_t mode) {
  return ((language == EPOCANVA_BACKEND_EN || language == EPOCANVA_BACKEND_FR) && mode == EPOCANVA_BACKEND_LATIN) ||
      (language == EPOCANVA_BACKEND_ZH && (mode == EPOCANVA_BACKEND_PINYIN || mode == EPOCANVA_BACKEND_ZHUYIN)) ||
      (language == EPOCANVA_BACKEND_JA && (mode == EPOCANVA_BACKEND_ROMAJI || mode == EPOCANVA_BACKEND_KATAKANA || mode == EPOCANVA_BACKEND_KANA)) ||
      (language == EPOCANVA_BACKEND_KO && (mode == EPOCANVA_BACKEND_LATIN || mode == EPOCANVA_BACKEND_TWO_SET));
}
epocanva_status ReadProfile(const epocanva_backend_profile* value, Profile* out) {
  const auto status = Structure(value, EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  if (status != EPOCANVA_OK) return status;
  if (!value->profile_id || value->reserved || !ModeValid(value->language, value->input_mode) ||
      value->keyboard_layout > EPOCANVA_BACKEND_KEYBOARD_DVORAK) return EPOCANVA_BACKEND_INVALID_PROFILE;
  const auto variant = Text(value->variant_id, 64);
  const auto language = value->language;
  const auto output = value->output_mode;
  bool admitted = false;
  if (language == EPOCANVA_BACKEND_EN)
    admitted = (variant == "en" || variant == "en-US" || variant == "en-GB") && output == EPOCANVA_BACKEND_OUTPUT_LATIN;
  if (language == EPOCANVA_BACKEND_FR)
    admitted = (variant == "fr" || variant == "fr-FR") && output == EPOCANVA_BACKEND_OUTPUT_LATIN;
  if (language == EPOCANVA_BACKEND_ZH)
    admitted = (variant == "zh" || (variant == "zh-pinyin" && value->input_mode == EPOCANVA_BACKEND_PINYIN) ||
        (variant == "zh-zhuyin" && value->input_mode == EPOCANVA_BACKEND_ZHUYIN)) && output == EPOCANVA_BACKEND_OUTPUT_HANZI;
  if (language == EPOCANVA_BACKEND_JA)
    admitted = (variant == "ja" || variant == "ja-JP") && output >= EPOCANVA_BACKEND_OUTPUT_HIRAGANA && output <= EPOCANVA_BACKEND_OUTPUT_KANJI;
  if (language == EPOCANVA_BACKEND_KO)
    admitted = (variant == "ko" || variant == "ko-KR") && output == EPOCANVA_BACKEND_OUTPUT_HANGUL;
  if (!admitted) return EPOCANVA_BACKEND_UNAVAILABLE;
  out->id = value->profile_id; out->language = language; out->input_mode = value->input_mode;
  out->output_mode = output; out->keyboard_layout = value->keyboard_layout; out->variant = variant;
  out->abbreviation_path = Text(value->abbreviation_path, 30000);
  out->abbreviation_sha256 = Text(value->abbreviation_sha256, 64);
  if (out->abbreviation_path.empty() != out->abbreviation_sha256.empty()) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  return EPOCANVA_OK;
}
bool SameProfile(const Profile& a, const Profile& b) {
  return a.id == b.id && a.language == b.language && a.input_mode == b.input_mode &&
      a.output_mode == b.output_mode && a.keyboard_layout == b.keyboard_layout && a.variant == b.variant &&
      a.abbreviation_path == b.abbreviation_path && a.abbreviation_sha256 == b.abbreviation_sha256;
}
epocanva_status PrepareProfile(Profile* profile, const Resource& resource) {
  std::string source;
  if (!profile->abbreviation_path.empty()) {
    epocanva::backend::LocalResource held;
    if (!held.Open(std::filesystem::u8path(profile->abbreviation_path)) ||
        !Digest(held.path(), profile->abbreviation_sha256)) return EPOCANVA_BACKEND_INVALID_RESOURCE;
    const auto bytes = std::filesystem::file_size(held.path());
    if (bytes > pred::abbreviation::kMaxSourceUtf8Bytes) return EPOCANVA_BACKEND_INVALID_RESOURCE;
    source.resize(static_cast<std::size_t>(bytes));
    std::ifstream file(held.path(), std::ios::binary);
    if (!file.read(source.data(), static_cast<std::streamsize>(bytes))) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  }
  profile->abbreviations = pred::abbreviation::Decoder::Build(resource.model, source);
  return profile->abbreviations ? EPOCANVA_OK : EPOCANVA_BACKEND_INVALID_RESOURCE;
}
epocanva_status RegisterSession(Backend& backend, Resource& resource, Profile profile,
                                const epocanva_backend_session_options& options,
                                epocanva_backend_session_handle* out) {
  if (options.context.privacy_class != EPOCANVA_PRIVACY_NORMAL) return EPOCANVA_PRIVACY_DENIED;
  if (options.reserved || options.context.reserved || options.learning_enabled > 1 ||
      !options.context.context_target || !options.context.generation) return EPOCANVA_INVALID_ARGUMENT;
  if (!ModeValid(options.language, options.input_mode)) return EPOCANVA_INVALID_ARGUMENT;
  if (backend.owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
  if (options.language == EPOCANVA_BACKEND_ZH && !backend.rime) return EPOCANVA_BACKEND_UNAVAILABLE;
  if (profile.id && profile.language == EPOCANVA_BACKEND_JA &&
      profile.output_mode == EPOCANVA_BACKEND_OUTPUT_KANJI && !resource.japanese) return EPOCANVA_BACKEND_UNAVAILABLE;
  auto session = std::make_unique<Session>();
  session->backend = &backend; session->resource = &resource; session->profile = std::move(profile);
  session->language = options.language; session->mode = options.input_mode; session->identity = options.context;
  // Learning is scoped by profile, not merely by language. The short primary subtag stays
  // in Catalog::language; the numeric product profile identity gets its own file namespace
  // and header field. A full BCP 47 tag is never stored in the language slot.
  auto catalog = resource.catalog;
  catalog.language = Language(options.language);
  catalog.profile_id = session->profile.id ? std::to_string(session->profile.id) : "legacy";
  catalog.name_space = "backend." + catalog.language +
      (session->profile.id ? ".p" + catalog.profile_id : ".legacy") + ".v1";
  session->flow = flow::Session::Create(catalog, resource.model_sha256, Access(options.context, options.learning_enabled));
  if (!session->flow) return EPOCANVA_BACKEND_INVALID_RESOURCE;
  if (options.language == EPOCANVA_BACKEND_ZH) {
    auto request = Init<epocanva_session_options>(); request.context = options.context;
    const std::string schema = options.input_mode == EPOCANVA_BACKEND_ZHUYIN ? "epocanva_backend_zhuyin" : "epocanva_backend_pinyin";
    request.schema_id = {schema.data(), schema.size()};
    const auto status = epocanva_session_open(backend.rime, &request, &session->rime);
    if (status != EPOCANVA_OK) return status;
    session->rime_context = request.context;
  }
  const auto token = State().Token();
  if (!token) return EPOCANVA_OUT_OF_RANGE;
  State().sessions.emplace(token, std::move(session)); ++backend.sessions; *out = token;
  return EPOCANVA_OK;
}
epocanva_status Copy(std::initializer_list<std::string_view> fields, char* buffer, std::uint64_t capacity) {
  if (!buffer && capacity) return EPOCANVA_INVALID_ARGUMENT;
  std::uint64_t required = 0;
  for (auto field : fields) required += field.size();
  if (required > capacity) return EPOCANVA_BUFFER_TOO_SMALL;
  std::size_t offset_utf8_byte = 0;
  for (auto field : fields) if (!field.empty()) { std::memcpy(buffer + offset_utf8_byte, field.data(), field.size()); offset_utf8_byte += field.size(); }
  return EPOCANVA_OK;
}
}  // namespace

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_open(
    const epocanva_backend_options* options, epocanva_backend_handle* out) noexcept {
  return Guard([&]() -> epocanva_status {
    if (!out) return EPOCANVA_INVALID_ARGUMENT;
    *out = 0; auto status = Structure(options);
    if (status != EPOCANVA_OK) return status;
    if (!options->resources || options->resource_count == 0 || options->resource_count > 5 || options->allow_test_fixture > 1)
      return EPOCANVA_INVALID_ARGUMENT;
    auto backend = std::make_unique<Backend>();
    for (std::uint32_t ordinal = 0; ordinal < options->resource_count; ++ordinal) {
      const auto& input = options->resources[ordinal];
      status = Structure(&input);
      if (status != EPOCANVA_OK) return status;
      if (!Language(input.language) || input.reserved || backend->resources.count(input.language)) return EPOCANVA_INVALID_ARGUMENT;
      Resource resource;
      status = LoadResource(input, options->allow_test_fixture != 0, &resource);
      if (status != EPOCANVA_OK) return status;
      backend->resources.emplace(input.language, std::move(resource));
    }
    const auto learning_root = Text(options->learning_root, 30000);
    if (!learning_root.empty()) backend->repository = std::make_unique<learn::WindowsLearningRepository>(std::filesystem::u8path(learning_root));
    if (options->rime_options) {
      status = epocanva_engine_open(options->rime_options, &backend->rime);
      if (status != EPOCANVA_OK) return status;
    }
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    const auto token = registry.Token();
    if (!token) { return EPOCANVA_OUT_OF_RANGE; }
    registry.backends.emplace(token, std::move(backend)); *out = token;
    return EPOCANVA_OK;
  });
}
#include "profile_api.inc"

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_close(epocanva_backend_handle handle) noexcept {
  return Guard([&]() -> epocanva_status {
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.backends.find(handle); if (found == registry.backends.end()) return EPOCANVA_INVALID_HANDLE;
    if (found->second->owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
    if (found->second->sessions) return EPOCANVA_BUSY;
    if (found->second->rime) { auto status = epocanva_engine_close(found->second->rime); if (status != EPOCANVA_OK) return status; found->second->rime = 0; }
    registry.backends.erase(found); return EPOCANVA_OK;
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_session_open(
    epocanva_backend_handle handle, const epocanva_backend_session_options* options,
    epocanva_backend_session_handle* out) noexcept {
  return Guard([&]() -> epocanva_status {
    if (!out) return EPOCANVA_INVALID_ARGUMENT;
    *out = 0; auto status = Structure(options); if (status != EPOCANVA_OK) return status;
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.backends.find(handle); if (found == registry.backends.end()) return EPOCANVA_INVALID_HANDLE;
    auto& backend = *found->second;
    if (backend.profiles_only) return EPOCANVA_BACKEND_INVALID_PROFILE;
    auto resource = backend.resources.find(options->language);
    if (resource == backend.resources.end()) return EPOCANVA_BACKEND_UNAVAILABLE;
    Profile profile;
    profile.language = options->language; profile.input_mode = options->input_mode;
    return RegisterSession(backend, resource->second, std::move(profile), *options, out);
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_session_close(epocanva_backend_session_handle handle) noexcept {
  return Guard([&]() -> epocanva_status {
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.sessions.find(handle); if (found == registry.sessions.end()) return EPOCANVA_INVALID_HANDLE;
    if (found->second->backend->owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
    if (found->second->rime) { auto status = epocanva_session_close(found->second->rime); if (status != EPOCANVA_OK) return status; found->second->rime = 0; }
    --found->second->backend->sessions; registry.sessions.erase(found); return EPOCANVA_OK;
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_set_input(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request, epocanva_utf8_view input) noexcept {
  return Guard([&] { return With(handle, request, [&](Session& session, const learn::Access& access) {
    return Compose(session, Text(input), access);
  }); });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_push_ascii(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request, std::uint32_t byte) noexcept {
  return Guard([&] { return With(handle, request, [&](Session& session, const learn::Access& access) -> epocanva_status {
    if (byte < 0x20 || byte >= 0x7f || session.raw.size() >= kMaxInputBytes) return EPOCANVA_INVALID_ARGUMENT;
    return Compose(session, session.raw + static_cast<char>(byte), access);
  }); });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_backspace(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request) noexcept {
  return Guard([&] { return With(handle, request, [&](Session& session, const learn::Access& access) {
    auto raw = session.raw;
    if (!raw.empty()) { auto begin_utf8_byte = raw.size()-1; while (begin_utf8_byte && (static_cast<unsigned char>(raw[begin_utf8_byte])&0xc0)==0x80) --begin_utf8_byte; raw.resize(begin_utf8_byte); }
    return Compose(session, raw, access);
  }); });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_cancel(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status = Request(request); if (status != EPOCANVA_OK) return status;
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.sessions.find(handle); if (found == registry.sessions.end()) return EPOCANVA_INVALID_HANDLE;
    auto& session = *found->second;
    status = Result(session.flow->Reset(Access(request->context, request->learning_enabled)));
    if (status != EPOCANVA_OK) return status;
    session.raw.clear(); session.preedit.clear(); session.japanese.Cancel(); session.korean.Cancel();
    if (session.rime) return RimeReset(session);
    return EPOCANVA_OK;
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_query_candidates(
    epocanva_backend_session_handle handle, const epocanva_backend_query* query) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status = Structure(query); if (status != EPOCANVA_OK) return status;
    if (!query->max_candidates || query->max_candidates > 64 ||
        query->query_kind < EPOCANVA_BACKEND_COMPLETION || query->query_kind > EPOCANVA_BACKEND_NEXT_UNIT ||
        !query->max_continuation_words || query->max_continuation_words > 4 || query->search_budget > 10000000)
      return EPOCANVA_INVALID_ARGUMENT;
    auto request = Init<epocanva_backend_request>(); request.context = query->context; request.learning_enabled = query->learning_enabled;
    return With(handle, &request, [&](Session& session, const learn::Access& access) -> epocanva_status {
      flow::Commit pending; if (session.flow->ReadCommit(access, &pending) == flow::Status::kOk) return EPOCANVA_BUSY;
      pred::PersonalCounts personal; lex::RankingInputs ranking;
      std::vector<lex::LearnedCount> words; std::vector<lex::ContextCount> pairs;
      std::vector<learn::PreferenceRecord> preferences;
      Personal(session, access, &personal, &ranking, &words, &pairs, &preferences);
      const auto context = session.flow->Context(access);
      // Existing preferences participate even while recording is paused. An empty set is
      // the model-free path and leaves the backend's order untouched.
      const auto* counts =
          personal.unigrams.empty() && personal.bigrams.empty() ? nullptr : &personal;
      std::vector<flow::Candidate> candidates;
      bool complete = true;
      session.rime_ordinals.clear();
      if (query->query_kind != EPOCANVA_BACKEND_COMPLETION) {
        if (!session.raw.empty()) return EPOCANVA_INVALID_ARGUMENT;
        if (session.profile.id && session.language == EPOCANVA_BACKEND_JA &&
            session.profile.output_mode != EPOCANVA_BACKEND_OUTPUT_KANJI) return EPOCANVA_BACKEND_UNAVAILABLE;
        pred::PredictOptions options;
        options.language_id = Language(session.language); options.max_suggestions = query->max_candidates;
        // NEXT_WORD is a word prediction, not a generic lexical-token prediction. The
        // model ranks punctuation as ordinary tokens, so asking for only max_candidates
        // and then dropping punctuation can leave a short or empty menu. Fetch the
        // documented model maximum and stop once enough words are collected; NEXT_UNIT
        // still returns punctuation because that is a valid unit there.
        const bool words_only = query->query_kind == EPOCANVA_BACKEND_NEXT_WORD;
        if (words_only) options.max_suggestions = pred::kMaxSuggestions;
        options.max_words = query->query_kind == EPOCANVA_BACKEND_CONTINUATION ? query->max_continuation_words : 1;
        options.max_index_entries = static_cast<std::size_t>(query->search_budget);
        const auto predicted = session.resource->model->Predict(std::nullopt, context, options, counts);
        if (predicted.status != pred::Status::kOk && predicted.status != pred::Status::kBudgetExhausted &&
            predicted.status != pred::Status::kBeamPruned) return EPOCANVA_BACKEND_UNAVAILABLE;
        complete = predicted.complete;
        for (const auto& row : predicted.suggestions) {
          if (row.lexeme_ids.empty()) continue;
          if (words_only && row.terminal) continue;
          if (candidates.size() >= query->max_candidates) break;
          flow::Candidate candidate;
          candidate.kind = EPOCANVA_BACKEND_PREDICTION; candidate.text_utf8 = Join(row.tokens_utf8, session.language);
          candidate.lexeme_ids = row.lexeme_ids; candidate.score = row.log_probability;
          candidate.input_end_utf8_byte = session.raw.size();
          candidates.push_back(std::move(candidate));
        }
      } else if (session.language == EPOCANVA_BACKEND_ZH) {
        epocanva_snapshot snapshot{}; std::string bytes;
        status = RimeState(session, &snapshot, &bytes); if (status != EPOCANVA_OK) return status;
        if (!session.raw.empty()) {
          flow::Candidate original; original.text_utf8 = session.raw; original.input_end_utf8_byte = session.raw.size();
          candidates.push_back(std::move(original)); session.rime_ordinals.push_back(UINT32_MAX);
        }
        for (std::uint32_t ordinal = 0; ordinal < snapshot.candidate_count_on_page && candidates.size() < query->max_candidates; ++ordinal) {
          auto wanted = Init<epocanva_candidate_request>(); wanted.context = session.rime_context;
          wanted.snapshot_version = snapshot.snapshot_version; wanted.candidate_ordinal_on_page = ordinal;
          auto result = Init<epocanva_candidate>();
          status = epocanva_session_read_candidate(session.rime, &wanted, &result, nullptr, 0);
          if (status != EPOCANVA_OK && status != EPOCANVA_BUFFER_TOO_SMALL) return status;
          std::string buffer(static_cast<std::size_t>(result.required_utf8_bytes), '\0');
          status = epocanva_session_read_candidate(session.rime, &wanted, &result, buffer.data(), buffer.size());
          if (status != EPOCANVA_OK) return status;
          flow::Candidate candidate; candidate.text_utf8 = Slice(buffer, result.text);
          candidate.kind = EPOCANVA_BACKEND_CONVERSION;
          std::vector<pred::LexemeId> context_ids; bool all_known = false;
          (void)epocanva::backend::ResolveContext(*session.resource->model, "zh", candidate.text_utf8,
              {}, &context_ids, &all_known);
          const double context_score = all_known ? CandidateModelScore(session, context_ids, context, counts) :
              CandidateModelScore(session, {}, context, counts);
          candidate.score = -0.05 * static_cast<double>(ordinal) + context_score;
          candidate.input_end_utf8_byte = session.raw.size();
          // Rime menu text can inform transient ranking, not learning identity.
          candidates.push_back(std::move(candidate)); session.rime_ordinals.push_back(ordinal);
        }
        if (session.resource->pinyin_initialism && !session.raw.empty()) {
          const auto initials = session.resource->pinyin_initialism->Query(
              session.raw, context, counts,
              std::min<std::size_t>(query->max_candidates, 8));
          for (const auto& row : initials) {
            if (candidates.size() >= query->max_candidates) break;
            const bool duplicate = std::any_of(candidates.begin(), candidates.end(),
                [&](const flow::Candidate& prior) { return prior.text_utf8 == row.text_utf8; });
            if (duplicate) continue;
            flow::Candidate candidate;
            candidate.kind = EPOCANVA_BACKEND_CONVERSION;
            candidate.text_utf8 = row.text_utf8;
            candidate.lexeme_ids = row.lexeme_ids;
            candidate.score = row.log_score;
            candidate.input_end_utf8_byte = session.raw.size();
            candidates.push_back(std::move(candidate));
            session.rime_ordinals.push_back(UINT32_MAX);
          }
          // The initialism decoder is bounded (beam/character caps) and does not
          // enumerate every pinyin word.
          if (!initials.empty()) complete = false;
        }
        if (candidates.size() > 1) {
          std::vector<std::size_t> order;
          const auto first = !session.raw.empty() ? 1u : 0u;
          for (std::size_t index = first; index < candidates.size(); ++index) order.push_back(index);
          std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return candidates[a].score > candidates[b].score; });
          std::vector<flow::Candidate> sorted;
          std::vector<std::uint32_t> ordinals;
          if (first) { sorted.push_back(std::move(candidates.front())); ordinals.push_back(UINT32_MAX); }
          for (const auto index : order) { sorted.push_back(std::move(candidates[index])); ordinals.push_back(session.rime_ordinals[index]); }
          candidates = std::move(sorted); session.rime_ordinals = std::move(ordinals);
        }
        complete = snapshot.is_last_page && snapshot.candidate_count_on_page + (!session.raw.empty()) <= query->max_candidates;
      } else if (session.language == EPOCANVA_BACKEND_JA) {
        if (!session.raw.empty()) {
          flow::Candidate original; original.text_utf8=session.raw; original.input_end_utf8_byte=session.raw.size();
          candidates.push_back(std::move(original));
          flow::Candidate kana; kana.kind=EPOCANVA_BACKEND_CONVERSION;
          kana.text_utf8=KanaScript(JapaneseReading(session), session.profile.output_mode == EPOCANVA_BACKEND_OUTPUT_KATAKANA ||
              (!session.profile.id && session.mode == EPOCANVA_BACKEND_KATAKANA));
          kana.input_end_utf8_byte=session.raw.size();
          kana.score = CandidateModelScore(session, {}, context, counts) - 1.0;
          if (kana.text_utf8 != session.raw && candidates.size()<query->max_candidates) candidates.push_back(std::move(kana));
        }
        complete = false;
        if(session.resource->japanese && !session.preedit.empty() &&
            (session.profile.output_mode == 0 || session.profile.output_mode == EPOCANVA_BACKEND_OUTPUT_KANJI)) {
          std::vector<epocanva::japanese_decoder::Candidate> converted;
          const auto converted_status=session.resource->japanese->Convert(JapaneseReading(session),query->max_candidates,&converted);
          complete=converted_status==epocanva::japanese_decoder::Status::kOk;
          for(const auto& row:converted){
            if(candidates.size()>=query->max_candidates)break;
            if(std::any_of(candidates.begin(),candidates.end(),[&](const auto& prior){return prior.text_utf8==row.text_utf8;}))continue;
            flow::Candidate candidate;candidate.kind=EPOCANVA_BACKEND_CONVERSION;candidate.text_utf8=row.text_utf8;
            candidate.score=-static_cast<double>(row.cost);candidate.input_end_utf8_byte=session.raw.size();
            bool known=!row.segments.empty();
            for(const auto& segment:row.segments){
              auto entry=session.resource->model->Find("ja",segment.surface_utf8);
              if(segment.unknown||!entry){known=false;break;}candidate.lexeme_ids.push_back(entry->id);
            }
            if(!known||candidate.lexeme_ids.size()>8)candidate.lexeme_ids.clear();
            candidate.score = -static_cast<double>(row.cost) / 1000.0 +
                CandidateModelScore(session, candidate.lexeme_ids, context, counts);
            complete=complete&&row.complete;candidates.push_back(std::move(candidate));
          }
          const auto first = session.raw.empty() ? candidates.begin() : candidates.begin() + 1;
          std::stable_sort(first, candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        }
      } else {
        lex::QueryOptions options; options.max_candidates = 64;
        options.keyboard_layout = static_cast<epocanva::keyboard::Layout>(session.profile.keyboard_layout);
        if (session.profile.abbreviations) options.enable_abbreviation = false;
        auto lexical = session.resource->lexical->Suggest(Language(session.language), session.preedit, options, ranking);
        if (lexical.status == lex::QueryStatus::kDictionaryUnavailable) return EPOCANVA_BACKEND_UNAVAILABLE;
        complete = !(lexical.prefix_budget_exhausted || lexical.accent_budget_exhausted || lexical.morphology_budget_exhausted ||
                     lexical.spelling_budget_exhausted || lexical.segmentation_budget_exhausted);
        for (const auto& row : lexical.candidates) {
          flow::Candidate candidate;
          candidate.kind=Kind(row.type); candidate.text_utf8=row.text_utf8;
          candidate.lexeme_ids=row.lexeme_ids; candidate.score=row.match_log_score;
          candidate.input_end_utf8_byte=session.raw.size();
          if (candidate.lexeme_ids.size()>8 || std::any_of(candidate.lexeme_ids.begin(),candidate.lexeme_ids.end(),
                [&](auto id){return !id || !session.resource->model->Find(id);})) candidate.lexeme_ids.clear();
          if (!candidate.lexeme_ids.empty())
            candidate.score += CandidateModelScore(session, candidate.lexeme_ids, context, counts);
          candidates.push_back(std::move(candidate));
        }
        if ((lexical.status == lex::QueryStatus::kOk || lexical.status == lex::QueryStatus::kEmptyInput) &&
            !session.preedit.empty()) {
          pred::PredictOptions predicted_options;predicted_options.language_id=Language(session.language);
          predicted_options.max_suggestions=query->max_candidates;predicted_options.max_index_entries=static_cast<std::size_t>(query->search_budget);
            const auto predicted_prefix = lex::FoldUtf8(session.preedit);
            const auto predicted=session.resource->model->Predict(
                predicted_prefix.status == lex::TextStatus::kOk ? predicted_prefix.text_utf8 : session.preedit,
                context,predicted_options,counts);
          complete = complete && predicted.complete;
          const auto analysis=lex::AnalyzeWordUtf8(session.preedit,true);
          for(const auto& row:predicted.suggestions){
            if(row.lexeme_ids.size()!=1 || row.tokens_utf8.size()!=1)continue;
            const auto* entry=session.resource->model->Find(row.lexeme_ids.front());
            if(!entry || entry->kind!=pred::TokenKind::kWord)continue;
            const auto cased=lex::ApplyCaseStyleUtf8(row.tokens_utf8.front(),analysis.case_style);
            const auto text=cased.status==lex::TextStatus::kOk?cased.text_utf8:row.tokens_utf8.front();
            flow::Candidate candidate;candidate.text_utf8=text;candidate.kind=EPOCANVA_BACKEND_WORD;
            candidate.lexeme_ids=row.lexeme_ids;candidate.score=row.log_probability;candidate.input_end_utf8_byte=session.raw.size();
            MergeCandidate(&candidates, std::move(candidate));
          }
        }
        if (session.profile.abbreviations && !session.preedit.empty() &&
            (lexical.status == lex::QueryStatus::kOk ||
             (session.preedit.find(' ') != std::string::npos && lexical.status == lex::QueryStatus::kInvalidWord))) {
          pred::abbreviation::Options expansion_options;
          expansion_options.max_suggestions = std::min<std::size_t>(query->max_candidates, pred::abbreviation::kMaxResults);
          if (query->search_budget) expansion_options.max_index_entries =
              std::min<std::size_t>(query->search_budget, pred::abbreviation::kMaxIndexEntries);
          const auto explicit_options = [&] {
            auto value = expansion_options; value.enable_initialism = false; return value;
          }();
          const auto explicit_result = session.profile.abbreviations->Decode(session.preedit, context,
              Language(session.language), explicit_options, counts);
          auto expanded = session.profile.abbreviations->Decode(session.preedit, context,
              Language(session.language), expansion_options, counts);
          for (const auto& row : explicit_result.suggestions)
            if (row.exact_key) expanded.suggestions.push_back(row);
          if (expanded.status == pred::Status::kOk || expanded.status == pred::Status::kBudgetExhausted || expanded.status == pred::Status::kBeamPruned) {
            complete = complete && expanded.complete;
            for (const auto& row : expanded.suggestions) {
              flow::Candidate candidate; candidate.kind = EPOCANVA_BACKEND_EXPANSION;
              candidate.text_utf8 = row.text_utf8; candidate.lexeme_ids = row.lexeme_ids;
              const auto analysis = lex::AnalyzeWordUtf8(session.preedit, true);
              const auto display = lex::ApplyCaseStyleUtf8(candidate.text_utf8, analysis.case_style);
              if (analysis.status == lex::TextStatus::kOk && display.status == lex::TextStatus::kOk) candidate.text_utf8 = display.text_utf8;
              candidate.score = row.log_probability + row.match_log_score;
              candidate.exact_expansion = row.exact_key;
              candidate.input_end_utf8_byte = session.raw.size();
              MergeCandidate(&candidates, std::move(candidate));
            }
          }
        }
#ifdef EPOCANVA_BACKEND_FRENCH
        if (session.resource->french && !session.preedit.empty()) {
          const auto corrected = session.resource->french->Suggest(session.preedit, query->max_candidates);
          for (const auto& text : corrected.values_utf8) {
            flow::Candidate candidate; candidate.kind=EPOCANVA_BACKEND_SPELLING; candidate.text_utf8=text;
            candidate.input_end_utf8_byte=session.raw.size();
            const auto id=session.resource->lexical->FindExact("fr",text);
            if(id) candidate.lexeme_ids.push_back(id);
            // A dictionary-only word has one smoothed unseen-token probability;
            // it must not outrank attested words merely by lacking a model score.
            candidate.score = -4.0 + CandidateModelScore(session, candidate.lexeme_ids, context, counts);
            MergeCandidate(&candidates, std::move(candidate));
          }
        }
#endif
        const auto begin = candidates.empty() ? candidates.end() : candidates.begin()+1;
        std::stable_sort(begin,candidates.end(),[](const auto& lhs,const auto& rhs) {
          if (lhs.exact_expansion != rhs.exact_expansion) return lhs.exact_expansion;
          if (lhs.score != rhs.score) return lhs.score > rhs.score;
          return lhs.lexeme_ids < rhs.lexeme_ids;
        });
        if(candidates.size()>query->max_candidates)candidates.resize(query->max_candidates);
      }
      // Personal preferences re-rank the completion/conversion menu the user is choosing
      // from. Next-word/next-unit predictions keep their context-model order in this phase;
      // a just-committed word must not displace the model's next-word prediction.
      if (query->query_kind == EPOCANVA_BACKEND_COMPLETION)
        ApplyPersonalRanking(session, preferences, &candidates);
      // Unified candidate contract. Language + scheme scope a stable identity;
      // learnable rows use the same `lex:<id>` key schema v3 persists. A source
      // that cannot produce a valid contract is a programming error, not an
      // empty menu, so the query is refused instead of publishing an identity
      // the learning layer could not round-trip.
      const char* language_tag = Language(session.language);
      const char* scheme_id = Scheme(session.mode);
      if (language_tag == nullptr || scheme_id == nullptr)
        return EPOCANVA_BACKEND_UNAVAILABLE;
      for (auto& candidate : candidates) {
        cand::Record record;
        if (!cand::Describe(language_tag, scheme_id, session.language, session.mode,
                            candidate.kind, candidate.text_utf8, candidate.lexeme_ids,
                            candidate.score, &record))
          return EPOCANVA_BACKEND_UNAVAILABLE;
        record.input_begin_utf8_byte = candidate.input_begin_utf8_byte;
        record.input_end_utf8_byte = candidate.input_end_utf8_byte;
        std::string reason;
        if (!cand::Validate(record, &reason)) return EPOCANVA_BACKEND_UNAVAILABLE;
        candidate.contract = std::move(record);
      }
      return Result(session.flow->Publish(std::move(candidates),complete,access));
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_snapshot(
    epocanva_backend_session_handle handle,const epocanva_backend_request* request,
    epocanva_backend_snapshot* out,char* buffer,std::uint64_t capacity) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status=Output(out);if(status!=EPOCANVA_OK)return status;
    return With(handle,request,[&](Session& session,const learn::Access& access){
      flow::Snapshot snapshot;auto result=Result(session.flow->ReadSnapshot(access,&snapshot));if(result!=EPOCANVA_OK)return result;
      out->snapshot_token=snapshot.token;out->context_target=request->context.context_target;out->generation=request->context.generation;
      out->raw_input={0,snapshot.raw_utf8.size()};out->preedit={snapshot.raw_utf8.size(),snapshot.preedit_utf8.size()};
      out->required_utf8_bytes=snapshot.raw_utf8.size()+snapshot.preedit_utf8.size();out->caret_utf8_byte_offset=snapshot.preedit_utf8.size();
      out->candidate_count=static_cast<std::uint32_t>(snapshot.candidates.size());out->search_complete=snapshot.complete;
      out->language=session.language;out->input_mode=session.mode;
      return Copy({snapshot.raw_utf8,snapshot.preedit_utf8},buffer,capacity);
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_candidate(
    epocanva_backend_session_handle handle,const epocanva_backend_request* request,
    std::uint64_t token,std::uint32_t ordinal,epocanva_backend_candidate* out,char* buffer,std::uint64_t capacity) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status=Output(out);if(status!=EPOCANVA_OK)return status;
    return With(handle,request,[&](Session& session,const learn::Access& access)->epocanva_status{
      flow::Snapshot snapshot;auto result=Result(session.flow->ReadSnapshot(access,&snapshot));if(result!=EPOCANVA_OK)return result;
      if(!token || snapshot.token!=token)return EPOCANVA_STALE_SNAPSHOT;
      if(ordinal>=snapshot.candidates.size())return EPOCANVA_OUT_OF_RANGE;
      const auto& candidate=snapshot.candidates[ordinal];out->snapshot_token=token;out->candidate_token=candidate.token;
      out->required_utf8_bytes=candidate.text_utf8.size();out->text={0,candidate.text_utf8.size()};
      out->kind=candidate.kind;out->lexeme_count=static_cast<std::uint32_t>(candidate.lexeme_ids.size());
      std::copy(candidate.lexeme_ids.begin(),candidate.lexeme_ids.end(),out->lexeme_ids);
      out->score=candidate.score;out->input_begin_utf8_byte=candidate.input_begin_utf8_byte;out->input_end_utf8_byte=candidate.input_end_utf8_byte;
      return Copy({candidate.text_utf8},buffer,capacity);
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_select(
    epocanva_backend_session_handle handle,const epocanva_backend_request* request,
    std::uint64_t snapshot_token,std::uint64_t candidate_token) noexcept {
  return Guard([&]{return With(handle,request,[&](Session& session,const learn::Access& access)->epocanva_status{
    if (!session.rime || session.rime_ordinals.empty()) return Result(session.flow->Select(snapshot_token,candidate_token,{},access));
    flow::Snapshot shown;
    auto status=Result(session.flow->ReadSnapshot(access,&shown));
    if(status!=EPOCANVA_OK)return status;
    if(!snapshot_token || shown.token!=snapshot_token)return EPOCANVA_STALE_SNAPSHOT;
    const auto found=std::find_if(shown.candidates.begin(),shown.candidates.end(),
      [candidate_token](const auto& candidate){return candidate.token==candidate_token;});
    if(found==shown.candidates.end())return EPOCANVA_STALE_SNAPSHOT;
    const auto ordinal=static_cast<std::size_t>(found-shown.candidates.begin());
    if(ordinal>=session.rime_ordinals.size())return EPOCANVA_STALE_SNAPSHOT;
    const auto native_ordinal=session.rime_ordinals[ordinal];
    if(native_ordinal==UINT32_MAX)return Result(session.flow->Select(snapshot_token,candidate_token,{},access));
    auto native=Init<epocanva_candidate_request>();native.context=session.rime_context;
    native.snapshot_version=session.rime_snapshot;native.candidate_ordinal_on_page=native_ordinal;
    auto selected=Init<epocanva_operation_result>();
    status=epocanva_session_select_candidate(session.rime,&native,&selected);
    if(status!=EPOCANVA_OK){session.flow->Reset(access);return status;}
    if(!selected.backend_reported_handled){session.flow->Reset(access);return EPOCANVA_BACKEND_ERROR;}
    // Some schemas confirm a segment before producing a commit. Explicit
    // acceptance then sends Return once; never synthesize the display as commit.
    if(!selected.commit_pending){
      auto key=Init<epocanva_key_request>();key.context=session.rime_context;key.keysym=0xff0d;
      selected=Init<epocanva_operation_result>();status=epocanva_session_process_key(session.rime,&key,&selected);
      if(status!=EPOCANVA_OK){session.flow->Reset(access);return status;}
    }
    auto commit=Init<epocanva_commit>();auto read=RimeRequest(session);
    status=epocanva_session_read_commit(session.rime,&read,&commit,nullptr,0);
    if(status!=EPOCANVA_BUFFER_TOO_SMALL){session.flow->Reset(access);return EPOCANVA_BACKEND_ERROR;}
    std::string committed(static_cast<std::size_t>(commit.required_utf8_bytes),'\0');
    status=epocanva_session_read_commit(session.rime,&read,&commit,committed.data(),committed.size());
    if(status!=EPOCANVA_OK){session.flow->Reset(access);return status;}
    epocanva_snapshot snapshot{};std::string state;
    status=RimeState(session,&snapshot,&state);if(status!=EPOCANVA_OK){session.flow->Reset(access);return status;}
    return Result(session.flow->AcceptExternal(snapshot_token,candidate_token,Slice(committed,commit.text),
      Slice(state,snapshot.raw_input),Slice(state,snapshot.preedit),access));
  });});
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_commit(
    epocanva_backend_session_handle handle,const epocanva_backend_request* request,
    epocanva_backend_commit* out,char* buffer,std::uint64_t capacity) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status=Output(out);if(status!=EPOCANVA_OK)return status;
    return With(handle,request,[&](Session& session,const learn::Access& access)->epocanva_status{
      flow::Commit commit;auto result=Result(session.flow->ReadCommit(access,&commit));if(result!=EPOCANVA_OK)return result;
      out->commit_token=commit.token;out->text={0,commit.text_utf8.size()};
      out->remaining_preedit={commit.text_utf8.size(),commit.remaining_preedit_utf8.size()};
      out->required_utf8_bytes=commit.text_utf8.size()+commit.remaining_preedit_utf8.size();out->awaiting_ack=1;
      if(!buffer && !capacity)return EPOCANVA_BUFFER_TOO_SMALL;
      result=Copy({commit.text_utf8,commit.remaining_preedit_utf8},buffer,capacity);
      if(result!=EPOCANVA_OK)return result;
      return Result(session.flow->MarkCommitCopied(commit.token,access));
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_ack_commit(
    epocanva_backend_session_handle handle,const epocanva_backend_request* request,
    std::uint64_t token,std::uint32_t applied,epocanva_backend_ack_result* out) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status=Output(out);if(status!=EPOCANVA_OK)return status;
    status=Request(request);if(status!=EPOCANVA_OK)return status;if(applied>1)return EPOCANVA_INVALID_ARGUMENT;
    auto& registry=State();std::lock_guard<std::mutex> lock(registry.mutex);
    auto found=registry.sessions.find(handle);if(found==registry.sessions.end())return EPOCANVA_INVALID_HANDLE;
    auto& session=*found->second;auto access=Access(request->context,request->learning_enabled);
    flow::Persist persist;
    if(session.backend->repository)persist=[&session](const learn::Selection& selected,const flow::PrivacyCallback& permission){
      return session.backend->repository->Record(session.flow->catalog(),selected,permission);
    };
    const auto prior_context = session.flow->Context(access);
    flow::Commit committed;
    const bool has_commit = session.flow->ReadCommit(access, &committed) == flow::Status::kOk && committed.token == token;
    const auto result=session.flow->Acknowledge(token,applied!=0,[access]{return access;},persist);
    out->consumed=result.consumed;out->learned=result.learned;out->context_advanced=result.context_advanced;
    if (has_commit && result.status == flow::Status::kOk && result.consumed && applied &&
        !result.context_advanced && committed.lexeme_ids.empty()) {
      auto ids = epocanva::backend::ResolveContext(*session.resource->model, Language(session.language),
          committed.text_utf8, prior_context);
      if (!ids.empty())
        out->context_advanced = session.flow->SetContext(std::move(ids), access) == flow::Status::kOk;
    }
    out->persistence_status=static_cast<std::uint32_t>(result.persistence_status);
    if(result.consumed){
      flow::Snapshot after;
      if(session.flow->ReadSnapshot(access,&after)==flow::Status::kOk){session.raw=after.raw_utf8;session.preedit=after.preedit_utf8;}
      else{session.raw.clear();session.preedit.clear();}
      session.japanese.Cancel();session.korean.Cancel();
      if(session.rime && session.preedit.empty())(void)RimeReset(session);
    }
    return Result(result.status);
  });
}
