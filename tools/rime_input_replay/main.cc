// EpoCanva P1.3 -- headless librime input-link replay.
//
// Drives the public C API of the pinned librime build through
//   get_api -> setup -> initialize -> deploy fixture -> create_session
//   -> select_schema -> process_key (one event at a time) -> get_status /
//   get_context / get_input / get_commit -> destroy_session -> finalize
// and asserts, per event, what the engine actually reported.
//
// WHAT THIS PROVES: keys reach a real registered component set, a real
// composition forms, backspace and cancel mutate it, and one explicit operation
// commits it exactly once -- for the ASCII fixture in tests/fixtures/rime_raw.
// See that README for the component-level justification.
//
// WHAT THIS DOES NOT PROVE: nothing about Unicode, CJK, word lists, prediction,
// completion, correction or learning. The fixture's candidate text is
// byte-identical to the injected keys, so no language behaviour is exercised.
// All test data is ASCII, so byte == UTF-8 code unit == grapheme for it; that is
// a property of the test data, not a claim about the engine (AGENTS.md rule 5).
//
// UNITS. Every string crossing the C API is UTF-8 code units (bytes) and every
// buffer_size argument counts bytes. RimeComposition.length / .cursor_pos /
// .sel_start / .sel_end are byte offsets into `preedit`. rime_api.h annotates
// none of this, so it is stated here.
//
// OWNERSHIP. Verified at commit 33e78140 in src/rime_api_impl.h:
//   get_status  -> `new char[]` inside rime.dll (:347-350), freed by free_status
//   get_context -> `new char[]` inside rime.dll (:223-232), freed by free_context
//   get_commit  -> `new char[]` inside rime.dll (:319-320), freed by free_commit
// This program therefore never calls free()/delete[] on an upstream pointer and
// calls the matching free_* slot for every successful get_*. get_input
// (:1062-1070) has NO free slot: it returns ctx->input().c_str(), so the pointer
// is copied immediately and never released. ApiBalance makes an unpaired get_*
// produce a non-zero exit code instead of remaining a prose promise.
//
// HONESTY. There is no path in this program that could imitate the engine: it
// never constructs a preedit or commit string itself, never substitutes an empty
// value after an API failure (each such failure returns a stage code and stops),
// and the negative run asserts the ABSENCE of engine behaviour with its own exit
// code rather than wrapping a failure in a passing test.

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rime_api.h"

namespace {

// One distinct non-zero code per structural stage. Value assertions share
// kAssertionFailed and are itemised in the printed per-event lines.
enum ExitCode : int {
  kOk = 0,
  kUsage = 2,
  kApiUnavailable = 3,
  kLoadedModuleMismatch = 4,
  kFixturePreconditionFailed = 5,
  kRunDirectorySetupFailed = 6,
  kTraitsNotApplied = 7,
  kDeployFailed = 8,
  kCreateSessionFailed = 9,
  kSchemaSelectionFailed = 10,
  kGetStatusFailed = 11,
  kGetContextFailed = 12,
  kGetCommitFailed = 13,
  kAssertionFailed = 14,
  kDestroySessionFailed = 15,
  kApiResultUnbalanced = 16,
  kLogFilesWritten = 17,
  kNegativeRunNotReproducible = 18,
  kSessionAliveAfterFinalize = 19,
  kEngineRestartedAfterFinalize = 20,
};

constexpr std::size_t kPathBuffer = 4096;
// rime_api.h defines no named invalid-session constant; RimeFindSession treats id
// 0 as unknown (src/rime_api_impl.h:153-155).
constexpr RimeSessionId kNoSession = 0;

// X11 keysyms, read out of the pinned checkout's vendored header rather than
// from memory: third_party/librime/include/X11/keysymdef.h
//   XK_BackSpace 0xff08 (:124)   XK_Return 0xff0d (:128)
//   XK_Escape    0xff1b (:132)   XK_space  0x0020 (:527)
// Printable ASCII keeps its code point. `mask` is librime's modifier bitmask
// (src/rime/key_table.h:15-43: bit 0 Shift, bit 2 Control, bit 30 Release);
// this program always sends mask = 0.
constexpr int kXkBackSpace = 0xff08;
constexpr int kXkReturn = 0xff0d;
constexpr int kXkEscape = 0xff1b;
constexpr int kXkSpace = 0x0020;

constexpr char kFixtureDefaultYaml[] = "default.yaml";
constexpr char kFixtureSchemaYaml[] = "epocanva_test_raw.schema.yaml";
constexpr char kCompiledDefaultYaml[] = "default.yaml";
constexpr char kCompiledSchemaYaml[] = "epocanva_test_raw.schema.yaml";
constexpr char kTestSchemaId[] = "epocanva_test_raw";
// A schema id that must not resolve to any file. It cannot collide with anything
// in the fixture directory.
constexpr char kBogusSchemaId[] = "epocanva_no_such_schema_p13";
// A run root is only deletable when its parent directory carries this exact name,
// which nothing in this repository produces except the generated CTest file.
constexpr char kRunRootGuard[] = "rime-input-replay-run";

struct Options {
  std::wstring expect_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path run_root;
  // The negative run: the fixture is deliberately absent and the required
  // outcome is that the engine cannot compose.
  bool expect_missing_fixture = false;
};

struct ApiBalance {
  int status_get = 0;
  int status_free = 0;
  int context_get = 0;
  int context_free = 0;
  int commit_get = 0;
  int commit_free = 0;

