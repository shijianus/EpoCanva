// Value configuration and revocable access for the Windows unified backend adapter.
#ifndef EPOCANVA_WINDOWS_ENGINE_EXECUTOR_BACKEND_CONFIG_H_
#define EPOCANVA_WINDOWS_ENGINE_EXECUTOR_BACKEND_CONFIG_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "../../../engine/include/epocanva_backend.h"

namespace epocanva::windows::engine {

struct BackendProfileConfig {
  std::uint32_t profile_id = 1;
  std::uint32_t language = EPOCANVA_BACKEND_EN;
  std::uint32_t input_mode = EPOCANVA_BACKEND_LATIN;
  std::uint32_t output_mode = EPOCANVA_BACKEND_OUTPUT_LATIN;
  std::uint32_t keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
  std::string variant_id = "en-US";
  std::filesystem::path abbreviation_path;
  std::string abbreviation_sha256;
  bool operator==(const BackendProfileConfig& rhs) const {
    return std::tie(profile_id, language, input_mode, output_mode, keyboard_layout,
                    variant_id, abbreviation_path, abbreviation_sha256) ==
           std::tie(rhs.profile_id, rhs.language, rhs.input_mode, rhs.output_mode,
                    rhs.keyboard_layout, rhs.variant_id, rhs.abbreviation_path,
                    rhs.abbreviation_sha256);
  }
};

struct BackendResourceConfig {
  BackendProfileConfig profile;
  std::filesystem::path model_path;
  std::string model_sha256;
  std::filesystem::path conversion_path;
  std::string conversion_sha256;
  std::filesystem::path auxiliary_path;
  std::string auxiliary_sha256;
  bool operator==(const BackendResourceConfig& rhs) const {
    return profile == rhs.profile &&
        std::tie(model_path, model_sha256, conversion_path, conversion_sha256,
                 auxiliary_path, auxiliary_sha256) ==
        std::tie(rhs.model_path, rhs.model_sha256, rhs.conversion_path,
                 rhs.conversion_sha256, rhs.auxiliary_path, rhs.auxiliary_sha256);
  }
};

struct UnifiedBackendConfig {
  bool enabled = false;
  bool use_rime = false;
  bool allow_test_fixture = false;
  std::vector<BackendResourceConfig> profiles;
  std::uint32_t active_profile_id = 1;
  std::uint32_t max_candidates = 8;  // candidate records
  bool completion_enabled = true;
  bool spelling_enabled = false;
  bool morphology_enabled = false;
  // Mirrors ProductSettings: contextual abbreviation expansion is on by default.
  bool abbreviation_enabled = true;
  // Existing local preferences are read for ranking whenever this root is supplied,
  // even while recording is paused. Writes additionally require both flags below.
  std::filesystem::path learning_root;
  bool learning_recording_enabled = false;
  // Hard gate: product builds leave this false because no OS-enforced field-permission
  // lock covers the queued worker call chain. Tests and a future proven lock may set it.
  bool learning_writes_trusted = false;
  bool operator==(const UnifiedBackendConfig& rhs) const {
    return std::tie(enabled, use_rime, allow_test_fixture, active_profile_id,
                    max_candidates, completion_enabled, spelling_enabled,
                    morphology_enabled, abbreviation_enabled, learning_root,
                    learning_recording_enabled, learning_writes_trusted) ==
           std::tie(rhs.enabled, rhs.use_rime, rhs.allow_test_fixture,
                    rhs.active_profile_id, rhs.max_candidates, rhs.completion_enabled,
                    rhs.spelling_enabled, rhs.morphology_enabled, rhs.abbreviation_enabled,
                    rhs.learning_root, rhs.learning_recording_enabled,
                    rhs.learning_writes_trusted) &&
           profiles == rhs.profiles;
  }
};

// Contains no COM object. Revoked on target loss/privacy refusal before retirement.
// This only cancels queued work; it is not an OS field lock. Product operations therefore
// disable personal learning. Results still require the STA's live target/permission gates.
struct BackendAccess {
  std::uint64_t context_target = 0;
  std::uint64_t generation = 0;
  std::shared_ptr<std::atomic<bool>> current;
  bool allowed() const {
    return context_target != 0 && generation != 0 && current &&
           current->load(std::memory_order_acquire);
  }
};

enum class BackendOperation {
  kPushUtf8, kBackspace, kClearInput, kSetContext, kConfirmText,
  kQuery, kSelect, kAcknowledgeCommit, kObserveHostText
};
struct BackendWork {
  BackendOperation operation = BackendOperation::kQuery;
  BackendAccess access;
  std::string text_utf8;  // bounded transient UTF-8 bytes, never diagnostic text
  std::string preceding_utf8;  // bounded confirmed host context, never persisted
  std::uint32_t query_kind = EPOCANVA_BACKEND_COMPLETION;
  std::uint64_t event_token = 0;  // confirmed-edit ordinal, not a text offset
  std::uint64_t snapshot_token = 0;
  std::uint64_t candidate_token = 0;
  std::uint64_t commit_token = 0;
  bool host_applied = false;  // set only from the TSF edit callback's Applied result
};

}  // namespace epocanva::windows::engine
#endif
