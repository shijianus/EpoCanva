// EpoCanva P1.2 -- minimal librime lifecycle check.
//
// Drives the public C API of the pinned librime build through
//   get_api -> setup -> initialize -> create_session -> verify -> destroy_session
//   -> finalize
// using only project-local test directories.
//
// Deliberately out of scope: this program never reads user input, the clipboard,
// a user dictionary or an existing Rime configuration directory, and it starts no
// maintenance and no dictionary deployment. A pass here proves the engine
// initialises and that a session can be created, inspected and released. It says
// nothing about key handling, candidates or correction, which are not implemented.
//
// Units (AGENTS.md rule 5): strings crossing the C API are UTF-8 code units
// (bytes) and every buffer_size argument counts bytes too. rime_api.h annotates
// neither, so the units are stated here.
//
// Ownership across the DLL boundary (verified at commit 33e78140 in
// src/rime_api_impl.h): get_status/get_context/get_commit allocate with
// `new char[]` inside rime.dll and the matching free_* call `delete[]` inside
// rime.dll. The client must therefore never free(), delete[] or overwrite those
// pointers, and must call the free_* slot for every successful get_*.

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rime_api.h"

namespace {

// A distinct non-zero code per stage, so a report never has to infer the failing
// step from prose.
enum ExitCode : int {
  kOk = 0,
  kUsage = 2,
  kApiUnavailable = 3,
  kLoadedModuleMismatch = 4,
  kTestDirectorySetupFailed = 5,
  kTraitsNotApplied = 6,
  kCreateSessionFailed = 7,
  kFindSessionFailed = 8,
  kGetStatusFailed = 9,
  kGetContextFailed = 10,
  kCommitQueueNotEmpty = 11,
  kDestroySessionFailed = 12,
  kSessionAliveAfterFinalize = 13,
  kEngineRestartedAfterFinalize = 14,
  kLogFilesWritten = 15,
};

constexpr std::size_t kPathBuffer = 4096;
// rime_api.h has no named invalid-session constant; RimeFindSession treats id 0
// as unknown (src/rime_api_impl.h:153-155), which is what this checks against.
constexpr RimeSessionId kNoSession = 0;

struct Options {
  std::wstring expect_dll;
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
};

void Log(const char* stage, const char* detail) {
  std::printf("[lifecycle] %-26s %s\n", stage, detail);
}

void Fail(ExitCode code, const char* stage, const char* detail) {
  std::printf("[lifecycle] FAIL %-22s %s (stage code %d)\n", stage, detail,
              static_cast<int>(code));
}

bool IsAsciiOnly(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

// Widens an ASCII-only argv byte string. The C API takes const char* and
// interprets it with the process ANSI code page, so non-ASCII paths are refused
// rather than silently converted.
std::wstring Widen(const std::string& ascii) {
  return std::wstring(ascii.begin(), ascii.end());
}

std::wstring NormalizeWide(const std::wstring& raw) {
  std::vector<wchar_t> buffer(kPathBuffer);
  const DWORD n = GetFullPathNameW(raw.c_str(),
                                   static_cast<DWORD>(buffer.size()),
                                   buffer.data(), nullptr);
  if (n == 0 || n >= buffer.size()) {
    return raw;
  }
  return std::wstring(buffer.data(), n);
}

// Finds the module that actually supplies the code behind an API function
// pointer, so a stale rime.dll reachable through PATH can never be mistaken for
// the library this run was linked against.
std::wstring LoadedModulePath(const void* address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module) {
    return std::wstring();
  }
  std::vector<wchar_t> buffer(kPathBuffer, L'\0');
  const DWORD n = GetModuleFileNameW(module, buffer.data(),
                                     static_cast<DWORD>(buffer.size()));
  if (n == 0 || n >= buffer.size()) {
    return std::wstring();
  }
  return NormalizeWide(std::wstring(buffer.data(), n));
}

int CountFilesBeneath(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return 0;
  }
  std::error_code bec;
  std::filesystem::recursive_directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, bec);
  if (bec) {
    return -1;
  }
  int count = 0;
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code fec;
    if (it->is_regular_file(fec) && !fec) {
      ++count;
    }
  }
  return count;
}

