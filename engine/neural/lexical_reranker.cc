#include "lexical_reranker.h"

#include "llama.h"

#ifndef _WIN32
#error The optional neural file-verification adapter currently supports Windows only.
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace epocanva::neural {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kMaxModelBytes = 512ull * 1024ull * 1024ull;

void DiscardLog(ggml_log_level, const char*, void*) noexcept {}

void InitializeBackend() {
  static std::once_flag once;
  std::call_once(once, [] {
    // nullptr restores stderr logging in b4514. Keep a NON-null sink forever.
    llama_log_set(DiscardLog, nullptr);
    llama_backend_init();
    // Do NOT call ggml_backend_load_all or any dynamic backend loader.
  });
}

Status CheckControl(const RequestControl& control) noexcept {
  if (control.cancelled && control.cancelled->load(std::memory_order_acquire))
    return Status::kCancelled;
  if (control.current_version &&
      control.current_version->load(std::memory_order_acquire) != control.expected_version)
    return Status::kStaleRequest;
  if (Clock::now() >= control.deadline) return Status::kDeadlineExceeded;
  return Status::kOk;
}

struct AbortState {
  RequestControl control;
  std::atomic<Status> reason{Status::kOk};
  bool Stop() noexcept {
    if (reason.load(std::memory_order_relaxed) != Status::kOk) return true;
    const auto status = CheckControl(control);
    if (status != Status::kOk) {
      auto expected = Status::kOk;
      reason.compare_exchange_strong(expected, status, std::memory_order_relaxed);
    }
    return reason.load(std::memory_order_relaxed) != Status::kOk;
  }
};
bool AbortDecode(void* data) noexcept { return static_cast<AbortState*>(data)->Stop(); }
bool LoadProgress(float, void* data) noexcept { return !AbortDecode(data); }

bool IsSha256(std::string_view value) noexcept {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

// Iteration offsets and widths are UTF-8 code units (bytes).
bool ValidUtf8(std::string_view text) noexcept {
  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[byte_offset]);
    if (first == 0) return false;
    if (first < 0x80) { ++byte_offset; continue; }
    std::size_t width_bytes = 0;
    std::uint32_t scalar = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) { width_bytes = 2; scalar = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { width_bytes = 3; scalar = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { width_bytes = 4; scalar = first & 0x07; minimum = 0x10000; }
    else return false;
    if (width_bytes > text.size() - byte_offset) return false;
    for (std::size_t byte = 1; byte < width_bytes; ++byte) {
      const auto next = static_cast<unsigned char>(text[byte_offset + byte]);
      if ((next & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (next & 0x3f);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    byte_offset += width_bytes;
  }
  return true;
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
      value = std::exchange(other.value, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

bool LocalDrivePath(const std::filesystem::path& path) {
  const auto text = path.native(); // Windows filesystem paths use UTF-16 code units.
  if (!path.is_absolute() || text.size() < 3 || text[1] != L':' ||
      (text[2] != L'\\' && text[2] != L'/')) return false;
  if (text.find(L'\0') != std::wstring::npos || text.find(L':', 2) != std::wstring::npos) return false;
  if (GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED) return false;
  for (const auto& part : path.relative_path()) {
    if (part == L".." || part == L".") return false;
  }
  return true;
}

bool OpenLocalResource(const std::filesystem::path& path, Handle* file,
                       std::vector<Handle>* ancestor_handles, std::uint64_t* file_bytes) {
  if (!LocalDrivePath(path)) return false;
  // Open ancestors top-down without following directory junctions/symlinks.
  // Non-name-surrogate OneDrive directory tags do not redirect to another path.
  auto parent = path.root_path();
  for (const auto& part : path.parent_path().relative_path()) {
    parent /= part;
    Handle directory(CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (directory.value == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(directory.value, FileAttributeTagInfo, &info, sizeof(info)) ||
        !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(info.ReparseTag)))
      return false;
    ancestor_handles->push_back(std::move(directory));
  }
  *file = Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  FILE_ATTRIBUTE_TAG_INFO info{};
  LARGE_INTEGER bytes{};
  constexpr DWORD kRecallOnOpen = 0x00040000;
  constexpr DWORD kRecallOnDataAccess = 0x00400000;
  if (file->value == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(file->value, FileAttributeTagInfo, &info, sizeof(info)) ||
      (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                             FILE_ATTRIBUTE_OFFLINE | kRecallOnOpen | kRecallOnDataAccess)) ||
      !GetFileSizeEx(file->value, &bytes) || bytes.QuadPart <= 0 ||
      static_cast<std::uint64_t>(bytes.QuadPart) > kMaxModelBytes) return false;
  *file_bytes = static_cast<std::uint64_t>(bytes.QuadPart);
  return true;
}

bool HashFile(HANDLE file, std::uint64_t expected_bytes, std::string* sha256, AbortState* abort) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<unsigned char> object;
  struct Release {
    BCRYPT_ALG_HANDLE* algorithm;
    BCRYPT_HASH_HANDLE* hash;
    ~Release() {
      if (*hash) BCryptDestroyHash(*hash);
      if (*algorithm) BCryptCloseAlgorithmProvider(*algorithm, 0);
    }
  } release{&algorithm, &hash};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  DWORD object_bytes = 0, returned_bytes = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned_bytes, 0) < 0) return false;
  object.resize(object_bytes);
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) < 0) return false;
  std::vector<unsigned char> block(1024 * 1024);
  std::uint64_t read_bytes = 0;
  for (;;) {
    if (abort->Stop()) return false;
    DWORD chunk_bytes = 0;
    if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &chunk_bytes, nullptr)) return false;
    if (!chunk_bytes) break;
    read_bytes += chunk_bytes;
    if (read_bytes > expected_bytes || BCryptHashData(hash, block.data(), chunk_bytes, 0) < 0) return false;
  }
  std::array<unsigned char, 32> digest{};
  if (read_bytes != expected_bytes || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
    return false;
  constexpr char hex[] = "0123456789abcdef";
  sha256->clear();
  for (auto byte : digest) { sha256->push_back(hex[byte >> 4]); sha256->push_back(hex[byte & 15]); }
  return true;
}

