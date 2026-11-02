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
#include "engine/lexical_service/lexical_service.h"
#include "engine/prediction/model.h"
#include "engine/languages/ja/romaji_composer.h"
#include "engine/languages/ko/two_set_composer.h"
#include "engine/japanese_decoder/japanese_decoder.h"
#include "engine/data_store/windows_learning_repository.h"
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
template<class T> epocanva_status Structure(const T* value) {
  if (!value) return EPOCANVA_INVALID_ARGUMENT;
  if (value->struct_size_bytes < sizeof(T)) return EPOCANVA_STRUCT_TOO_SMALL;
  return value->abi_version == EPOCANVA_BACKEND_ABI_VERSION ? EPOCANVA_OK : EPOCANVA_ABI_VERSION_MISMATCH;
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
      learn::PrivacyClass::kUnknown, enabled != 0, context.context_target, context.generation};
}
struct Resource {
  std::shared_ptr<const pred::Model> model;
  std::unique_ptr<lex::LexicalService> lexical;
  std::string model_sha256;
  std::filesystem::path conversion_path;
  std::unique_ptr<epocanva::japanese_decoder::Decoder> japanese;
#ifdef EPOCANVA_BACKEND_FRENCH
  std::shared_ptr<const epocanva::french_dictionary::Dictionary> french;
#endif
  learn::Catalog catalog;
};
struct Backend {
  ~Backend() { if (rime) (void)epocanva_engine_close(rime); }
  std::thread::id owner = std::this_thread::get_id();
  std::map<std::uint32_t, Resource> resources;
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
epocanva_status Compose(Session& session, const std::string& raw, const learn::Access& access) {
  flow::Commit pending;
  if (session.flow->ReadCommit(access, &pending) == flow::Status::kOk) return EPOCANVA_BUSY;
  std::string preedit = raw;
  if (session.language == EPOCANVA_BACKEND_JA) {
    auto composer = epocanva::ja::RomajiComposer(session.mode == EPOCANVA_BACKEND_KATAKANA ?
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
void Personal(Session& session, const learn::Access& access, pred::PersonalCounts* counts,
              lex::RankingInputs* ranking, std::vector<lex::LearnedCount>* words,
              std::vector<lex::ContextCount>* pairs) {
  learn::Snapshot snapshot;
  if (!access.learning_enabled) return;
  if (session.backend->repository) {
    auto status = session.backend->repository->LoadSnapshot(session.flow->catalog(), [access] { return access; }, &snapshot);
    if (status != learn::Status::kOk) return;
    session.flow->ImportLearning(snapshot, access);
  } else if (session.flow->ReadLearning(access, &snapshot) != learn::Status::kOk) return;
  for (auto row : snapshot.words) { counts->unigrams.push_back({row.id,row.count}); words->push_back({row.id,row.count}); }
  for (auto row : snapshot.pairs) { counts->bigrams.push_back({row.previous_id,row.id,row.count}); pairs->push_back({row.previous_id,row.id,row.count}); }
  ranking->allow_learning = true; ranking->learned = words; ranking->context = pairs;
  const auto context = session.flow->Context(access);
  ranking->previous_id = context.empty() ? 0 : context.back();
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
      auto model_path = std::filesystem::u8path(Text(input.model_path, 30000));
      epocanva::backend::LocalResource held_model;
      if (!held_model.Open(model_path)) return EPOCANVA_BACKEND_INVALID_RESOURCE;
      model_path = held_model.path();
      resource.model_sha256 = Text(input.model_sha256, 64);
      if (!Digest(model_path, resource.model_sha256)) return EPOCANVA_BACKEND_INVALID_RESOURCE;
      pred::LoadOptions load; load.allow_test_fixture = options->allow_test_fixture != 0;
      resource.model = pred::Model::Load(model_path, load);
      if (!resource.model || !resource.model->available()) return EPOCANVA_BACKEND_INVALID_RESOURCE;
      resource.catalog.name_space = std::string("backend.") + Language(input.language) + ".v1";
      resource.catalog.sha256 = resource.model_sha256;
      for (const auto& row : resource.model->lexemes()) if (row.language_id == Language(input.language)) resource.catalog.admitted_ids.push_back(row.id);
      if (resource.catalog.admitted_ids.empty()) return EPOCANVA_BACKEND_INVALID_RESOURCE;
      if (input.language == EPOCANVA_BACKEND_EN || input.language == EPOCANVA_BACKEND_FR || input.language == EPOCANVA_BACKEND_KO) {
        resource.lexical = lex::LexicalService::Create(*resource.model);
        if (!resource.lexical) return EPOCANVA_BACKEND_INVALID_RESOURCE;
      }
      if (input.conversion_path.utf8_byte_count) {
        if (input.language != EPOCANVA_BACKEND_JA && input.language != EPOCANVA_BACKEND_FR)
          return EPOCANVA_INVALID_ARGUMENT;
        resource.conversion_path = std::filesystem::u8path(Text(input.conversion_path, 30000));
        // Each actual converter owns paired local-path pinning and hash checks;
        // never pre-open arbitrary resource paths with a generic file helper.
      }
      if (input.language == EPOCANVA_BACKEND_JA && input.conversion_path.utf8_byte_count) {
        resource.japanese = std::make_unique<epocanva::japanese_decoder::Decoder>();
        epocanva::japanese_decoder::LoadOptions loading;
        loading.expected_sha256 = Text(input.conversion_sha256, 64);
        if (resource.japanese->Load(resource.conversion_path.u8string(), loading) != epocanva::japanese_decoder::Status::kOk)
          return EPOCANVA_BACKEND_INVALID_RESOURCE;
      }
      if (input.language == EPOCANVA_BACKEND_FR && input.conversion_path.utf8_byte_count) {
#ifdef EPOCANVA_BACKEND_FRENCH
        const auto dictionary_path = std::filesystem::u8path(Text(input.auxiliary_path, 30000));
        epocanva::french_dictionary::ExpectedHashes expected;
        expected.aff_sha256 = Text(input.conversion_sha256, 64);
        expected.dic_sha256 = Text(input.auxiliary_sha256, 64);
        resource.french = epocanva::french_dictionary::Dictionary::Load(resource.conversion_path, dictionary_path, expected);
        if (!resource.french) return EPOCANVA_BACKEND_INVALID_RESOURCE;
#else
        return EPOCANVA_BACKEND_UNAVAILABLE;
#endif
      }
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
    if (options->context.privacy_class != EPOCANVA_PRIVACY_NORMAL) return EPOCANVA_PRIVACY_DENIED;
    if (options->reserved || options->context.reserved || options->learning_enabled > 1 ||
        !options->context.context_target || !options->context.generation) return EPOCANVA_INVALID_ARGUMENT;
    const bool mode_ok = ((options->language == EPOCANVA_BACKEND_EN || options->language == EPOCANVA_BACKEND_FR) && options->input_mode == EPOCANVA_BACKEND_LATIN) ||
        (options->language == EPOCANVA_BACKEND_ZH && (options->input_mode == EPOCANVA_BACKEND_PINYIN || options->input_mode == EPOCANVA_BACKEND_ZHUYIN)) ||
        (options->language == EPOCANVA_BACKEND_JA && (options->input_mode == EPOCANVA_BACKEND_ROMAJI || options->input_mode == EPOCANVA_BACKEND_KATAKANA)) ||
        (options->language == EPOCANVA_BACKEND_KO && (options->input_mode == EPOCANVA_BACKEND_LATIN || options->input_mode == EPOCANVA_BACKEND_TWO_SET));
    if (!mode_ok) return EPOCANVA_INVALID_ARGUMENT;
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.backends.find(handle); if (found == registry.backends.end()) return EPOCANVA_INVALID_HANDLE;
    auto& backend = *found->second;
    if (backend.owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
    auto resource = backend.resources.find(options->language);
    if (resource == backend.resources.end()) return EPOCANVA_BACKEND_UNAVAILABLE;
    if (options->language == EPOCANVA_BACKEND_ZH && !backend.rime) return EPOCANVA_BACKEND_UNAVAILABLE;
    auto session = std::make_unique<Session>();
    session->backend = &backend; session->resource = &resource->second;
    session->language = options->language; session->mode = options->input_mode; session->identity = options->context;
    session->flow = flow::Session::Create(resource->second.catalog, resource->second.model_sha256, Access(options->context, options->learning_enabled));
    if (!session->flow) return EPOCANVA_BACKEND_INVALID_RESOURCE;
    if (options->language == EPOCANVA_BACKEND_ZH) {
      auto request = Init<epocanva_session_options>(); request.context = options->context;
      const std::string schema = options->input_mode == EPOCANVA_BACKEND_ZHUYIN ? "epocanva_backend_zhuyin" : "epocanva_backend_pinyin";
      request.schema_id = {schema.data(), schema.size()};
      status = epocanva_session_open(backend.rime, &request, &session->rime);
      if (status != EPOCANVA_OK) return status;
      session->rime_context = request.context;
    }
    const auto token = registry.Token();
    if (!token) return EPOCANVA_OUT_OF_RANGE;
    registry.sessions.emplace(token, std::move(session)); ++backend.sessions; *out = token;
    return EPOCANVA_OK;
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
        query->query_kind < EPOCANVA_BACKEND_COMPLETION || query->query_kind > EPOCANVA_BACKEND_CONTINUATION ||
        !query->max_continuation_words || query->max_continuation_words > 4 || query->search_budget > 10000000)
      return EPOCANVA_INVALID_ARGUMENT;
    auto request = Init<epocanva_backend_request>(); request.context = query->context; request.learning_enabled = query->learning_enabled;
    return With(handle, &request, [&](Session& session, const learn::Access& access) -> epocanva_status {
      flow::Commit pending; if (session.flow->ReadCommit(access, &pending) == flow::Status::kOk) return EPOCANVA_BUSY;
      pred::PersonalCounts personal; lex::RankingInputs ranking;
      std::vector<lex::LearnedCount> words; std::vector<lex::ContextCount> pairs;
      Personal(session, access, &personal, &ranking, &words, &pairs);
      const auto context = session.flow->Context(access);
      const auto* counts = access.learning_enabled ? &personal : nullptr;
      std::vector<flow::Candidate> candidates;
      bool complete = true;
      session.rime_ordinals.clear();
      if (query->query_kind != EPOCANVA_BACKEND_COMPLETION) {
        pred::PredictOptions options;
        options.language_id = Language(session.language); options.max_suggestions = query->max_candidates;
        options.max_words = query->query_kind == EPOCANVA_BACKEND_NEXT_WORD ? 1 : query->max_continuation_words;
        options.max_index_entries = static_cast<std::size_t>(query->search_budget);
        const auto predicted = session.resource->model->Predict(std::nullopt, context, options, counts);
        if (predicted.status != pred::Status::kOk && predicted.status != pred::Status::kBudgetExhausted &&
            predicted.status != pred::Status::kBeamPruned) return EPOCANVA_BACKEND_UNAVAILABLE;
        complete = predicted.complete;
        for (const auto& row : predicted.suggestions) {
          if (row.lexeme_ids.empty()) continue;
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
          candidate.kind = EPOCANVA_BACKEND_CONVERSION; candidate.score = -static_cast<double>(ordinal);
          candidate.input_end_utf8_byte = session.raw.size();
          // Rime's text-only public menu cannot prove lexical identity for learning.
          candidates.push_back(std::move(candidate)); session.rime_ordinals.push_back(ordinal);
        }
        complete = snapshot.is_last_page && snapshot.candidate_count_on_page + (!session.raw.empty()) <= query->max_candidates;
      } else if (session.language == EPOCANVA_BACKEND_JA) {
        if (!session.raw.empty()) {
          flow::Candidate original; original.text_utf8=session.raw; original.input_end_utf8_byte=session.raw.size();
          candidates.push_back(std::move(original));
          auto composer = session.japanese;
          flow::Candidate kana; kana.kind=EPOCANVA_BACKEND_CONVERSION; kana.text_utf8=composer.CommitUtf8();
          kana.input_end_utf8_byte=session.raw.size();
          if (kana.text_utf8 != session.raw && candidates.size()<query->max_candidates) candidates.push_back(std::move(kana));
        }
        complete = false;
        if(session.resource->japanese && !session.preedit.empty()) {
          std::vector<epocanva::japanese_decoder::Candidate> converted;
          auto compose=session.japanese;
          const auto converted_status=session.resource->japanese->Convert(compose.CommitUtf8(),query->max_candidates,&converted);
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
            complete=complete&&row.complete;candidates.push_back(std::move(candidate));
          }
        }
      } else {
        lex::QueryOptions options; options.max_candidates = query->max_candidates;
        auto lexical = session.resource->lexical->Suggest(Language(session.language), session.preedit, options, ranking);
        if (lexical.status == lex::QueryStatus::kDictionaryUnavailable) return EPOCANVA_BACKEND_UNAVAILABLE;
        complete = !(lexical.prefix_budget_exhausted || lexical.accent_budget_exhausted || lexical.morphology_budget_exhausted ||
                     lexical.spelling_budget_exhausted || lexical.segmentation_budget_exhausted);
        for (const auto& row : lexical.candidates) {
          flow::Candidate candidate;
          candidate.kind=Kind(row.type); candidate.text_utf8=row.text_utf8;
          candidate.lexeme_ids=row.lexeme_ids; candidate.score=row.score;
          candidate.input_end_utf8_byte=session.raw.size();
          if (candidate.lexeme_ids.size()>8 || std::any_of(candidate.lexeme_ids.begin(),candidate.lexeme_ids.end(),
                [&](auto id){return !id || !session.resource->model->Find(id);})) candidate.lexeme_ids.clear();
          if (!candidate.lexeme_ids.empty()) {
            auto previous = context;
            double score = 0;
            for (auto id : candidate.lexeme_ids) {
              const auto scored = session.resource->model->Score(previous,id,counts);
              if (scored.status == pred::Status::kOk) score += scored.log_probability;
              previous.push_back(id); if(previous.size()>2) previous.erase(previous.begin());
            }
            candidate.score += score / static_cast<double>(candidate.lexeme_ids.size());
          }
          candidates.push_back(std::move(candidate));
        }
        if ((lexical.status == lex::QueryStatus::kOk || lexical.status == lex::QueryStatus::kEmptyInput) &&
            !session.preedit.empty()) {
          pred::PredictOptions predicted_options;predicted_options.language_id=Language(session.language);
          predicted_options.max_suggestions=query->max_candidates;predicted_options.max_index_entries=static_cast<std::size_t>(query->search_budget);
          const auto predicted=session.resource->model->Predict(session.preedit,context,predicted_options,counts);
          complete = complete && predicted.complete;
          const auto analysis=lex::AnalyzeWordUtf8(session.preedit,true);
          for(const auto& row:predicted.suggestions){
            if(row.lexeme_ids.size()!=1 || row.tokens_utf8.size()!=1)continue;
            const auto* entry=session.resource->model->Find(row.lexeme_ids.front());
            if(!entry || entry->kind!=pred::TokenKind::kWord)continue;
            const auto cased=lex::ApplyCaseStyleUtf8(row.tokens_utf8.front(),analysis.case_style);
            const auto text=cased.status==lex::TextStatus::kOk?cased.text_utf8:row.tokens_utf8.front();
            if(std::any_of(candidates.begin(),candidates.end(),[&](const auto& candidate){return candidate.kind!=EPOCANVA_BACKEND_RAW&&candidate.text_utf8==text;}))continue;
            flow::Candidate candidate;candidate.text_utf8=text;candidate.kind=EPOCANVA_BACKEND_WORD;
            candidate.lexeme_ids=row.lexeme_ids;candidate.score=row.log_probability;candidate.input_end_utf8_byte=session.raw.size();
            candidates.push_back(std::move(candidate));
          }
        }
#ifdef EPOCANVA_BACKEND_FRENCH
        if (session.resource->french && !session.preedit.empty()) {
          const auto corrected = session.resource->french->Suggest(session.preedit, query->max_candidates);
          for (const auto& text : corrected.values_utf8) {
            if (std::any_of(candidates.begin(),candidates.end(),[&](const auto& row){return row.text_utf8==text;})) continue;
            flow::Candidate candidate; candidate.kind=EPOCANVA_BACKEND_SPELLING; candidate.text_utf8=text;
            candidate.input_end_utf8_byte=session.raw.size(); candidate.score=-4;
            auto id=session.resource->lexical->FindExact("fr",text);
            if(id) {candidate.lexeme_ids.push_back(id);candidate.score=session.resource->model->Score(context,id,counts).log_probability;}
            candidates.push_back(std::move(candidate));
          }
        }
#endif
        const auto begin = candidates.empty() ? candidates.end() : candidates.begin()+1;
        std::stable_sort(begin,candidates.end(),[](const auto& lhs,const auto& rhs){return lhs.score>rhs.score;});
        if(candidates.size()>query->max_candidates)candidates.resize(query->max_candidates);
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
    const auto result=session.flow->Acknowledge(token,applied!=0,[access]{return access;},persist);
    out->consumed=result.consumed;out->learned=result.learned;out->context_advanced=result.context_advanced;
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