bool ParseArgs(int argc, char** argv, Options* out, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) {
      *error = "option " + key + " has no value";
      return false;
    }
    const std::string value = argv[++i];
    if (!IsAsciiOnly(value)) {
      *error = "option " + key + " is not ASCII-only";
      return false;
    }
    if (key == "--expect-dll") {
      out->expect_dll = NormalizeWide(Widen(value));
    } else if (key == "--shared-dir") {
      out->shared_dir = value;
    } else if (key == "--user-dir") {
      out->user_dir = value;
    } else if (key == "--log-dir") {
      out->log_dir = value;
    } else {
      *error = "unknown option " + key;
      return false;
    }
  }
  if (out->expect_dll.empty() || out->shared_dir.empty() ||
      out->user_dir.empty() || out->log_dir.empty()) {
    *error = "--expect-dll, --shared-dir, --user-dir and --log-dir are all required";
    return false;
  }
  return true;
}

ExitCode VerifyModuleIdentity(const Options& opt, RimeApi* api) {
  if (!api->get_version) {
    Fail(kLoadedModuleMismatch, "module.identity", "get_version slot is NULL");
    return kLoadedModuleMismatch;
  }
  const std::wstring actual =
      LoadedModulePath(reinterpret_cast<const void*>(api->get_version));
  if (actual.empty()) {
    Fail(kLoadedModuleMismatch, "module.identity",
         "cannot resolve the module owning rime_get_api()");
    return kLoadedModuleMismatch;
  }
  if (_wcsicmp(actual.c_str(), opt.expect_dll.c_str()) != 0) {
    std::printf("[lifecycle] FAIL module.identity\n");
    std::printf("  expected: %ls\n  actual  : %ls\n", opt.expect_dll.c_str(),
                actual.c_str());
    Fail(kLoadedModuleMismatch, "module.identity",
         "loaded module is not the artefact this program was built against");
    return kLoadedModuleMismatch;
  }
  std::printf("[lifecycle] module-loaded    %ls\n", actual.c_str());
  std::printf("[lifecycle] librime-version  %s\n", api->get_version());
  return kOk;
}

ExitCode VerifyTraitsApplied(RimeApi* api, const Options& opt) {
  if (!RIME_API_AVAILABLE(api, get_shared_data_dir_s) ||
      !RIME_API_AVAILABLE(api, get_user_data_dir_s)) {
    Fail(kTraitsNotApplied, "traits.readback",
         "the loaded API table has no *_dir_s accessors");
    return kTraitsNotApplied;
  }
  // buffer_size counts bytes and the implementation strncpy()s, which does not
  // guarantee termination when it truncates; a full buffer is treated as failure.
  std::vector<char> buffer(kPathBuffer, '\0');
  api->get_shared_data_dir_s(buffer.data(), buffer.size());
  if (strnlen(buffer.data(), buffer.size()) >= buffer.size() - 1 ||
      strcmp(buffer.data(), opt.shared_dir.string().c_str()) != 0) {
    std::printf("  expected: %s\n  actual  : %s\n",
                opt.shared_dir.string().c_str(), buffer.data());
    Fail(kTraitsNotApplied, "traits.readback",
         "shared_data_dir did not take effect");
    return kTraitsNotApplied;
  }
  buffer.assign(kPathBuffer, '\0');
  api->get_user_data_dir_s(buffer.data(), buffer.size());
  if (strnlen(buffer.data(), buffer.size()) >= buffer.size() - 1 ||
      strcmp(buffer.data(), opt.user_dir.string().c_str()) != 0) {
    std::printf("  expected: %s\n  actual  : %s\n",
                opt.user_dir.string().c_str(), buffer.data());
    Fail(kTraitsNotApplied, "traits.readback", "user_data_dir did not take effect");
    return kTraitsNotApplied;
  }
  Log("traits.readback", "shared and user data dirs match what was set");
  return kOk;
}