  bool balanced() const {
    return status_get == status_free && context_get == context_free &&
           commit_get == commit_free;
  }
  int outstanding() const {
    return (status_get - status_free) + (context_get - context_free) +
           (commit_get - commit_free);
  }
};

// Everything asserted about a session's state is filled in here from an API call;
// no field is ever computed by this program.
struct ObservedState {
  std::string schema_id;
  bool status_disabled = false;
  bool status_composing = false;
  bool has_preedit = false;
  std::string preedit;
  long preedit_length = -1;
  long preedit_cursor = -1;
  long preedit_sel_start = -1;
  long preedit_sel_end = -1;
  long num_candidates = 0;
  std::string first_candidate;
  std::string commit_text_preview;
  std::string raw_input;  // copied from get_input, which has no free slot
  bool commit_present = false;
  std::string commit_text;
};

void Log(const char* stage, const char* detail) {
  std::printf("[replay] %-24s %s\n", stage, detail);
}

void Fail(ExitCode code, const char* stage, const char* detail) {
  std::printf("[replay] FAIL %-20s %s (stage code %d)\n", stage, detail,
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
    if (key == "--expect-missing-fixture") {
      out->expect_missing_fixture = true;
      continue;
    }
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
    } else if (key == "--fixture-dir") {
      out->fixture_dir = value;
    } else if (key == "--run-root") {
      out->run_root = value;
    } else {
      *error = "unknown option " + key;
      return false;
    }
  }
  if (out->expect_dll.empty() || out->fixture_dir.empty() ||
      out->run_root.empty()) {
    *error = "--expect-dll, --fixture-dir and --run-root are all required";
    return false;
  }
  return true;
}

