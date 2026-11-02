#ifndef EPOCANVA_ENGINE_ADAPTER_RIME_ADAPTER_H_
#define EPOCANVA_ENGINE_ADAPTER_RIME_ADAPTER_H_

// Private C++ seam. The ONLY backend is the existing Windows RimeRuntime/RimeSession.
// This is not an injectable fake Session or a second runtime implementation.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epocanva_engine.h"

namespace epocanva {
namespace engine {
namespace adapter {

struct Candidate {
  std::string text_utf8;
  std::string comment_utf8;
};

struct Snapshot {
  std::string schema_id_utf8;
  std::string schema_name_utf8;
  std::string raw_input_utf8;
  std::string preedit_utf8;
  std::uint64_t cursor_utf8_byte_offset = EPOCANVA_NO_UTF8_BYTE_OFFSET;
  std::uint64_t selection_start_utf8_byte_offset = EPOCANVA_NO_UTF8_BYTE_OFFSET;
  std::uint64_t selection_end_utf8_byte_offset = EPOCANVA_NO_UTF8_BYTE_OFFSET;
  bool is_composing = false;
  bool service_disabled = false;
  std::uint32_t highlighted_candidate_ordinal_on_page = EPOCANVA_NO_CANDIDATE_ORDINAL;
  std::uint32_t menu_page_ordinal = 0;
  std::uint32_t menu_page_capacity = 0;
  bool is_last_page = false;
  std::vector<Candidate> candidates;
};

struct Commit {
  bool present = false;
  std::string text_utf8;
};

struct RuntimeInfo {
  epocanva_engine_info counters{};
  std::string loaded_dll_path_utf8;
  std::string rime_version_utf8;
};

// Uses the existing strict Windows converter; explicit UTF-8 byte count, no NUL.
// A malformed sequence is never repaired. Strings are not logged.
epocanva_status CopyUtf8Identifier(epocanva_utf8_view view, std::string* output);

class Session;
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  epocanva_status Start(const epocanva_engine_options& options);
  epocanva_status Shutdown();
  epocanva_status CreateSession(const std::string& schema_id, Session* output);
  epocanva_status ReadInfo(RuntimeInfo* output) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  epocanva_status ProcessKey(std::uint32_t keysym, std::uint32_t modifiers,
                            bool* handled);
  epocanva_status SelectCandidate(std::uint32_t candidate_ordinal_on_page,
                                 bool* accepted);
  epocanva_status Capture(Snapshot* output);
  epocanva_status ReadCommit(Commit* output);
  epocanva_status Close();

 private:
  friend class Runtime;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace adapter
}  // namespace engine
}  // namespace epocanva
#endif