// Steps 5 to 7 of the brief. *created_out keeps the id that was handed out even
// after it has been released, so the caller can assert post-finalize that it is
// gone; *still_alive reports whether the caller still has to destroy it.
ExitCode VerifySession(RimeApi* api, RimeSessionId* created_out, bool* still_alive) {
  const RimeSessionId created = api->create_session();
  if (created == kNoSession) {
    std::printf("[lifecycle] FAIL session.create\n");
    std::printf("  create_session() returned the invalid id while shared_data_dir "
                "was empty; the minimal configuration a session actually needs is "
                "a deployable default.yaml plus the schema/dictionary it names "
                "(stage code %d)\n",
                static_cast<int>(kCreateSessionFailed));
    *created_out = kNoSession;
    *still_alive = false;
    return kCreateSessionFailed;
  }
  *created_out = created;
  *still_alive = true;
  std::printf("[lifecycle] session.create     id=%llu\n",
              static_cast<unsigned long long>(created));

  if (!api->find_session(created)) {
    Fail(kFindSessionFailed, "session.find",
         "find_session() rejected the id create_session() just returned");
    return kFindSessionFailed;
  }
  Log("session.find", "find_session() returned True");

  {
    RIME_STRUCT(RimeStatus, status);
    if (!api->get_status(created, &status)) {
      Fail(kGetStatusFailed, "session.status", "get_status() returned False");
      return kGetStatusFailed;
    }
    std::printf("[lifecycle] session.status     schema_id=%s disabled=%d "
                "composing=%d\n",
                status.schema_id ? status.schema_id : "(null)",
                status.is_disabled, status.is_composing);
    const bool has_id = (status.schema_id && status.schema_id[0]);
    const bool enabled = (status.is_disabled == False);
    api->free_status(&status);
    if (!has_id) {
      Fail(kGetStatusFailed, "session.status",
           "a live session reported an empty schema_id");
      return kGetStatusFailed;
    }
    if (!enabled) {
      Fail(kGetStatusFailed, "session.status",
           "the service reports itself disabled right after initialize()");
      return kGetStatusFailed;
    }
    Log("session.status", "free_status() returned True");
  }

  {
    RIME_STRUCT(RimeContext, context);
    if (!api->get_context(created, &context)) {
      Fail(kGetContextFailed, "session.context", "get_context() returned False");
      return kGetContextFailed;
    }
    const bool has_preedit = (context.composition.preedit != nullptr);
    const int candidates = context.menu.num_candidates;
    api->free_context(&context);
    std::printf("[lifecycle] session.context    preedit=%s num_candidates=%d\n",
                has_preedit ? "present" : "none", candidates);
    // A session that was never given a key must have nothing composed and no
    // menu. Any text or candidate here would mean the reported state is invented.
    if (has_preedit || candidates != 0) {
      Fail(kGetContextFailed, "session.context",
           "a brand new session reported text or candidates without any input");
      return kGetContextFailed;
    }
    Log("session.context", "free_context() returned True");
  }

  {
    RIME_STRUCT(RimeCommit, commit);
    if (api->get_commit(created, &commit)) {
      const bool had_text = (commit.text != nullptr);
      api->free_commit(&commit);
      std::printf("[lifecycle] FAIL session.commit: get_commit() returned %s "
                  "although no key was ever sent\n",
                  had_text ? "text" : "True with a null text pointer");
      return kCommitQueueNotEmpty;
    }
    Log("session.commit", "get_commit() returned False, as expected");
  }

  if (!api->destroy_session(created)) {
    Fail(kDestroySessionFailed, "session.destroy",
         "destroy_session() returned False");
    // Leave still_alive set: the id may or may not be gone, and the caller's
    // cleanup path reports what it finds instead of assuming success.
    return kDestroySessionFailed;
  }
  if (api->find_session(created)) {
    Fail(kDestroySessionFailed, "session.destroy",
         "the destroyed id is still resolvable");
    *still_alive = false;
    return kDestroySessionFailed;
  }
  *still_alive = false;
  Log("session.destroyed",
      "destroy_session() True and the id no longer resolves");
  return kOk;
}

ExitCode VerifyAfterFinalize(RimeApi* api, RimeSessionId previously_created) {
  if (previously_created != kNoSession &&
      api->find_session(previously_created)) {
    Fail(kSessionAliveAfterFinalize, "post.finalize",
         "an id created before finalize() is still resolvable");
    return kSessionAliveAfterFinalize;
  }
  const RimeSessionId late = api->create_session();
  if (late != kNoSession) {
    api->destroy_session(late);
    Fail(kEngineRestartedAfterFinalize, "post.finalize",
         "create_session() still succeeds after finalize()");
    return kEngineRestartedAfterFinalize;
  }
  Log("post.finalize", "create_session() refused; the service is down");
  return kOk;
}