bool ValidLimits(const Limits& value) noexcept {
  return value.context_tokens >= 64 && value.context_tokens <= 512 &&
      value.max_candidate_tokens >= 1 && value.max_candidate_tokens <= 32 &&
      value.max_candidates >= 1 && value.max_candidates <= 8 &&
      value.max_context_utf8_bytes >= 1 && value.max_context_utf8_bytes <= 2048 &&
      value.max_candidate_utf8_bytes >= 1 && value.max_candidate_utf8_bytes <= 128 &&
      value.worker_threads >= 1 && value.worker_threads <= 16 &&
      value.max_request_time.count() > 0 && value.max_request_time <= std::chrono::seconds(30);
}

bool Tokenize(const llama_vocab* vocab, std::string_view utf8,
              std::uint32_t capacity_tokens, std::vector<llama_token>* tokens) {
  tokens->resize(capacity_tokens);
  const auto count_tokens = llama_tokenize(vocab, utf8.data(), static_cast<std::int32_t>(utf8.size()),
      tokens->data(), static_cast<std::int32_t>(capacity_tokens), false, false);
  if (count_tokens < 0 || count_tokens > static_cast<std::int32_t>(capacity_tokens)) return false;
  tokens->resize(static_cast<std::size_t>(count_tokens));
  return true;
}

struct ContextDeleter {
  void operator()(llama_context* value) const noexcept {
    if (value) { llama_kv_cache_clear(value); llama_free(value); }
  }
};
using Context = std::unique_ptr<llama_context, ContextDeleter>;

// Batch offsets/counts are model-token units; never UTF-8 or UTF-16 offsets.
Status Decode(llama_context* ctx, const std::vector<llama_token>& tokens,
              std::size_t first_token, std::size_t count_tokens, bool all_outputs,
              AbortState* abort) {
  if (abort->Stop()) return abort->reason.load();
  std::vector<llama_pos> token_positions(count_tokens);
  std::vector<std::int8_t> output_flags(count_tokens, all_outputs ? 1 : 0);
  for (std::size_t token = 0; token < count_tokens; ++token)
    token_positions[token] = static_cast<llama_pos>(first_token + token);
  output_flags.back() = 1;
  llama_batch batch{};
  batch.n_tokens = static_cast<std::int32_t>(count_tokens);
  batch.token = const_cast<llama_token*>(tokens.data() + first_token);
  batch.pos = token_positions.data();
  batch.logits = output_flags.data();
  const auto code = llama_decode(ctx, batch);
  if (abort->Stop()) return abort->reason.load();
  // b4514 llama.cpp:8631 maps GGML_STATUS_ABORTED to +2, not a negative error.
  if (code != 0) return abort->reason.load() != Status::kOk ? abort->reason.load() : Status::kInternalError;
  return Status::kOk;
}

