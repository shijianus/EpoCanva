#include "lexical_reranker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

using namespace epocanva::neural;
namespace {
constexpr char kModelSha[] = "4b965ca8e4a95be38f3223bbb512cc73112efaef00c0313b7e618f0835c504c0";
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}
void CheckStatus(const Result& result, Status expected, const char* label) {
  Check(result.status == expected && (expected == Status::kOk || result.ranking.empty()), label);
  std::cout << "case=" << label << " status=" << StatusName(result.status)
            << " elapsed_us=" << result.elapsed_microseconds << '\n';
}
std::vector<std::size_t> Indices(const Result& value) {
  std::vector<std::size_t> result;
  for (const auto& item : value.ranking) result.push_back(item.candidate_index);
  return result;
}
std::uint64_t PeakBytes() {
  PROCESS_MEMORY_COUNTERS_EX memory{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
    return 0;
  return memory.PeakWorkingSetSize;
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--force-failure") {
    Check(false, "forced_assertion_failure");
    return failures ? 1 : 0;
  }
  if (argc != 2 || !argv[1][0]) { std::cerr << "FAIL missing_explicit_model_resource\n"; return 2; }
  Status status = Status::kInternalError;
  const auto path = std::filesystem::u8path(argv[1]);
  const auto started = std::chrono::steady_clock::now();
  auto model = Model::Load(path, kModelSha, &status);
  Check(model && status == Status::kOk, "model_load");
  if (!model) { std::cerr << "FAIL model_load_status=" << StatusName(status) << '\n'; return 1; }
  const auto model_load_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
  std::cout << "model_load_us=" << model_load_us << " model_bytes=" << model->info().file_bytes
            << " parameters=" << model->info().parameter_count << " vocab_tokens=" << model->info().vocabulary_tokens << '\n';
  Check(model->info().file_bytes == 531067776 && model->info().vocabulary_tokens == 151936 &&
        model->info().training_context_tokens == 32768, "model_metadata");

  HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, 0, nullptr);
  Check(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "weights_locked_readonly");
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);

  Check(!Model::Load("relative.gguf", kModelSha, &status) && status == Status::kInvalidModel, "relative_resource_rejected");
  Check(!Model::Load(path, "bad", &status) && status == Status::kInvalidArgument, "invalid_digest_rejected");
  std::atomic<bool> stop{true};
  RequestControl control;
  control.cancelled = &stop;
  Check(!Model::Load(path, kModelSha, &status, control) && status == Status::kCancelled, "load_cancelled");

  Limits limits;
  limits.max_request_time = std::chrono::seconds(30); // Explicit correctness budget, not product default.
  auto scorer = LexicalReranker::Create(model, limits, &status);
  Check(scorer && status == Status::kOk, "scorer_create");
  if (!scorer) return 1;
  const std::vector<std::string_view> candidates{"banana", "Paris", "London"};
  auto result = scorer->Rerank("The capital of France is ", candidates, true);
  CheckStatus(result, Status::kOk, "real_scoring");
  Check(result.ranking.size() == candidates.size(), "candidate_permutation");
  auto sorted = Indices(result);
  std::sort(sorted.begin(), sorted.end());
  Check(sorted == std::vector<std::size_t>({0, 1, 2}), "unchanged_lexical_candidate_set");
  if (result.ranking.size() == candidates.size()) {
    Check(result.ranking.front().candidate_index == 1, "synthetic_factual_fixture_ranks_first");
    for (const auto& item : result.ranking) {
      Check(std::isfinite(item.log_probability) && item.log_probability <= 0 && item.scored_tokens > 0,
            "finite_real_token_scores");
      std::cout << "fixture=capital candidate_index=" << item.candidate_index
                << " log_probability=" << item.log_probability << " scored_tokens=" << item.scored_tokens << '\n';
    }
  }
  const auto again = scorer->Rerank("The capital of France is ", candidates, true);
  CheckStatus(again, Status::kOk, "repeat_scoring");
  Check(Indices(result) == Indices(again), "deterministic_order");
  if (result.ranking.size() == again.ranking.size()) {
    for (std::size_t i = 0; i < result.ranking.size(); ++i)
      Check(std::abs(result.ranking[i].log_probability - again.ranking[i].log_probability) <= 1e-6,
            "deterministic_scores");
  }
  const auto duplicate = scorer->Rerank("The capital of France is ", {"Paris", "Paris"}, true);
  CheckStatus(duplicate, Status::kOk, "duplicate_tie");
  Check(Indices(duplicate) == std::vector<std::size_t>({0, 1}), "stable_tie_order");
  const auto multibyte = scorer->Rerank(u8"Bonjour, ", {u8"école", u8"étude"}, true);
  CheckStatus(multibyte, Status::kOk, "utf8_candidates");
  const auto merged_boundary = scorer->Rerank("hel", {"lo", "p"}, true);
  CheckStatus(merged_boundary, Status::kOk, "joint_tokenization_boundary");

  // Denial must happen without dereferencing content, including invalid pointers.
  std::string_view unreadable(reinterpret_cast<const char*>(1), 16);
  CheckStatus(scorer->Rerank(unreadable, {unreadable}, false), Status::kPrivacyDenied, "privacy_denied_before_read");
  CheckStatus(scorer->Rerank("x", {}, true), Status::kInvalidArgument, "empty_candidates");
  CheckStatus(scorer->Rerank("x", {""}, true), Status::kInvalidArgument, "empty_candidate");
  const std::string invalid_utf8("\xc0\x80", 2);
  CheckStatus(scorer->Rerank(invalid_utf8, {"x"}, true), Status::kInvalidArgument, "invalid_utf8");
  const std::string long_prefix(2049, 'x');
  CheckStatus(scorer->Rerank(long_prefix, {"x"}, true), Status::kLimitExceeded, "prefix_byte_limit");
  const std::string long_candidate(129, 'x');
  CheckStatus(scorer->Rerank("", {long_candidate}, true), Status::kLimitExceeded, "candidate_byte_limit");
  CheckStatus(scorer->Rerank("x", {"a","b","c","d","e","f","g","h","i"}, true),
              Status::kLimitExceeded, "candidate_count_limit");
  Limits invalid = limits;
  invalid.context_tokens = 513;
  Check(!LexicalReranker::Create(model, invalid, &status) && status == Status::kInvalidArgument,
        "context_token_limit");

  CheckStatus(scorer->Rerank("x", candidates, true, control), Status::kCancelled, "pre_cancel");
  stop.store(false);
  control.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
  CheckStatus(scorer->Rerank("x", candidates, true, control), Status::kDeadlineExceeded, "past_deadline");
  control.deadline = std::chrono::steady_clock::time_point::max();
  std::atomic<std::uint64_t> version{2};
  control.current_version = &version;
  control.expected_version = 1;
  CheckStatus(scorer->Rerank("x", candidates, true, control), Status::kStaleRequest, "stale_version");
  control.current_version = nullptr;

  std::string context;
  for (int i = 0; i < 40; ++i) context += "This is a public synthetic test sentence. ";
  std::thread cancel_thread([&stop] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); stop.store(true, std::memory_order_release);
  });
  const auto active_cancel = scorer->Rerank(context, candidates, true, control);
  cancel_thread.join();
  CheckStatus(active_cancel, Status::kCancelled, "active_cancel");
  stop.store(false);
  version.store(1);
  control.current_version = &version;
  control.expected_version = 1;
  std::thread supersede_thread([&version] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); version.store(2, std::memory_order_release);
  });
  const auto superseded = scorer->Rerank(context, candidates, true, control);
  supersede_thread.join();
  CheckStatus(superseded, Status::kStaleRequest, "active_supersede");
  control = {};
  control.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  CheckStatus(scorer->Rerank(context, candidates, true, control), Status::kDeadlineExceeded, "active_deadline");

  // A separate instance shares only weights; abandoned calls never contaminate it.
  auto second = LexicalReranker::Create(model, limits, &status);
  Check(second && status == Status::kOk, "shared_model_second_instance");
  if (second) {
    const auto clean = second->Rerank("The capital of France is ", candidates, true);
    CheckStatus(clean, Status::kOk, "clean_after_cancel");
    Check(Indices(clean) == Indices(result), "no_cross_request_state");
  }
  auto default_scorer = LexicalReranker::Create(model, Limits{}, &status);
  Check(default_scorer && status == Status::kOk, "default_limits");
  if (default_scorer) {
    const auto timed = default_scorer->Rerank("The capital of France is ", candidates, true);
    Check(timed.status == Status::kOk || timed.status == Status::kDeadlineExceeded, "default_200ms_fallback");
    Check(timed.status == Status::kOk || timed.ranking.empty(), "timeout_has_no_partial_ranking");
    std::cout << "case=default_200ms_measurement status=" << StatusName(timed.status)
              << " elapsed_us=" << timed.elapsed_microseconds << '\n';
  }
  std::cout << "peak_working_set_bytes=" << PeakBytes() << '\n';
  std::cout << "failures=" << failures << '\n';
  return failures ? 1 : 0;
}