// --- run directory -------------------------------------------------------------
//
// Each run gets a private, rebuilt directory tree under the project's build
// directory. Nothing is read from a real Rime user directory and nothing is
// written outside it. The tree is deleted at the start of every run, so a
// previous run's compiled config can never satisfy an assertion: "does not depend
// on the previous cache" holds by construction, not by promise.
ExitCode PrepareRunRoot(const Options& opt, std::filesystem::path* shared_dir,
                        std::filesystem::path* user_dir,
                        std::filesystem::path* log_dir) {
  const std::filesystem::path parent = opt.run_root.parent_path();
  if (parent.filename().u8string() != kRunRootGuard) {
    std::printf("[replay] FAIL run.root: --run-root %s is not a leaf under a "
                "directory named '%s'; refusing to delete anything here\n",
                opt.run_root.string().c_str(), kRunRootGuard);
    return kRunDirectorySetupFailed;
  }
  std::error_code ec;
  if (std::filesystem::exists(opt.run_root, ec)) {
    std::filesystem::remove_all(opt.run_root, ec);
    if (ec) {
      std::printf("[replay] FAIL run.root: cannot remove the previous run "
                  "directory %s (%s)\n", opt.run_root.string().c_str(),
                  ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
  }
  *shared_dir = opt.run_root / "shared";
  *user_dir = opt.run_root / "user";
  *log_dir = opt.run_root / "log";
  for (const std::filesystem::path& d : {*shared_dir, *user_dir, *log_dir}) {
    std::filesystem::create_directories(d, ec);
    if (ec) {
      std::printf("[replay] FAIL run.root: cannot create %s (%s)\n",
                  d.string().c_str(), ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
  }
  std::printf("[replay] run-root             %s (recreated empty)\n",
              opt.run_root.string().c_str());
  return kOk;
}

// Copies the two fixture files by name so an unexpected extra file in the fixture
// directory cannot silently change what gets deployed.
ExitCode StageFixture(const Options& opt, const std::filesystem::path& shared_dir,
                      std::filesystem::path* schema_source_path) {
  const std::filesystem::path default_src = opt.fixture_dir / kFixtureDefaultYaml;
  const std::filesystem::path schema_src = opt.fixture_dir / kFixtureSchemaYaml;
  std::error_code ec;
  bool any_absent = false;
  for (const std::filesystem::path& p : {default_src, schema_src}) {
    if (!std::filesystem::is_regular_file(p, ec) || ec) {
      std::printf("[replay] fixture.absent       %s\n", p.string().c_str());
      any_absent = true;
    }
  }
  if (any_absent) {
    return kFixturePreconditionFailed;
  }
  for (const std::filesystem::path& p : {default_src, schema_src}) {
    const std::filesystem::path dest = shared_dir / p.filename();
    std::filesystem::copy_file(p, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      std::printf("[replay] FAIL fixture.copy: %s -> %s (%s)\n",
                  p.string().c_str(), dest.string().c_str(),
                  ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
    const auto size = static_cast<unsigned long long>(
        std::filesystem::file_size(dest, ec));
    std::printf("[replay] fixture.staged       %s (%llu bytes)\n",
                dest.string().c_str(), size);
  }
  *schema_source_path = shared_dir / kFixtureSchemaYaml;
  return kOk;
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
    std::printf("[replay] FAIL module.identity\n");
    std::printf("  expected: %ls\n  actual  : %ls\n", opt.expect_dll.c_str(),
                actual.c_str());
    Fail(kLoadedModuleMismatch, "module.identity",
         "loaded module is not the artefact this program was built against");
    return kLoadedModuleMismatch;
  }
  std::printf("[replay] module-loaded        %ls\n", actual.c_str());
  std::printf("[replay] librime-version      %s\n", api->get_version());
  return kOk;
}

// --- observation ---------------------------------------------------------------

// Returns false only when an API call itself failed, which the caller turns into
// a stage code. A successful call reporting "no preedit" is not a failure: that
// is the documented sentinel (composition.preedit == NULL; RimeGetContext fills
// it only while ctx->IsComposing(), src/rime_api_impl.h:220-224).
bool ReadObservedState(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
                       ObservedState* out, ExitCode* failure) {
  {
    RIME_STRUCT(RimeStatus, status);
    if (!api->get_status(sid, &status)) {
      *failure = kGetStatusFailed;
      return false;
    }
    ++balance->status_get;
    out->schema_id = status.schema_id ? status.schema_id : "";
    out->status_disabled = (status.is_disabled != False);
    out->status_composing = (status.is_composing != False);
    api->free_status(&status);
    ++balance->status_free;
  }
  {
    RIME_STRUCT(RimeContext, context);
    if (!api->get_context(sid, &context)) {
      *failure = kGetContextFailed;
      return false;
    }
    ++balance->context_get;
    out->has_preedit = (context.composition.preedit != nullptr);
    if (out->has_preedit) {
      out->preedit = context.composition.preedit;
    }
    out->preedit_length = context.composition.length;
    out->preedit_cursor = context.composition.cursor_pos;
    out->preedit_sel_start = context.composition.sel_start;
    out->preedit_sel_end = context.composition.sel_end;
    out->num_candidates = context.menu.num_candidates;
    if (out->num_candidates > 0 && context.menu.candidates &&
        context.menu.candidates[0].text) {
      out->first_candidate = context.menu.candidates[0].text;
    }
    if (context.commit_text_preview) {
      out->commit_text_preview = context.commit_text_preview;
    }
    api->free_context(&context);
    ++balance->context_free;
  }
  if (api->get_input) {
    const char* input = api->get_input(sid);
    // No free slot exists for this pointer and it is invalid after any edit
    // (rime_api.h:428-429), so it is copied here and never released.
    out->raw_input = input ? input : "(null)";
  }
  {
    RIME_STRUCT(RimeCommit, commit);
    const Bool got = api->get_commit(sid, &commit);
    if (got == True) {
      ++balance->commit_get;
      out->commit_present = true;
      out->commit_text = commit.text ? commit.text : "(null text)";
      api->free_commit(&commit);
      ++balance->commit_free;
    } else if (got != False) {
      *failure = kGetCommitFailed;
      return false;
    }
    // get_commit() consumes the queued text (RimeGetCommit calls
    // session->ResetCommitText(), src/rime_api_impl.h:321), so observing it on
    // every event is also what proves a commit is not re-readable.
  }
  return true;
}

std::string DescribeState(const ObservedState& s) {
  char buf[96];
  std::string line;
  std::snprintf(buf, sizeof(buf), "schema=%s composing=%d disabled=%d ",
                s.schema_id.c_str(), s.status_composing ? 1 : 0,
                s.status_disabled ? 1 : 0);
  line += buf;
  if (s.has_preedit) {
    line += "preedit=\"" + s.preedit + "\"";
    std::snprintf(buf, sizeof(buf), "[len=%lld cur=%lld sel=%lld..%lld] ",
                  static_cast<long long>(s.preedit_length),
                  static_cast<long long>(s.preedit_cursor),
                  static_cast<long long>(s.preedit_sel_start),
                  static_cast<long long>(s.preedit_sel_end));
    line += buf;
  } else {
    line += "preedit=- ";  // sentinel: composition.preedit == NULL
  }
  std::snprintf(buf, sizeof(buf), "cands=%lld ",
                static_cast<long long>(s.num_candidates));
  line += buf;
  if (!s.first_candidate.empty()) {
    line += "cand0=\"" + s.first_candidate + "\" ";
  }
  if (!s.commit_text_preview.empty()) {
    line += "preview=\"" + s.commit_text_preview + "\" ";
  }
  line += "input=\"" + s.raw_input + "\" ";
  line += s.commit_present ? ("commit=\"" + s.commit_text + "\"") : "commit=-";
  return line;
}

// --- assertions ----------------------------------------------------------------

class Checker {
 public:
  void Check(bool condition, const char* what, const std::string& got) {
    ++total_;
    if (condition) {
      return;
    }
    ++failed_;
    std::printf("[replay]   ASSERT FAIL %s: got \"%s\"\n", what, got.c_str());
  }

  void CheckState(const ObservedState& s, bool expect_preedit,
                  const char* expect_preedit_text, long expect_candidates,
                  bool expect_commit, const char* expect_commit_text) {
    Check(s.has_preedit == expect_preedit, "preedit presence",
          s.has_preedit ? "present" : "absent");
    Check(s.num_candidates == expect_candidates, "candidate count",
          std::to_string(s.num_candidates));
    if (expect_preedit) {
      Check(s.preedit == expect_preedit_text, "preedit text", s.preedit);
      // Asserted separately from the text so a future non-ASCII case cannot let
      // byte count and code unit count silently stand in for each other.
      Check(s.preedit_length ==
                static_cast<long>(std::strlen(expect_preedit_text)),
            "preedit length (UTF-8 code units)",
            std::to_string(s.preedit_length));
      if (expect_candidates > 0) {
        Check(s.first_candidate == expect_preedit_text, "candidate 0 text",
              s.first_candidate);
      }
    }
    Check(s.commit_present == expect_commit, "commit presence",
          s.commit_present ? s.commit_text : std::string("none"));
    if (expect_commit) {
      Check(s.commit_text == expect_commit_text, "commit text", s.commit_text);
    }
  }

  int total() const { return total_; }
  int failed() const { return failed_; }

 private:
  int total_ = 0;
  int failed_ = 0;
};

// --- deployment ----------------------------------------------------------------

ExitCode DeployFixture(RimeApi* api, const std::filesystem::path& schema_source,
                       const std::string& staging_dir, Checker* checks) {
  if (!RIME_API_AVAILABLE(api, deploy_config_file) ||
      !RIME_API_AVAILABLE(api, deploy_schema)) {
    Fail(kDeployFailed, "deploy.slots",
         "the loaded API table has no deploy_config_file/deploy_schema slot");
    return kDeployFailed;
  }
  // Both slots run their deployment task synchronously in this process
  // (Deployer::RunTask, src/rime/deployer.cc:28-40). No maintenance thread is
  // started, so there is nothing to poll and no sleep anywhere in this program.
  const Bool default_ok =
      api->deploy_config_file(kFixtureDefaultYaml, "config_version");
  // deploy_schema takes the SOURCE path. The run tree is ASCII-only (checked in
  // ParseArgs), so this narrow string is identical in UTF-8 and in the ANSI code
  // page, which is what std::filesystem::path(const char*) assumes on Windows.
  const std::string schema_src = schema_source.u8string();
  const Bool schema_ok = api->deploy_schema(schema_src.c_str());
  std::printf("[replay] deploy default.yaml    -> %d\n",
              static_cast<int>(default_ok));
  std::printf("[replay] deploy test schema     -> %d  (%s)\n",
              static_cast<int>(schema_ok), schema_src.c_str());

  std::error_code ec;
  const bool default_written =
      std::filesystem::is_regular_file(staging_dir + "/" + kCompiledDefaultYaml,
                                       ec);
  const bool schema_written =
      std::filesystem::is_regular_file(staging_dir + "/" + kCompiledSchemaYaml,
                                       ec);
  std::printf("[replay] staging                %s\n", staging_dir.c_str());
  std::printf("[replay]   compiled default.yaml %s\n",
              default_written ? "present" : "absent");
  std::printf("[replay]   compiled test schema  %s\n",
              schema_written ? "present" : "absent");

  checks->Check(default_ok == True, "deploy_config_file return value",
                std::to_string(static_cast<int>(default_ok)));
  checks->Check(schema_ok == True, "deploy_schema return value",
                std::to_string(static_cast<int>(schema_ok)));
  checks->Check(default_written, "compiled default.yaml exists",
                default_written ? "present" : "absent");
  checks->Check(schema_written, "compiled test schema exists",
                schema_written ? "present" : "absent");
  if (checks->failed() > 0) {
    return kDeployFailed;
  }
  return kOk;
}

// --- the event matrix ----------------------------------------------------------

struct EventSpec {
  int keycode;
  const char* label;
  bool expect_handled;
  bool expect_preedit;
  const char* expect_preedit_text;
  long expect_candidates;
  bool expect_commit;
  const char* expect_commit_text;
  const char* covers;
};

// Session A, one event at a time. `expect_*` values were written from the
// fixture's component set (tests/fixtures/rime_raw/README.md §4); every value in
// the printed line comes from the API.
const EventSpec kMatrix[] = {
    {kXkBackSpace, "BackSpace", false, false, nullptr, 0, false, nullptr,
     "idle: backspace must not be consumed"},
    {kXkEscape, "Escape", false, false, nullptr, 0, false, nullptr,
     "idle: cancel must not be consumed"},
    {kXkReturn, "Return", false, false, nullptr, 0, false, nullptr,
     "idle: return must not invent a commit"},
    {0x61, "a", true, true, "a", 1, false, nullptr, "matrix 2: a"},
    {0x62, "b", true, true, "ab", 1, false, nullptr, "matrix 2: ab"},
    {0x63, "c", true, true, "abc", 1, false, nullptr, "matrix 2: abc"},
    {kXkBackSpace, "BackSpace", true, true, "ab", 1, false, nullptr,
     "matrix 3: abc -> ab, nothing committed"},
    {kXkEscape, "Escape", true, false, nullptr, 0, false, nullptr,
     "matrix 4: cancel clears composition, commits nothing"},
    {0x61, "a", true, true, "a", 1, false, nullptr,
     "matrix 4: the input buffer really is empty again"},
    {0x62, "b", true, true, "ab", 1, false, nullptr, "matrix 2 re-run"},
    {0x63, "c", true, true, "abc", 1, false, nullptr, "matrix 2 re-run"},
    {kXkSpace, "space", false, true, "abc", 1, false, nullptr,
     "fixture erased the space binding"},
    {0x41, "A", false, true, "abc", 1, false, nullptr,
     "uppercase is outside speller/alphabet"},
    {0x31, "1", false, true, "abc", 1, false, nullptr,
     "digit is outside speller/alphabet"},
    {kXkReturn, "Return", true, false, nullptr, 0, true, "abc",
     "matrix 5: one explicit commit of the raw composition"},
};

void RunMatrixEvents(Checker* checks, RimeApi* api, RimeSessionId sid,
                     ApiBalance* balance, int* event_no) {
  for (const EventSpec& ev : kMatrix) {
    ++(*event_no);
    const Bool handled = api->process_key(sid, ev.keycode, 0);
    ObservedState s;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, sid, balance, &s, &failure)) {
      std::printf("[replay] ev=%02d key=0x%04x '%s' mask=0 -> API call failed\n",
                  *event_no, ev.keycode, ev.label);
      Fail(failure, "event.read",
           "an observation call failed; continuing with empty values is refused");
      checks->Check(false, ev.covers, "API failure");
      return;
    }
    std::printf("[replay] ev=%02d key=0x%04x '%s' mask=0 handled=%d  %s\n",
                *event_no, ev.keycode, ev.label, static_cast<int>(handled),
                DescribeState(s).c_str());
    checks->Check(handled == (ev.expect_handled ? True : False), ev.covers,
                  handled == True ? std::string("handled=True ") :
                                    std::string("handled=False "));
    checks->CheckState(s, ev.expect_preedit, ev.expect_preedit_text,
                       ev.expect_candidates, ev.expect_commit,
                       ev.expect_commit_text);
  }
}

ExitCode RunMatrixMode(RimeApi* api, const std::filesystem::path& schema_source,
                       const std::string& staging_dir, ApiBalance* balance) {
  Checker checks;
  const ExitCode rc = DeployFixture(api, schema_source, staging_dir, &checks);
  if (rc != kOk) {
    std::printf("[replay] assertions           %d checked, %d failed\n",
                checks.total(), checks.failed());
    return rc;
  }

  // --- matrix 1: exact schema selection --------------------------------------
  const RimeSessionId a = api->create_session();
  if (a == kNoSession) {
    Fail(kCreateSessionFailed, "session.A",
         "create_session() returned the invalid id with a deployed fixture");
    return kCreateSessionFailed;
  }
  std::printf("[replay] session.A              id=%llu\n",
              static_cast<unsigned long long>(a));

  const Bool selected = api->select_schema(a, kTestSchemaId);
  checks.Check(selected == True, "select_schema return value",
               selected == True ? "True" : "False");
  {
    ObservedState s;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, a, balance, &s, &failure)) {
      Fail(failure, "session.A.read", "get_status/get_context failed");
      api->destroy_session(a);
      return failure;
    }
    std::printf("[replay] after select_schema    %s\n", DescribeState(s).c_str());
    checks.Check(s.schema_id == kTestSchemaId, "get_status().schema_id exact",
                 s.schema_id);
    checks.Check(!s.status_disabled, "status.is_disabled",
                 s.status_disabled ? "true" : "false");
  }
  if (checks.failed() > 0) {
    // Rule: never run past a failed schema selection. Every later assertion would
    // then be about some other schema, which is exactly the misjudgement matrix 8
    // exists to catch.
    Fail(kSchemaSelectionFailed, "schema.select",
         "the test schema was not selected and read back exactly; aborting");
    api->destroy_session(a);
    return kSchemaSelectionFailed;
  }
  Log("schema.select", "test schema selected and confirmed via get_status");

  int event_no = 0;
  RunMatrixEvents(&checks, api, a, balance, &event_no);

  // --- matrix 6: the commit must not be readable a second time ---------------
  {
    ObservedState s;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, a, balance, &s, &failure)) {
      Fail(failure, "commit.reread", "an observation call failed");
      api->destroy_session(a);
      return failure;
    }
    std::printf("[replay] re-read after commit   %s\n", DescribeState(s).c_str());
    checks.Check(!s.commit_present, "matrix 6: second get_commit is empty",
                 s.commit_present ? s.commit_text : std::string("none"));
  }

  // --- matrix 7: two sessions must not share composition state --------------
  const RimeSessionId b = api->create_session();
  if (b == kNoSession) {
    Fail(kCreateSessionFailed, "session.B", "create_session() failed");
    api->destroy_session(a);
    return kCreateSessionFailed;
  }
  checks.Check(b != a, "sessions get distinct ids",
               std::to_string(static_cast<unsigned long long>(b)));
  checks.Check(api->select_schema(b, kTestSchemaId) == True,
               "select_schema on session B", "see next line");
  {
    ObservedState b1;
    ObservedState a1;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, b, balance, &b1, &failure)) {
      Fail(failure, "isolation.B.idle", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      return failure;
    }
    std::printf("[replay] isolate B new          %s\n", DescribeState(b1).c_str());
    checks.Check(b1.schema_id == kTestSchemaId, "session B schema",
                 b1.schema_id);
    checks.Check(!b1.has_preedit && b1.num_candidates == 0 &&
                     !b1.commit_present,
                 "matrix 7: new session B starts empty", DescribeState(b1));
    if (!ReadObservedState(api, a, balance, &a1, &failure)) {
      Fail(failure, "isolation.A.idle", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      return failure;
    }
    checks.Check(!a1.has_preedit && !a1.commit_present,
                 "matrix 7: A unaffected by reading B", DescribeState(a1));

    const Bool x = api->process_key(b, 0x78, 0);  // 'x'
    const Bool y = api->process_key(b, 0x79, 0);  // 'y'
    ObservedState b2;
    ObservedState a2;
    failure = kOk;
    if (!ReadObservedState(api, b, balance, &b2, &failure)) {
      Fail(failure, "isolation.B.composing", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      return failure;
    }
    std::printf("[replay] isolate B after x,y    handled=%d,%d  %s\n",
                static_cast<int>(x), static_cast<int>(y),
                DescribeState(b2).c_str());
    checks.Check(x == True && y == True, "matrix 7: B consumes its own keys",
                 "see previous line");
    checks.Check(b2.preedit == "xy", "matrix 7: B composes xy", b2.preedit);
    if (!ReadObservedState(api, a, balance, &a2, &failure)) {
      Fail(failure, "isolation.A.after.B", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      return failure;
    }
    std::printf("[replay] isolate A while B=xy   %s\n", DescribeState(a2).c_str());
    checks.Check(!a2.has_preedit, "matrix 7: A has no preedit from B",
                 a2.has_preedit ? a2.preedit : std::string("absent"));
    checks.Check(a2.num_candidates == 0, "matrix 7: A has no menu",
                 std::to_string(a2.num_candidates));
    checks.Check(!a2.commit_present, "matrix 7: A has no queued commit",
                 a2.commit_present ? a2.commit_text : std::string("none"));
    api->process_key(b, kXkEscape, 0);
  }

  // --- matrix 8: an id with no schema must not look like a working one -------
  //
  // Discriminated in both directions on the same session, because
  // RimeSelectSchema returns True whenever the session exists
  // (src/rime_api_impl.h:540-549) -- its return value carries no information
  // about whether the schema resolved.
  const RimeSessionId c = api->create_session();
  if (c == kNoSession) {
    Fail(kCreateSessionFailed, "session.C", "create_session() failed");
    api->destroy_session(a);
    api->destroy_session(b);
    return kCreateSessionFailed;
  }
  {
    const Bool bogus_selected = api->select_schema(c, kBogusSchemaId);
    std::printf("[replay] select_schema(bogus)   -> %d   <-- recorded, not "
                "trusted\n", static_cast<int>(bogus_selected));
    const Bool handled = api->process_key(c, 0x61, 0);
    ObservedState s;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, c, balance, &s, &failure)) {
      Fail(failure, "invalid.read", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      api->destroy_session(c);
      return failure;
    }
    std::printf("[replay] invalid schema + 'a'   handled=%d  %s\n",
                static_cast<int>(handled), DescribeState(s).c_str());
    checks.Check(handled == False,
                 "matrix 8: unresolvable schema must not consume a key",
                 handled == True ? "True" : "False");
    checks.Check(!s.has_preedit,
                 "matrix 8: unresolvable schema must not compose",
                 s.has_preedit ? s.preedit : std::string("absent"));
    checks.Check(s.num_candidates == 0,
                 "matrix 8: unresolvable schema has no menu",
                 std::to_string(s.num_candidates));
    checks.Check(!s.commit_present,
                 "matrix 8: unresolvable schema commits nothing",
                 s.commit_present ? s.commit_text : std::string("none"));
    checks.Check(s.schema_id != kTestSchemaId,
                 "matrix 8: bogus id must not report the test schema",
                 s.schema_id);

    // The other direction: selecting the real schema on the same session must
    // make it compose again. Without this, "it did not compose" could also mean
    // "this session is broken for an unrelated reason".
    checks.Check(api->select_schema(c, kTestSchemaId) == True,
                 "matrix 8: reselect of the test schema", "see next line");
    const Bool handled_again = api->process_key(c, 0x62, 0);  // 'b'
    ObservedState fixed;
    failure = kOk;
    if (!ReadObservedState(api, c, balance, &fixed, &failure)) {
      Fail(failure, "invalid.reselect_read", "an observation call failed");
      api->destroy_session(a);
      api->destroy_session(b);
      api->destroy_session(c);
      return failure;
    }
    std::printf("[replay] after reselect + 'b'   handled=%d  %s\n",
                static_cast<int>(handled_again), DescribeState(fixed).c_str());
    checks.Check(handled_again == True,
                 "matrix 8: the same session composes once the real schema is "
                 "selected", handled_again == True ? "True" : "False");
    checks.Check(fixed.preedit == "b", "matrix 8: reselected composition",
                 fixed.preedit);
    api->process_key(c, kXkEscape, 0);
  }

  const RimeSessionId ids[] = {a, b, c};
  for (RimeSessionId id : ids) {
    if (!api->destroy_session(id)) {
      Fail(kDestroySessionFailed, "session.destroy",
           "destroy_session() returned False");
      return kDestroySessionFailed;
    }
    if (api->find_session(id)) {
      Fail(kDestroySessionFailed, "session.destroy",
           "the destroyed id is still resolvable");
      return kDestroySessionFailed;
    }
  }
  Log("session.destroyed", "all three sessions destroyed and unresolvable");

  std::printf("[replay] assertions           %d checked, %d failed\n",
              checks.total(), checks.failed());
  if (checks.failed() > 0) {
    Fail(kAssertionFailed, "matrix", "one or more value assertions failed");
    return kAssertionFailed;
  }
  return kOk;
}

// --- the negative run ------------------------------------------------------------
//
// The fixture is absent. The required outcome is that the engine CANNOT compose.
// A regression that made some other configuration reachable, or that let the
// probe appear to succeed without the fixture, fails this run.
ExitCode RunMissingFixtureMode(RimeApi* api,
                               const std::filesystem::path& schema_source,
                               const std::string& staging_dir,
                               ApiBalance* balance) {
  Checker checks;
  if (!RIME_API_AVAILABLE(api, deploy_config_file) ||
      !RIME_API_AVAILABLE(api, deploy_schema)) {
    Fail(kDeployFailed, "deploy.slots", "deployment slots missing");
    return kDeployFailed;
  }
  std::error_code ec;
  checks.Check(!std::filesystem::exists(schema_source, ec),
               "the schema source really is absent",
               std::filesystem::exists(schema_source, ec) ? "present"
                                                          : "absent");

  const Bool default_ok =
      api->deploy_config_file(kFixtureDefaultYaml, "config_version");
  const std::string schema_src = schema_source.u8string();
  const Bool schema_ok = api->deploy_schema(schema_src.c_str());
  std::printf("[replay] deploy default.yaml    -> %d\n",
              static_cast<int>(default_ok));
  std::printf("[replay] deploy test schema     -> %d\n",
              static_cast<int>(schema_ok));
  std::printf("[replay] staging                %s\n", staging_dir.c_str());

  // SchemaUpdate::Run returns false for a nonexistent source file
  // (src/rime/lever/deployment_tasks.cc:329-333). If that ever stops being true,
  // this run must fail, because the negative case rests on deployment reporting
  // the absence.
  checks.Check(schema_ok == False,
               "deploy_schema must report the missing fixture as failure",
               schema_ok == True ? "True" : "False");
  // Recorded honestly: ConfigFileUpdate does NOT report a missing source as
  // failure -- it returns true after loading nothing
  // (src/rime/lever/deployment_tasks.cc:431-452 with ConfigBuilder::LoadConfig at
  // src/rime/config/config_component.cc:244-253, where Link() and therefore
  // SaveOutputPlugin are skipped when resource->loaded is false). So
  // deploy_config_file's return value alone cannot detect a missing default.yaml;
  // what is asserted instead is that nothing reached the staging directory.
  const bool default_written = std::filesystem::is_regular_file(
      staging_dir + "/" + kCompiledDefaultYaml, ec);
  const bool schema_written = std::filesystem::is_regular_file(
      staging_dir + "/" + kCompiledSchemaYaml, ec);
  std::printf("[replay]   compiled default.yaml %s\n",
              default_written ? "present" : "absent");
  std::printf("[replay]   compiled test schema  %s\n",
              schema_written ? "present" : "absent");
  checks.Check(!default_written, "no compiled default.yaml may appear",
               default_written ? "present" : "absent");
  checks.Check(!schema_written, "no compiled test schema may appear",
               schema_written ? "present" : "absent");

  const RimeSessionId sid = api->create_session();
  checks.Check(sid != kNoSession,
               "a bare session needs no schema (measured in P1.2)",
               sid == kNoSession ? "refused" : "created");
  if (sid == kNoSession) {
    std::printf("[replay] assertions           %d checked, %d failed\n",
                checks.total(), checks.failed());
    Fail(kNegativeRunNotReproducible, "negative",
         "create_session() itself failed, so the composition probe could not run");
    return kNegativeRunNotReproducible;
  }

  const Bool selected = api->select_schema(sid, kTestSchemaId);
  std::printf("[replay] select_schema(test)    -> %d   <-- True here only means "
              "the session exists (src/rime_api_impl.h:540-549)\n",
              static_cast<int>(selected));
  const Bool handled = api->process_key(sid, 0x61, 0);
  ObservedState s;
  ExitCode failure = kOk;
  if (!ReadObservedState(api, sid, balance, &s, &failure)) {
    Fail(failure, "negative.read", "an observation call failed");
    api->destroy_session(sid);
    return failure;
  }
  std::printf("[replay] negative state         handled=%d  %s\n",
              static_cast<int>(handled), DescribeState(s).c_str());
  checks.Check(handled == False, "matrix 9: no fixture, so no consumed key",
               handled == True ? "True" : "False");
  checks.Check(!s.has_preedit, "matrix 9: no fixture, so no preedit",
               s.has_preedit ? s.preedit : std::string("absent"));
  checks.Check(s.num_candidates == 0, "matrix 9: no fixture, so no candidates",
               std::to_string(s.num_candidates));
  checks.Check(!s.commit_present, "matrix 9: no fixture, so no commit",
               s.commit_present ? s.commit_text : std::string("none"));

  if (!api->destroy_session(sid) || api->find_session(sid)) {
    Fail(kDestroySessionFailed, "session.destroy", "cleanup failed");
    return kDestroySessionFailed;
  }

  std::printf("[replay] assertions           %d checked, %d failed\n",
              checks.total(), checks.failed());
  if (checks.failed() > 0) {
    Fail(kNegativeRunNotReproducible, "negative",
         "the missing fixture did NOT make the engine fail closed");
    return kNegativeRunNotReproducible;
  }
  Log("RESULT", "PASS fail-closed for the missing-fixture run");
  return kOk;
}

ExitCode VerifyAfterFinalize(RimeApi* api) {
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

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string error;
  if (!ParseArgs(argc, argv, &opt, &error)) {
    std::printf("usage: rime_input_replay --expect-dll <path> "
                "--fixture-dir <path> --run-root <path> "
                "[--expect-missing-fixture]\n");
    std::printf("[replay] FAIL arguments: %s\n", error.c_str());
    return kUsage;
  }

  std::printf("[replay] mode                 %s\n",
              opt.expect_missing_fixture ? "missing-fixture (must fail closed)"
                                         : "matrix (fixture deployed)");
  std::printf("[replay] units                strings across the C ABI are UTF-8 "
              "code units (bytes); preedit length/offsets are byte offsets into "
              "preedit\n");
  std::printf("[replay] sentinels            preedit=- means "
              "composition.preedit == NULL; commit=- means get_commit returned "
              "False\n");
  std::printf("[replay] scope                ASCII fixture keys only -- a pass "
              "here is not evidence of Unicode or of any language capability\n");

  std::filesystem::path shared_dir, user_dir, log_dir;
  ExitCode rc = PrepareRunRoot(opt, &shared_dir, &user_dir, &log_dir);
  if (rc != kOk) {
    return rc;
  }

  std::filesystem::path schema_source;
  if (!opt.expect_missing_fixture) {
    rc = StageFixture(opt, shared_dir, &schema_source);
    if (rc != kOk) {
      if (rc == kFixturePreconditionFailed) {
        Fail(rc, "fixture.precondition",
             "required fixture files are missing from --fixture-dir");
      }
      return rc;
    }
  } else {
    // The negative run still needs a path to hand to deploy_schema: the one in
    // the intentionally empty shared directory. Its absence is what is asserted.
    schema_source = shared_dir / kFixtureSchemaYaml;
    std::printf("[replay] fixture.skipped        %s (left empty on purpose)\n",
                shared_dir.string().c_str());
  }

  RimeApi* api = rime_get_api();
  if (!api) {
    Fail(kApiUnavailable, "api.acquire", "rime_get_api() returned NULL");
    return kApiUnavailable;
  }
  Log("api.acquire", "rime_get_api() returned a table");

  rc = VerifyModuleIdentity(opt, api);
  if (rc != kOk) {
    return rc;
  }

  const std::string shared_s = shared_dir.string();
  const std::string user_s = user_dir.string();
  const std::string log_s = log_dir.string();

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared_s.c_str();
  traits.user_data_dir = user_s.c_str();
  traits.log_dir = log_s.c_str();
  traits.prebuilt_data_dir = nullptr;  // -> <shared>/build (src/rime/setup.cc:58-61)
  traits.staging_dir = nullptr;        // -> <user>/build  (src/rime/setup.cc:62-65)
  traits.app_name = "rime.epocanva.input_replay";
  traits.distribution_name = "EpoCanva";
  traits.distribution_code_name = "epocanva";
  traits.distribution_version = "0.0.0-p1.3";
  traits.min_log_level = 3;            // FATAL; inert while logging is OFF
  // "default" = core + dict + gears (src/rime/setup.cc:30), and gears is where
  // speller, fluid_editor, abc_segmentor, fallback_segmentor and echo_translator
  // are registered (src/rime/gear/gears_module.cc:46-88). "levers" adds only the
  // deployment task components (src/rime/lever/levers_module.cc:20-30), which the
  // default group does not provide. Both are loaded so that deploy_schema and
  // deploy_config_file are callable; ModuleManager::LoadModule is idempotent
  // (src/rime/module.cc:25-37), so a group listing costs nothing.
  static const char* kModules[] = {"default", "levers", nullptr};
  traits.modules = kModules;

  bool initialize_called = false;
  api->setup(&traits);
  Log("engine.setup", "setup() returned");
  api->initialize(&traits);
  initialize_called = true;
  Log("engine.initialize", "initialize() returned");

  std::string staging_dir;
  if (rc == kOk) {
    // buffer_size counts bytes and the implementation strncpy()s, which does not
    // terminate on truncation, so a full buffer is treated as failure rather than
    // as a shortened path.
    std::vector<char> buffer(kPathBuffer, '\0');
    api->get_shared_data_dir_s(buffer.data(), buffer.size());
    const bool shared_ok =
        strnlen(buffer.data(), buffer.size()) < buffer.size() - 1 &&
        std::strcmp(buffer.data(), shared_s.c_str()) == 0;
    buffer.assign(kPathBuffer, '\0');
    api->get_user_data_dir_s(buffer.data(), buffer.size());
    const bool user_ok = strnlen(buffer.data(), buffer.size()) <
                             buffer.size() - 1 &&
                         std::strcmp(buffer.data(), user_s.c_str()) == 0;
    if (!shared_ok || !user_ok) {
      std::vector<char> echo(kPathBuffer, '\0');
      api->get_shared_data_dir_s(echo.data(), echo.size());
      std::printf("  shared expected: %s\n  shared actual  : %s\n",
                  shared_s.c_str(), echo.data());
      echo.assign(kPathBuffer, '\0');
      api->get_user_data_dir_s(echo.data(), echo.size());
      std::printf("  user   expected: %s\n  user   actual  : %s\n",
                  user_s.c_str(), echo.data());
      Fail(kTraitsNotApplied, "traits.readback",
           "shared_data_dir/user_data_dir did not take effect");
      rc = kTraitsNotApplied;
    } else if (!RIME_API_AVAILABLE(api, get_staging_dir_s)) {
      Fail(kTraitsNotApplied, "traits.readback",
           "the loaded API table has no get_staging_dir_s slot");
      rc = kTraitsNotApplied;
    } else {
      buffer.assign(kPathBuffer, '\0');
      api->get_staging_dir_s(buffer.data(), buffer.size());
      if (strnlen(buffer.data(), buffer.size()) >= buffer.size() - 1) {
        Fail(kTraitsNotApplied, "traits.readback",
             "the staging dir did not fit the read buffer");
        rc = kTraitsNotApplied;
      } else {
        staging_dir.assign(buffer.data());
        std::printf("[replay] staging_dir          %s\n", staging_dir.c_str());
      }
    }
  }

  ApiBalance balance;
  if (rc == kOk) {
    rc = opt.expect_missing_fixture
             ? RunMissingFixtureMode(api, schema_source, staging_dir, &balance)
             : RunMatrixMode(api, schema_source, staging_dir, &balance);
  }

  if (initialize_called) {
    api->finalize();
    Log("cleanup.finalize", "finalize() returned");
    const ExitCode post = VerifyAfterFinalize(api);
    if (rc == kOk && post != kOk) {
      rc = post;
    }
  } else if (rc == kOk) {
    rc = kApiUnavailable;
    Fail(rc, "cleanup", "initialize() was never called");
  }

  if (!balance.balanced()) {
    std::printf("[replay] FAIL api.balance: %d result(s) obtained and not freed\n",
                balance.outstanding());
    if (rc == kOk) {
      rc = kApiResultUnbalanced;
    }
  } else {
    std::printf("[replay] api.balance          status %d/%d context %d/%d "
                "commit %d/%d -- paired\n",
                balance.status_get, balance.status_free, balance.context_get,
                balance.context_free, balance.commit_get, balance.commit_free);
  }

  {
    const int files = CountFilesBeneath(log_dir);
    // Built with ENABLE_LOGGING=OFF, so SetupLogging()'s body is compiled away
    // (src/rime/setup.cc:71-95) and LOG()/DLOG() expand to no-ops
    // (src/rime/no_logging.h:28-36). Nothing may land here.
    if (files != 0) {
      std::printf("[replay] FAIL post.finalize.log: %d file(s) under the log "
                  "directory\n", files);
      if (rc == kOk) {
        rc = kLogFilesWritten;
      }
    } else {
      Log("post.finalize", "log directory stayed empty");
    }
  }

  if (rc == kOk) {
    Log("RESULT", opt.expect_missing_fixture ? "PASS missing-fixture run"
                                            : "PASS input-link replay matrix");
  }
  return rc;
}