bool LogProbability(const float* logits, std::int32_t vocabulary_tokens, llama_token token,
                    double* result, AbortState* abort) {
  if (!logits || token < 0 || token >= vocabulary_tokens) return false;
  double maximum = -std::numeric_limits<double>::infinity();
  for (std::int32_t i = 0; i < vocabulary_tokens; ++i) {
    if ((i & 4095) == 0 && abort->Stop()) return false;
    if (!std::isfinite(logits[i])) return false;
    maximum = std::max(maximum, static_cast<double>(logits[i]));
  }
  double sum = 0;
  for (std::int32_t i = 0; i < vocabulary_tokens; ++i) {
    if ((i & 4095) == 0 && abort->Stop()) return false;
    sum += std::exp(static_cast<double>(logits[i]) - maximum);
  }
  *result = static_cast<double>(logits[token]) - maximum - std::log(sum);
  return std::isfinite(*result);
}
} // namespace

struct Model::Impl {
  Handle file;
  std::vector<Handle> ancestors;
  std::array<ggml_backend_dev_t, 1> no_offload_devices{nullptr};
  std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
  ModelInfo info;
};

Model::Model(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Model::~Model() = default;
const ModelInfo& Model::info() const noexcept { return impl_->info; }

std::shared_ptr<const Model> Model::Load(const std::filesystem::path& path,
    std::string_view expected_sha256, Status* status, const RequestControl& control) {
  const auto set = [status](Status value) { if (status) *status = value; };
  set(Status::kInvalidArgument);
  if (!IsSha256(expected_sha256)) return {};
  AbortState abort{control};
  abort.control.deadline = std::min(control.deadline, Clock::now() + std::chrono::seconds(30));
  if (abort.Stop()) { set(abort.reason.load()); return {}; }
  try {
    auto impl = std::make_unique<Impl>();
    if (!OpenLocalResource(path, &impl->file, &impl->ancestors, &impl->info.file_bytes)) {
      set(Status::kInvalidModel); return {};
    }
    if (!HashFile(impl->file.value, impl->info.file_bytes, &impl->info.sha256, &abort) ||
        impl->info.sha256 != expected_sha256) {
      set(abort.reason.load() == Status::kOk ? Status::kInvalidModel : abort.reason.load()); return {};
    }
    InitializeBackend();
    if (ggml_backend_reg_count() != 1 || !ggml_backend_reg_by_name("CPU") ||
        llama_supports_rpc() || llama_supports_gpu_offload()) {
      set(Status::kUnavailable); return {};
    }
    auto parameters = llama_model_default_params();
    parameters.devices = impl->no_offload_devices.data();
    parameters.n_gpu_layers = 0;
    // Preserve the default LAYER split mode: NONE erroneously demands a GPU in b4514.
    parameters.use_mmap = true;
    parameters.use_mlock = false;
    parameters.vocab_only = false;
    parameters.check_tensors = true;
    parameters.progress_callback = LoadProgress;
    parameters.progress_callback_user_data = &abort;
    impl->model.reset(llama_model_load_from_file(path.u8string().c_str(), parameters));
    if (abort.Stop()) { set(abort.reason.load()); return {}; }
    if (!impl->model) { set(Status::kInvalidModel); return {}; }
    char architecture[32]{};
    if (llama_model_meta_val_str(impl->model.get(), "general.architecture", architecture, sizeof(architecture)) != 5 ||
        std::string_view(architecture) != "qwen2" || llama_model_has_encoder(impl->model.get()) ||
        !llama_model_has_decoder(impl->model.get()) || llama_model_is_recurrent(impl->model.get()) ||
        llama_model_n_embd(impl->model.get()) != 896 || llama_model_n_layer(impl->model.get()) != 24 ||
        llama_model_n_head(impl->model.get()) != 14) {
      set(Status::kInvalidModel); return {};
    }
    const auto* vocab = llama_model_get_vocab(impl->model.get());
    impl->info.vocabulary_tokens = static_cast<std::uint32_t>(llama_vocab_n_tokens(vocab));
    impl->info.training_context_tokens = static_cast<std::uint32_t>(llama_model_n_ctx_train(impl->model.get()));
    impl->info.parameter_count = llama_model_n_params(impl->model.get());
    if (impl->info.vocabulary_tokens != 151936 || impl->info.training_context_tokens != 32768 ||
        impl->info.parameter_count != 494032768 ||
        llama_vocab_type(vocab) != LLAMA_VOCAB_TYPE_BPE || llama_vocab_bos(vocab) != 151643) {
      set(Status::kInvalidModel); return {};
    }
    set(Status::kOk);
    return std::shared_ptr<const Model>(new Model(std::move(impl)));
  } catch (...) { set(Status::kInternalError); return {}; }
}

struct LexicalReranker::Impl {
  std::shared_ptr<const Model> model;
  Limits limits;
  std::mutex mutex;
};
LexicalReranker::LexicalReranker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LexicalReranker::~LexicalReranker() = default;

std::unique_ptr<LexicalReranker> LexicalReranker::Create(
    std::shared_ptr<const Model> model, const Limits& limits, Status* status) {
  if (status) *status = model ? Status::kInvalidArgument : Status::kUnavailable;
  if (!model || !ValidLimits(limits)) return {};
  try {
    auto impl = std::make_unique<Impl>();
    impl->model = std::move(model);
    impl->limits = limits;
    auto result = std::unique_ptr<LexicalReranker>(new LexicalReranker(std::move(impl)));
    if (status) *status = Status::kOk;
    return result;
  } catch (...) { if (status) *status = Status::kInternalError; return {}; }
}

Result LexicalReranker::Rerank(std::string_view prefix,
    const std::vector<std::string_view>& candidates, bool privacy_permitted,
    const RequestControl& control) {
  const auto started = Clock::now();
  Result result;
  result.version = control.expected_version;
  AbortState abort{control};
  abort.control.deadline = std::min(control.deadline, started + impl_->limits.max_request_time);
  const auto finish = [&](Status value) {
    result.status = value;
    if (value == Status::kOk && abort.Stop()) result.status = abort.reason.load();
    if (result.status != Status::kOk) result.ranking.clear();
    result.elapsed_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
    return std::move(result);
  };
  if (!privacy_permitted) return finish(Status::kPrivacyDenied);
  if (abort.Stop()) return finish(abort.reason.load());
  std::unique_lock<std::mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) return finish(Status::kBusy);
  const auto& limits = impl_->limits;
  if (candidates.empty()) return finish(Status::kInvalidArgument);
  if (candidates.size() > limits.max_candidates || prefix.size() > limits.max_context_utf8_bytes)
    return finish(Status::kLimitExceeded);
  if (!ValidUtf8(prefix)) return finish(Status::kInvalidArgument);
  for (const auto candidate : candidates) {
    if (candidate.size() > limits.max_candidate_utf8_bytes) return finish(Status::kLimitExceeded);
    if (candidate.empty() || !ValidUtf8(candidate)) return finish(Status::kInvalidArgument);
  }
  try {
    const auto work = [&]() -> Status {
      const auto* vocab = llama_model_get_vocab(impl_->model->impl_->model.get());
      std::vector<std::vector<llama_token>> token_sequences;
      token_sequences.reserve(candidates.size());
      std::vector<llama_token> candidate_tokens;
      for (const auto candidate : candidates) {
        if (abort.Stop()) return abort.reason.load();
        if (!Tokenize(vocab, candidate, limits.max_candidate_tokens, &candidate_tokens))
          return Status::kLimitExceeded;
        std::string joined(prefix);
        joined.append(candidate);
        std::vector<llama_token> sequence;
        if (!Tokenize(vocab, joined, limits.context_tokens - 1, &sequence)) return Status::kLimitExceeded;
        // A single explicit BOS anchors empty-prefix input. No chat template,
        // generated prompt, special-token parsing, or artificial leading space.
        sequence.insert(sequence.begin(), llama_vocab_bos(vocab));
        token_sequences.push_back(std::move(sequence));
      }
      std::size_t common_tokens = token_sequences.front().size();
      for (const auto& sequence : token_sequences) {
        common_tokens = std::min(common_tokens, sequence.size());
        std::size_t token = 0;
        while (token < common_tokens && sequence[token] == token_sequences.front()[token]) ++token;
        common_tokens = token;
      }
      // Score at least the last token even for a single/duplicate candidate.
      common_tokens = std::min(common_tokens, token_sequences.front().size() - 1);
      for (const auto& sequence : token_sequences) {
        if (sequence.size() - common_tokens > limits.max_candidate_tokens) return Status::kLimitExceeded;
      }
      auto parameters = llama_context_default_params();
      parameters.n_ctx = limits.context_tokens;
      parameters.n_batch = limits.context_tokens;
      parameters.n_ubatch = std::min<std::uint32_t>(64, limits.context_tokens);
      parameters.n_seq_max = 1;
      parameters.n_threads = static_cast<std::int32_t>(limits.worker_threads);
      parameters.n_threads_batch = static_cast<std::int32_t>(limits.worker_threads);
      parameters.offload_kqv = false;
      parameters.flash_attn = false;
      parameters.no_perf = true;
      parameters.embeddings = false;
      parameters.abort_callback = AbortDecode;
      parameters.abort_callback_data = &abort;
      Context ctx(llama_init_from_model(impl_->model->impl_->model.get(), parameters));
      if (abort.Stop()) return abort.reason.load();
      if (!ctx) return Status::kInternalError;
      auto status = Decode(ctx.get(), token_sequences.front(), 0, common_tokens, false, &abort);
      if (status != Status::kOk) return status;
      const auto vocabulary_tokens = llama_vocab_n_tokens(vocab);
      const auto* logits = llama_get_logits_ith(ctx.get(), -1);
      if (!logits) return Status::kInternalError;
      const std::vector<float> shared_logits(logits, logits + vocabulary_tokens);
      for (std::size_t candidate_index = 0; candidate_index < token_sequences.size(); ++candidate_index) {
        if (abort.Stop()) return abort.reason.load();
        const auto& sequence = token_sequences[candidate_index];
        const auto suffix_tokens = sequence.size() - common_tokens;
        if (!llama_kv_cache_seq_rm(ctx.get(), 0, static_cast<llama_pos>(common_tokens), -1))
          return Status::kInternalError;
        double score = 0;
        if (suffix_tokens && !LogProbability(shared_logits.data(), vocabulary_tokens,
            sequence[common_tokens], &score, &abort))
          return abort.reason.load() == Status::kOk ? Status::kInternalError : abort.reason.load();
        if (suffix_tokens > 1) {
          status = Decode(ctx.get(), sequence, common_tokens, suffix_tokens - 1, true, &abort);
          if (status != Status::kOk) return status;
          for (std::size_t token = 1; token < suffix_tokens; ++token) {
            double probability = 0;
            if (!LogProbability(llama_get_logits_ith(ctx.get(), static_cast<std::int32_t>(token - 1)),
                vocabulary_tokens, sequence[common_tokens + token], &probability, &abort))
              return abort.reason.load() == Status::kOk ? Status::kInternalError : abort.reason.load();
            score += probability;
          }
        }
        result.ranking.push_back({candidate_index, score, static_cast<std::uint32_t>(suffix_tokens)});
      }
      // ctx is destroyed before the final deadline/version check. No request
      // state survives in the reranker; weights remain shared and unchanged.
      std::stable_sort(result.ranking.begin(), result.ranking.end(), [](const auto& a, const auto& b) {
        return a.log_probability > b.log_probability;
      });
      return Status::kOk;
    };
    return finish(work());
  } catch (...) { return finish(Status::kInternalError); }
}

const char* StatusName(Status status) noexcept {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kUnavailable: return "unavailable";
    case Status::kInvalidArgument: return "invalid_argument";
    case Status::kInvalidModel: return "invalid_model";
    case Status::kPrivacyDenied: return "privacy_denied";
    case Status::kLimitExceeded: return "limit_exceeded";
    case Status::kCancelled: return "cancelled";
    case Status::kDeadlineExceeded: return "deadline_exceeded";
    case Status::kStaleRequest: return "stale_request";
    case Status::kBusy: return "busy";
    case Status::kInternalError: return "internal_error";
  }
  return "unknown";
}
} // namespace epocanva::neural