ExitCode VerifyNoLogFiles(const Options& opt) {
  const int files = CountFilesBeneath(opt.log_dir);
  // Built with ENABLE_LOGGING=OFF, so the body of SetupLogging() is compiled
  // away (third_party/librime/src/rime/setup.cc:71-95). Nothing may land here.
  if (files != 0) {
    std::printf("[lifecycle] FAIL post.finalize.log: %d file(s) written under "
                "the log directory\n",
                files);
    return kLogFilesWritten;
  }
  Log("post.finalize", "log directory stayed empty");
  return kOk;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string error;
  if (!ParseArgs(argc, argv, &opt, &error)) {
    std::printf("usage: rime_lifecycle_smoke --expect-dll <path> "
                "--shared-dir <path> --user-dir <path> --log-dir <path>\n");
    std::printf("[lifecycle] FAIL arguments: %s\n", error.c_str());
    return kUsage;
  }

  for (const std::filesystem::path& d :
       {opt.shared_dir, opt.user_dir, opt.log_dir}) {
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    if (ec) {
      std::printf("[lifecycle] FAIL test.directories: cannot create %s (%s)\n",
                  d.string().c_str(), ec.message().c_str());
      return kTestDirectorySetupFailed;
    }
  }

  // --- 1. acquire the API table -------------------------------------------------
  RimeApi* api = rime_get_api();
  if (!api) {
    Fail(kApiUnavailable, "api.acquire", "rime_get_api() returned NULL");
    return kApiUnavailable;
  }
  Log("api.acquire", "rime_get_api() returned a table");

  // --- 2. prove which binary supplied that table --------------------------------
  ExitCode rc = VerifyModuleIdentity(opt, api);
  if (rc != kOk) {
    return rc;
  }

  // --- 3. traits, all paths project-local --------------------------------------
  // RimeTraits holds borrowed const char*, so the strings it points at must
  // outlive setup() and initialize(); they are named locals rather than
  // temporaries of the assignment statements.
  const std::string shared_s = opt.shared_dir.string();  // UTF-8 bytes / ANSI bytes
  const std::string user_s = opt.user_dir.string();
  const std::string log_s = opt.log_dir.string();

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared_s.c_str();
  traits.user_data_dir = user_s.c_str();
  traits.log_dir = log_s.c_str();
  traits.prebuilt_data_dir = nullptr;  // librime defaults to <shared>/build
  traits.staging_dir = nullptr;        // librime defaults to <user>/build
  traits.app_name = "rime.epocanva.lifecycle_smoke";
  traits.distribution_name = "EpoCanva";
  traits.distribution_code_name = "epocanva";
  traits.distribution_version = "0.0.0-p1.2";
  traits.min_log_level = 3;            // FATAL; inert while logging is OFF
  traits.modules = nullptr;            // librime's own default module group

  // --- 4. setup + initialize ----------------------------------------------------
  // initialize() is void, so it cannot report failure across the C boundary. What
  // it does leave behind is observable: the traits readback below shows the
  // deployer really took the settings, and create_session() shows the service came
  // up. Cleanup is driven by those observations, never by an assumed happy path.
  bool initialize_called = false;
  api->setup(&traits);
  Log("engine.setup", "setup() returned");
  api->initialize(&traits);
  initialize_called = true;
  Log("engine.initialize", "initialize() returned");

  rc = VerifyTraitsApplied(api, opt);
  RimeSessionId created = kNoSession;
  bool session_alive = false;
  if (rc == kOk) {
    // --- 5..7 session lifecycle -------------------------------------------------
    rc = VerifySession(api, &created, &session_alive);
  }

  // --- 8. finalize exactly what was brought up ---------------------------------
  if (session_alive) {
    if (api->destroy_session(created)) {
      Log("cleanup.session", "destroyed on the failure path");
    } else {
      std::printf("[lifecycle] cleanup.session destroy_session() also failed\n");
    }
    session_alive = false;
  }
  if (initialize_called) {
    api->finalize();
    Log("cleanup.finalize", "finalize() returned");
    const ExitCode post = VerifyAfterFinalize(api, created);
    if (rc == kOk && post != kOk) {
      rc = post;
    }
  } else if (rc == kOk) {
    rc = kApiUnavailable;
    std::printf("[lifecycle] FAIL cleanup: initialize() was never called\n");
  }

  const ExitCode logs = VerifyNoLogFiles(opt);
  if (rc == kOk && logs != kOk) {
    rc = logs;
  }

  if (rc == kOk) {
    Log("RESULT", "PASS full lifecycle");
  }
  return rc;
}
