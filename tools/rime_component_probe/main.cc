// EpoCanva P1.4 -- reachability test for a project-owned rime component that was
// compiled INTO rime.dll (docs/adr/0004-compiled-in-rime-components.md).
//
// WHAT THIS PROVES, and nothing more: in the engine binary built with
// tools/cmake/epocanva_rime_compiled_in_components.cmake,
//   1. a module named EPOCANVA_PROBE_MODULE_NAME is present in that DLL and is
//      loaded by librime's own module mechanism;
//   2. its Processor class is constructed when an engine is built from a schema
//      that lists it;
//   3. keys really arrive in that Processor's ProcessKeyEvent(), and the letters
//      it releases still form the same composition the P1.3 fixture produced;
//   4. the fixture's one designated test key is consumed BY THAT COMPONENT
//      (a counter delta plus process_key() returning True), without changing the
//      preedit and without producing commit text;
//   5. the instances go away when their session is destroyed.
// In the unmodified baseline engine, and in the probe engine with the component
// removed from the schema, none of that may happen -- which the two negative modes
// assert positively.
//
// WHAT THIS DOES NOT PROVE: no English capability of any kind. No completion,
// no prediction, no correction, no segmentation, no word list, no learning. The
// probe component performs no linguistic work; it counts. The candidate text in
// this fixture is byte-identical to the injected keys.
//
// UNITS (AGENTS.md rule 5). Strings crossing the C API are UTF-8 code units
// (bytes) and every buffer_size argument counts bytes. RimeComposition.length /
// .cursor_pos / .sel_start / .sel_end are byte offsets into `preedit` -- not
// characters, not UTF-16 code units, not screen columns. Keycodes are X11 keysyms
// (32-bit identifiers, not character encodings) and `mask` is librime's own
// modifier bit set (src/rime/key_table.h:13-43). Every counter is an event count.
//
// OWNERSHIP ACROSS THE BOUNDARY (verified at commit 33e78140 in
// src/rime_api_impl.h): get_status/get_context/get_commit allocate with
// `new char[]` inside rime.dll and the matching free_* releases them there;
// get_input has no free slot at all. The counters this program reads are copied
// into a struct THIS program allocated; the diagnostics table and the build-id
// string returned by the module point at static storage inside rime.dll and are
// never freed. After RimeApi::finalize() this program deliberately stops touching
// the probe surface: finalize() runs ModuleManager::UnloadModules() and
// Registry::Clear() (src/rime_api_impl.h:58-63), so pointers obtained through the
// engine are not promised to survive it.
//
// HONESTY. No observation here is synthesised: every asserted preedit, candidate,
// commit string, handled flag and counter comes from an API return value. An API
// failure returns a stage code instead of continuing with an empty value. The two
// negative modes assert an ABSENCE with their own exit code, so they need no
// WILL_FAIL and cannot turn a real engine capability green.

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "probe_diagnostics.h"
#include "rime_api.h"

namespace {

// Distinct non-zero code per structural stage. 2..20 keep the same meanings as
// tools/rime_input_replay so the two programs' reports read alike; 21 and 22 are
// specific to the probe surface.
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
  kProbeSurfaceUnavailable = 21,
  kProbeCounterReadFailed = 22,
};

constexpr std::size_t kPathBuffer = 4096;
// src/rime_api_impl.h:153-155 treats id 0 as unknown; rime_api.h names no
// invalid-session constant.
constexpr RimeSessionId kNoSession = 0;

// X11 keysyms, read out of the pinned checkout's vendored header rather than from
// memory: third_party/librime/include/X11/keysymdef.h
//   XK_BackSpace 0xff08 (:124)   XK_Return 0xff0d (:128)   XK_Escape 0xff1b (:132)
//   XK_F1 0xffbe (:252)   XK_F6 0xffc3 (:257)
// The test key itself comes from EPOCANVA_PROBE_TEST_KEYCODE so the component and
// this program cannot drift apart. Printable ASCII keeps its code point:
// a 0x61, b 0x62, c 0x63, q 0x71 (:610), x 0x78 (:617), y 0x79 (:618).
// Modifier bits: src/rime/key_table.h:15-16 (kShiftMask = 1 << 0).
constexpr int kXkBackSpace = 0xff08;
constexpr int kXkReturn = 0xff0d;
constexpr int kXkEscape = 0xff1b;
constexpr int kShiftModifier = 1;

constexpr char kFixtureDefaultYaml[] = "default.yaml";
constexpr char kFixtureSchemaYaml[] = "epocanva_test_probe.schema.yaml";
constexpr char kTestSchemaId[] = "epocanva_test_probe";
// The exact list entry the second negative mode removes, after left-trimming the
// line: tests/fixtures/rime_probe/epocanva_test_probe.schema.yaml lists it first
// under engine/processors.
constexpr char kProbeProcessorListEntry[] = "- epocanva_probe_processor";
// A run root is only deletable when its parent directory carries this exact name,
// which nothing in this repository produces except the generated CTest commands.
constexpr char kRunRootGuard[] = "rime-component-probe-run";

enum class Mode {
  // Expect the compiled-in component to be reachable and to behave.
  kProbe,
  // Negative test 1: the unmodified baseline engine must show no such module.
  kExpectModuleAbsent,
  // Negative test 2: the probe engine, with the component removed from the
  // schema, must show a module that loads but never instantiates.
  kExpectComponentAbsent,
};

struct Options {
  std::wstring expect_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path run_root;
  Mode mode = Mode::kProbe;
};

// --- plumbing shared in spirit with tools/rime_input_replay --------------------
// Duplicated rather than factored into a library, for the reason recorded in the
// P1.3 report section 4: a shared static target would add a build-graph edge and a
// second place where the CRT has to be kept in agreement, to save ~90 lines.

void Log(const char* stage, const std::string& detail) {
  std::printf("[probe] %-24s %s\n", stage, detail.c_str());
}

void Fail(ExitCode code, const char* stage, const std::string& detail) {
  std::printf("[probe] FAIL %-20s %s (stage code %d)\n", stage, detail.c_str(),
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
  const DWORD n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(buffer.size()),
                                   buffer.data(), nullptr);
  if (n == 0 || n >= buffer.size()) {
    return raw;
  }
  return std::wstring(buffer.data(), n);
}

// Finds the module that actually supplies the code behind an API function pointer,
// so a stale rime.dll reachable through PATH can never be mistaken for the library
// this run was linked against.
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

class Checker {
 public:
  void Check(bool condition, const std::string& what, const std::string& got) {
    ++total_;
    if (condition) {
      return;
    }
    ++failed_;
    std::printf("[probe]   ASSERT FAIL %s: got \"%s\"\n", what.c_str(),
                got.c_str());
  }

  int total() const { return total_; }
  int failed() const { return failed_; }

 private:
  int total_ = 0;
  int failed_ = 0;
};

// --- observed engine state ------------------------------------------------------

struct ObservedState {
  std::string schema_id;
  bool status_composing = false;
  bool status_disabled = false;
  bool has_preedit = false;
  std::string preedit;
  long preedit_length = -1;
  long num_candidates = -1;
  std::string first_candidate;
  std::string raw_input;
  bool commit_present = false;
  std::string commit_text;
};

// Returns false only when an API call itself failed, which the caller turns into a
// stage code. A successful call reporting "no preedit" is not a failure: that is
// the documented sentinel (src/rime_api_impl.h:220-224 fills composition.preedit
// only while the context IsComposing()).
bool ReadObservedState(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
                       ObservedState* out, ExitCode* failure) {
  *out = ObservedState();  // never let a previous event's fields leak into this one
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
    out->num_candidates = context.menu.num_candidates;
    if (out->num_candidates > 0 && context.menu.candidates &&
        context.menu.candidates[0].text) {
      out->first_candidate = context.menu.candidates[0].text;
    }
    api->free_context(&context);
    ++balance->context_free;
  }
  if (api->get_input) {
    const char* input = api->get_input(sid);
    // No free slot exists for this pointer and it is invalidated by any edit, so
    // it is copied here and never released.
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
    std::snprintf(buf, sizeof(buf), "[len=%ld] ", s.preedit_length);
    line += buf;
  } else {
    line += "preedit=- ";  // sentinel: composition.preedit == NULL
  }
  std::snprintf(buf, sizeof(buf), "cands=%ld ", s.num_candidates);
  line += buf;
  if (!s.first_candidate.empty()) {
    line += "cand0=\"" + s.first_candidate + "\" ";
  }
  line += "input=\"" + s.raw_input + "\" ";
  line += s.commit_present ? ("commit=\"" + s.commit_text + "\"") : "commit=-";
  return line;
}

// --- the probe's own diagnostics surface -----------------------------------------

// Everything reachable through the public C API, with no EpoCanva-specific export.
struct ProbeSurface {
  bool module_found = false;
  bool api_available = false;
  RimeModule* module = nullptr;
  const EpoCanvaProbeApi* api = nullptr;
};

ProbeSurface AcquireProbeSurface(RimeApi* api) {
  ProbeSurface s;
  if (!RIME_API_AVAILABLE(api, find_module)) {
    return s;
  }
  s.module = api->find_module(EPOCANVA_PROBE_MODULE_NAME);
  s.module_found = (s.module != nullptr);
  if (!s.module || !s.module->get_api) {
    return s;
  }
  RimeCustomApi* raw = s.module->get_api();
  if (!raw) {
    return s;
  }
  // Deliberate type pun across the C boundary, in the direction upstream itself
  // uses when it hands a table out (src/rime/lever/levers_api_impl.h:279 returns
  // `(RimeCustomApi*)&s_api`). It is made safe by the data_size check below, not
  // by the cast.
  const EpoCanvaProbeApi* table = reinterpret_cast<const EpoCanvaProbeApi*>(raw);
  // Refuse a table smaller than this program's layout, or missing a function:
  // reading it anyway would be a type pun on a struct we do not have.
  const int payload = static_cast<int>(sizeof(EpoCanvaProbeApi) - sizeof(int));
  if (table->data_size < payload || !table->get_counters ||
      !table->reset_counters_when_idle || !table->build_id) {
    return s;
  }
  s.api = table;
  s.api_available = true;
  return s;
}

EpoCanvaProbeCounters ZeroCounters() {
  EpoCanvaProbeCounters c;
  std::memset(&c, 0, sizeof(c));
  RIME_STRUCT_INIT(EpoCanvaProbeCounters, c);
  return c;
}

bool ReadCounters(const ProbeSurface& s, EpoCanvaProbeCounters* out) {
  if (!s.api_available) {
    return false;
  }
  *out = ZeroCounters();
  return s.api->get_counters(out) == True;
}

std::string DescribeCounters(const EpoCanvaProbeCounters& c) {
  char buf[224];
  std::snprintf(buf, sizeof(buf),
                "module_init=%llu module_final=%llu constructed=%llu "
                "destroyed=%llu live=%llu pk_calls=%llu passed=%llu consumed=%llu",
                static_cast<unsigned long long>(c.module_initialized),
                static_cast<unsigned long long>(c.module_finalized),
                static_cast<unsigned long long>(c.component_constructed),
                static_cast<unsigned long long>(c.component_destroyed),
                static_cast<unsigned long long>(c.live_instances),
                static_cast<unsigned long long>(c.process_key_calls),
                static_cast<unsigned long long>(c.keys_passed_through),
                static_cast<unsigned long long>(c.test_keys_consumed));
  return std::string(buf);
}

// Difference between two snapshots as signed values: a negative delta is itself a
// finding (it would mean the counters were reset mid-run), so it must not be
// swallowed by unsigned arithmetic.
struct CounterDelta {
  long long module_initialized = 0;
  long long module_finalized = 0;
  long long constructed = 0;
  long long destroyed = 0;
  long long live = 0;
  long long process_key_calls = 0;
  long long keys_passed_through = 0;
  long long test_keys_consumed = 0;
};

CounterDelta DeltaOf(const EpoCanvaProbeCounters& before,
                     const EpoCanvaProbeCounters& after) {
  CounterDelta d;
  d.module_initialized = static_cast<long long>(after.module_initialized) -
                         static_cast<long long>(before.module_initialized);
  d.module_finalized = static_cast<long long>(after.module_finalized) -
                       static_cast<long long>(before.module_finalized);
  d.constructed = static_cast<long long>(after.component_constructed) -
                  static_cast<long long>(before.component_constructed);
  d.destroyed = static_cast<long long>(after.component_destroyed) -
               static_cast<long long>(before.component_destroyed);
  d.live = static_cast<long long>(after.live_instances) -
           static_cast<long long>(before.live_instances);
  d.process_key_calls = static_cast<long long>(after.process_key_calls) -
                        static_cast<long long>(before.process_key_calls);
  d.keys_passed_through = static_cast<long long>(after.keys_passed_through) -
                          static_cast<long long>(before.keys_passed_through);
  d.test_keys_consumed = static_cast<long long>(after.test_keys_consumed) -
                         static_cast<long long>(before.test_keys_consumed);
  return d;
}

void CheckDeltaNonNegative(Checker* checks, const CounterDelta& d,
                           const std::string& label) {
  const long long fields[] = {d.module_initialized, d.module_finalized,
                              d.constructed,        d.destroyed,
                              d.process_key_calls,  d.keys_passed_through,
                              d.test_keys_consumed};
  const char* names[] = {"module_init", "module_final", "constructed",
                         "destroyed",   "pk_calls",     "passed",
                         "consumed"};
  for (std::size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    checks->Check(fields[i] >= 0,
                  label + ": counter went backwards (" + names[i] + ")",
                  std::to_string(fields[i]));
  }
}

// --- arguments -------------------------------------------------------------------

bool ParseModeName(const std::string& s, Mode* out) {
  if (s == "probe") {
    *out = Mode::kProbe;
    return true;
  }
  if (s == "expect-module-absent") {
    *out = Mode::kExpectModuleAbsent;
    return true;
  }
  if (s == "expect-component-absent") {
    *out = Mode::kExpectComponentAbsent;
    return true;
  }
  return false;
}

bool ParseArgs(int argc, char** argv, Options* out, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key != "--expect-dll" && key != "--fixture-dir" && key != "--run-root" &&
        key != "--mode") {
      *error = "unknown option " + key;
      return false;
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
    } else if (!ParseModeName(value, &out->mode)) {
      *error = "unknown --mode value '" + value + "'";
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

const char* ModeName(Mode m) {
  switch (m) {
    case Mode::kProbe:
      return "probe (the compiled-in component must be reachable)";
    case Mode::kExpectModuleAbsent:
      return "expect-module-absent (negative test 1: baseline engine)";
    default:
      return "expect-component-absent (negative test 2: probe engine, schema "
             "without the component)";
  }
}

// --- run directory and fixture staging -------------------------------------------

ExitCode PrepareRunRoot(const Options& opt, std::filesystem::path* shared_dir,
                        std::filesystem::path* user_dir,
                        std::filesystem::path* log_dir) {
  const std::filesystem::path parent = opt.run_root.parent_path();
  if (parent.filename().u8string() != kRunRootGuard) {
    std::printf("[probe] FAIL run.root: --run-root %s is not a leaf under a "
                "directory named '%s'; refusing to delete anything here\n",
                opt.run_root.string().c_str(), kRunRootGuard);
    return kRunDirectorySetupFailed;
  }
  std::error_code ec;
  if (std::filesystem::exists(opt.run_root, ec)) {
    std::filesystem::remove_all(opt.run_root, ec);
    if (ec) {
      std::printf("[probe] FAIL run.root: cannot remove the previous run "
                  "directory %s (%s)\n",
                  opt.run_root.string().c_str(), ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
  }
  *shared_dir = opt.run_root / "shared";
  *user_dir = opt.run_root / "user";
  *log_dir = opt.run_root / "log";
  for (const std::filesystem::path& d : {*shared_dir, *user_dir, *log_dir}) {
    std::filesystem::create_directories(d, ec);
    if (ec) {
      std::printf("[probe] FAIL run.root: cannot create %s (%s)\n",
                  d.string().c_str(), ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
  }
  std::printf("[probe] run-root             %s (recreated empty)\n",
              opt.run_root.string().c_str());
  return kOk;
}

bool ReadWholeFile(const std::filesystem::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// Negative test 2 needs the fixture WITHOUT the probe component. The derived copy
// is written into this run's own directory: tests/fixtures/ is only ever read, so
// no formal source file is modified and there is nothing to "forget to restore".
// The number of removed lines is asserted by the caller, so the transformation
// cannot silently become a no-op if the fixture is edited later.
ExitCode StageSchemaWithoutComponent(const std::filesystem::path& src,
                                     const std::filesystem::path& dst,
                                     int* removed) {
  std::string text;
  if (!ReadWholeFile(src, &text)) {
    std::printf("[probe] FAIL fixture.read: cannot read %s\n",
                src.string().c_str());
    return kFixturePreconditionFailed;
  }
  std::string out;
  out.reserve(text.size());
  int removed_count = 0;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const bool has_newline = (nl != std::string::npos);
    const std::size_t end = has_newline ? nl : text.size();
    std::string line = text.substr(pos, end - pos);
    std::string content = line;
    if (!content.empty() && content.back() == '\r') {
      content.pop_back();
    }
    const std::size_t first = content.find_first_not_of(" \t");
    const std::string trimmed =
        (first == std::string::npos) ? std::string() : content.substr(first);
    if (trimmed == kProbeProcessorListEntry) {
      ++removed_count;  // drop the line and its newline
    } else {
      out += line;
      if (has_newline) {
        out += '\n';
      }
    }
    if (!has_newline) {
      break;
    }
    pos = nl + 1;
  }
  *removed = removed_count;
  std::ofstream os(dst, std::ios::binary);
  if (!os) {
    std::printf("[probe] FAIL fixture.write: cannot create %s\n",
                dst.string().c_str());
    return kRunDirectorySetupFailed;
  }
  os << out;
  os.close();
  std::error_code ec;
  std::printf("[probe] fixture.derived      %s (%llu bytes, %d list entry "
              "line(s) removed)\n",
              dst.string().c_str(),
              static_cast<unsigned long long>(
                  std::filesystem::file_size(dst, ec)),
              removed_count);
  return kOk;
}

ExitCode StageFixture(const Options& opt, const std::filesystem::path& shared_dir,
                      std::filesystem::path* schema_source_path) {
  const std::filesystem::path default_src = opt.fixture_dir / kFixtureDefaultYaml;
  const std::filesystem::path schema_src = opt.fixture_dir / kFixtureSchemaYaml;
  std::error_code ec;
  bool any_absent = false;
  for (const std::filesystem::path& p : {default_src, schema_src}) {
    if (!std::filesystem::is_regular_file(p, ec) || ec) {
      std::printf("[probe] fixture.absent       %s\n", p.string().c_str());
      any_absent = true;
    }
  }
  if (any_absent) {
    return kFixturePreconditionFailed;
  }

  const std::filesystem::path default_dest = shared_dir / kFixtureDefaultYaml;
  std::filesystem::copy_file(default_src, default_dest,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("[probe] FAIL fixture.copy: %s -> %s (%s)\n",
                default_src.string().c_str(), default_dest.string().c_str(),
                ec.message().c_str());
    return kRunDirectorySetupFailed;
  }
  std::printf("[probe] fixture.staged       %s (%llu bytes)\n",
              default_dest.string().c_str(),
              static_cast<unsigned long long>(
                  std::filesystem::file_size(default_dest, ec)));

  const std::filesystem::path schema_dest = shared_dir / kFixtureSchemaYaml;
  if (opt.mode == Mode::kExpectComponentAbsent) {
    int removed = 0;
    const ExitCode rc =
        StageSchemaWithoutComponent(schema_src, schema_dest, &removed);
    if (rc != kOk) {
      return rc;
    }
    if (removed != 1) {
      std::printf("[probe] FAIL fixture.derive: expected to remove exactly 1 line "
                  "matching '%s', removed %d\n",
                  kProbeProcessorListEntry, removed);
      return kFixturePreconditionFailed;
    }
  } else {
    std::filesystem::copy_file(schema_src, schema_dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      std::printf("[probe] FAIL fixture.copy: %s -> %s (%s)\n",
                  schema_src.string().c_str(), schema_dest.string().c_str(),
                  ec.message().c_str());
      return kRunDirectorySetupFailed;
    }
    std::printf("[probe] fixture.staged       %s (%llu bytes)\n",
                schema_dest.string().c_str(),
                static_cast<unsigned long long>(
                    std::filesystem::file_size(schema_dest, ec)));
  }
  *schema_source_path = schema_dest;
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
    std::printf("[probe] FAIL module.identity\n");
    std::printf("  expected: %ls\n  actual  : %ls\n", opt.expect_dll.c_str(),
                actual.c_str());
    Fail(kLoadedModuleMismatch, "module.identity",
         "loaded module is not the artefact this program was built against");
    return kLoadedModuleMismatch;
  }
  std::printf("[probe] module-loaded        %ls\n", actual.c_str());
  std::printf("[probe] librime-version      %s\n", api->get_version());
  return kOk;
}

// --- one key event ---------------------------------------------------------------

struct EventResult {
  bool handled = false;
  ObservedState state;
  EpoCanvaProbeCounters counters;
  CounterDelta delta;
};

// Sends exactly one key through RimeApi::process_key() and reads back both the
// engine's state and the component's counters. There is no other input path in
// this program: no keyboard hook, no clipboard, no host text, and nowhere is a key
// simulated as handled. When the probe surface is unavailable (negative test 1,
// the baseline engine) the counters are reported as unchanged, because there is
// nothing to read -- the callers that do that have already asserted the absence,
// and every mode that reads counters requires the surface up front.
ExitCode SendKey(RimeApi* api, const ProbeSurface& surface, RimeSessionId sid,
                 int keycode, int mask, ApiBalance* balance,
                 const EpoCanvaProbeCounters& before, EventResult* out) {
  const Bool handled = api->process_key(sid, keycode, mask);
  out->handled = (handled == True);
  if (handled != True && handled != False) {
    Fail(kGetCommitFailed, "key.process",
         "process_key() returned neither True nor False");
    return kGetCommitFailed;
  }
  ExitCode failure = kOk;
  if (!ReadObservedState(api, sid, balance, &out->state, &failure)) {
    return failure;
  }
  if (surface.api_available) {
    if (!ReadCounters(surface, &out->counters)) {
      return kProbeCounterReadFailed;
    }
  } else {
    out->counters = before;
  }
  out->delta = DeltaOf(before, out->counters);
  return kOk;
}

void ReportEvent(int index, int keycode, int mask, const char* key_name,
                 const EventResult& r) {
  char head[128];
  std::snprintf(head, sizeof(head), "ev=%02d key=0x%04x '%s' mask=%d", index,
                keycode, key_name, mask);
  std::printf("[probe] %-34s handled=%d d{pk=%lld pass=%lld consumed=%lld} %s\n",
              head, r.handled ? 1 : 0, r.delta.process_key_calls,
              r.delta.keys_passed_through, r.delta.test_keys_consumed,
              DescribeState(r.state).c_str());
}

// `expect_probe_consumed_delta` / `expect_probe_passed_delta` are what separate
// "the engine said True" from "THIS component was the one that said it".
void CheckEvent(Checker* checks, const char* label, const EventResult& r,
                bool expect_handled, bool expect_preedit,
                const char* expect_preedit_text, long expect_candidates,
                bool expect_commit, const char* expect_commit_text,
                long long expect_probe_consumed_delta,
                long long expect_passed_delta) {
  const ObservedState& s = r.state;
  const std::string tag(label);
  checks->Check(r.handled == expect_handled, tag + ": handled",
                r.handled ? "True" : "False");
  checks->Check(s.has_preedit == expect_preedit, tag + ": preedit presence",
                s.has_preedit ? "present" : "absent");
  checks->Check(s.num_candidates == expect_candidates,
                tag + ": candidate count", std::to_string(s.num_candidates));
  if (expect_preedit) {
    checks->Check(s.preedit == expect_preedit_text, tag + ": preedit text",
                  s.preedit);
    // Asserted separately from the text so a future non-ASCII case cannot let a
    // byte count silently stand in for a code unit count (AGENTS.md rule 5).
    checks->Check(
        s.preedit_length == static_cast<long>(std::strlen(expect_preedit_text)),
        tag + ": preedit length (UTF-8 code units)",
        std::to_string(s.preedit_length));
    if (expect_candidates > 0) {
      checks->Check(s.first_candidate == expect_preedit_text,
                    tag + ": candidate 0 text", s.first_candidate);
    }
  }
  checks->Check(s.commit_present == expect_commit, tag + ": commit presence",
                s.commit_present ? s.commit_text : std::string("none"));
  if (expect_commit) {
    checks->Check(s.commit_text == expect_commit_text, tag + ": commit text",
                  s.commit_text);
  }
  checks->Check(r.delta.test_keys_consumed == expect_probe_consumed_delta,
                tag + ": probe test-key consumed delta",
                std::to_string(r.delta.test_keys_consumed));
  checks->Check(r.delta.keys_passed_through == expect_passed_delta,
                tag + ": probe passed-through delta",
                std::to_string(r.delta.keys_passed_through));
  CheckDeltaNonNegative(checks, r.delta, tag);
}

// --- deployment -------------------------------------------------------------------

ExitCode DeployFixture(RimeApi* api, const std::filesystem::path& schema_source,
                       const std::string& staging_dir, Checker* checks) {
  const int failures_before = checks->failed();
  if (!RIME_API_AVAILABLE(api, deploy_config_file) ||
      !RIME_API_AVAILABLE(api, deploy_schema)) {
    Fail(kDeployFailed, "deploy.slots",
         "the loaded API table has no deploy_config_file/deploy_schema slot");
    return kDeployFailed;
  }
  // Both slots run their deployment task synchronously in this process
  // (src/rime/deployer.cc:28-40): no maintenance thread, no sleep, no polling.
  const Bool default_ok =
      api->deploy_config_file(kFixtureDefaultYaml, "config_version");
  const std::string schema_src = schema_source.u8string();
  const Bool schema_ok = api->deploy_schema(schema_src.c_str());
  std::printf("[probe] deploy default.yaml    -> %d\n",
              static_cast<int>(default_ok));
  std::printf("[probe] deploy test schema     -> %d  (%s)\n",
              static_cast<int>(schema_ok), schema_src.c_str());

  std::error_code ec;
  const bool default_written = std::filesystem::is_regular_file(
      staging_dir + "/" + kFixtureDefaultYaml, ec);
  const bool schema_written = std::filesystem::is_regular_file(
      staging_dir + "/" + kFixtureSchemaYaml, ec);
  std::printf("[probe] staging                %s\n", staging_dir.c_str());
  std::printf("[probe]   compiled default.yaml %s\n",
              default_written ? "present" : "absent");
  std::printf("[probe]   compiled test schema  %s\n",
              schema_written ? "present" : "absent");

  // deploy_config_file() returns True even when its source is missing
  // (docs/testing/rime-input-replay.md section 7), so the staging artefacts are
  // asserted too, never the return values alone.
  checks->Check(default_ok == True, "deploy_config_file return value",
                std::to_string(static_cast<int>(default_ok)));
  checks->Check(schema_ok == True, "deploy_schema return value",
                std::to_string(static_cast<int>(schema_ok)));
  checks->Check(default_written, "compiled default.yaml exists",
                default_written ? "present" : "absent");
  checks->Check(schema_written, "compiled test schema exists",
                schema_written ? "present" : "absent");
  // Only this function's own assertions decide its return code; an earlier failure
  // elsewhere must not be reported as a deployment failure.
  if (checks->failed() > failures_before) {
    return kDeployFailed;
  }
  return kOk;
}

// Selects the fixture schema and proves the selection by reading it back, never by
// select_schema()'s return value -- which is True even for an id that resolves to
// nothing (docs/testing/rime-input-replay.md section 7).
ExitCode SelectAndConfirmSchema(RimeApi* api, RimeSessionId sid,
                                ApiBalance* balance, Checker* checks,
                                const std::string& label) {
  const int failures_before = checks->failed();
  if (!RIME_API_AVAILABLE(api, select_schema)) {
    Fail(kSchemaSelectionFailed, "schema.select", "no select_schema slot");
    return kSchemaSelectionFailed;
  }
  api->select_schema(sid, kTestSchemaId);
  ObservedState s;
  ExitCode failure = kOk;
  if (!ReadObservedState(api, sid, balance, &s, &failure)) {
    std::printf("[probe] FAIL schema.readback: get_status/get_context failed on %s\n",
                label.c_str());
    return failure;
  }
  checks->Check(s.schema_id == kTestSchemaId,
                label + ": get_status().schema_id readback", s.schema_id);
  if (checks->failed() > failures_before) {
    return kSchemaSelectionFailed;
  }
  return kOk;
}

RimeSessionId CreateSessionOrZero(RimeApi* api, const std::string& what) {
  const RimeSessionId sid = api->create_session();
  if (sid == kNoSession) {
    std::printf("[probe] FAIL session.create (%s): create_session() returned the "
                "invalid id\n",
                what.c_str());
    return kNoSession;
  }
  std::printf("[probe] session.%-18s id=%llu\n", what.c_str(),
              static_cast<unsigned long long>(sid));
  return sid;
}

ExitCode DestroySessionAndCheck(RimeApi* api, RimeSessionId sid, Checker* checks,
                                const std::string& label) {
  if (sid == kNoSession) {
    return kOk;
  }
  const Bool ok = api->destroy_session(sid);
  checks->Check(ok == True, label + ": destroy_session() return",
                ok == True ? "True" : "False");
  if (api->find_session(sid)) {
    checks->Check(false, label + ": id still resolvable after destroy", "True");
    return kDestroySessionFailed;
  }
  Log("session.destroyed", label + ": released and no longer resolvable");
  return kOk;
}

// --- mode: probe -------------------------------------------------------------------

ExitCode RunProbeMode(RimeApi* api, const ProbeSurface& surface,
                      const std::filesystem::path& schema_source,
                      const std::string& staging_dir, ApiBalance* balance,
                      Checker* checks) {
  if (!surface.api_available) {
    Fail(kProbeSurfaceUnavailable, "probe.surface",
         "the engine loaded by this run does not expose the compiled-in probe "
         "module through find_module()/get_api()");
    return kProbeSurfaceUnavailable;
  }
  std::printf("[probe] probe.build_id       %s\n", surface.api->build_id());

  EpoCanvaProbeCounters c;
  if (!ReadCounters(surface, &c)) {
    Fail(kProbeCounterReadFailed, "probe.counters",
         "get_counters() returned False");
    return kProbeCounterReadFailed;
  }
  std::printf("[probe] counters.after_init  %s\n", DescribeCounters(c).c_str());

  // Acceptance 1: the module's initialize() ran. The DLL-load-time static
  // initializer only REGISTERS the module (src/rime_api.h:528-533); this counter
  // moves only when ModuleManager::LoadModule calls initialize()
  // (src/rime/module.cc:25-37), i.e. through LoadModules(traits->modules).
  checks->Check(c.module_initialized >= 1, "module_initialized >= 1",
                std::to_string(c.module_initialized));
  checks->Check(c.module_finalized == 0, "module_finalized == 0 before finalize",
                std::to_string(c.module_finalized));
  checks->Check(c.process_key_calls == 0, "no key processed yet",
                std::to_string(c.process_key_calls));
  checks->Check(c.test_keys_consumed == 0, "no test key consumed yet",
                std::to_string(c.test_keys_consumed));

  const ExitCode drc =
      DeployFixture(api, schema_source, staging_dir, checks);
  if (drc != kOk) {
    return drc;
  }

  EpoCanvaProbeCounters before_sessions;
  if (!ReadCounters(surface, &before_sessions)) {
    return kProbeCounterReadFailed;
  }

  // Acceptance 2: a real engine constructed a real instance of the component.
  const RimeSessionId a = CreateSessionOrZero(api, "A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  EpoCanvaProbeCounters after_A;
  if (!ReadCounters(surface, &after_A)) {
    api->destroy_session(a);
    return kProbeCounterReadFailed;
  }
  const CounterDelta created = DeltaOf(before_sessions, after_A);
  std::printf("[probe] constructed.by.A     delta=%lld live=%llu\n",
              created.constructed,
              static_cast<unsigned long long>(after_A.live_instances));
  checks->Check(created.constructed >= 1,
                "creating a session constructed the component",
                std::to_string(created.constructed));
  checks->Check(created.live > 0, "live instance count rose with the session",
                std::to_string(created.live));

  ExitCode rc =
      SelectAndConfirmSchema(api, a, balance, checks, "session A");
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }

  // The reset guard: while an instance is alive it must refuse, so a run cannot
  // lose its own evidence by accident. The snapshot either side is taken with no
  // other engine call in between -- selecting a schema legitimately constructs
  // another instance, and that must not be mistaken for the reset having moved the
  // counters.
  EpoCanvaProbeCounters before_reset;
  if (!ReadCounters(surface, &before_reset)) {
    api->destroy_session(a);
    return kProbeCounterReadFailed;
  }
  const Bool reset = surface.api->reset_counters_when_idle();
  checks->Check(reset == False,
                "reset_counters_when_idle() refuses while an instance is alive",
                std::to_string(static_cast<int>(reset)));
  EpoCanvaProbeCounters counters;
  if (!ReadCounters(surface, &counters)) {
    api->destroy_session(a);
    return kProbeCounterReadFailed;
  }
  checks->Check(counters.component_constructed == before_reset.component_constructed,
                "the refused reset left the counters untouched",
                std::to_string(before_reset.component_constructed) + " then " +
                    std::to_string(counters.component_constructed));

  // Acceptances 3, 4, 5, 6: the key matrix.
  struct EventSpec {
    int keycode;
    int mask;
    const char* name;
    bool expect_handled;
    bool expect_preedit;
    const char* expect_preedit_text;
    long expect_candidates;
    bool expect_commit;
    const char* expect_commit_text;
    long long expect_probe_consumed;
    long long expect_probe_passed;
  };
  // clang-format off
  const EventSpec kMatrix[] = {
      // The three fixture letters are consumed by the speller downstream, but each
      // one is counted by EpoCanva's component and released by it
      // (keys_passed_through +1). That is acceptance 3.
      {0x0061, 0, "a",         true,  true,  "a",   1, false, nullptr, 0, 1},
      {0x0062, 0, "b",         true,  true,  "ab",  1, false, nullptr, 0, 1},
      {0x0063, 0, "c",         true,  true,  "abc", 1, false, nullptr, 0, 1},
      // Bare F6: consumed BY THIS COMPONENT (consumed +1, passed +0), with the
      // preedit, the candidate count and the commit queue unchanged -- acceptances
      // 4 and 5.
      {EPOCANVA_PROBE_TEST_KEYCODE, 0, "F6", true, true, "abc", 1, false, nullptr,
       1, 0},
      // Shift+F6 is NOT the component's key: it must reach the end of the chain
      // unhandled. Without this row, "swallow every key" would also pass.
      {EPOCANVA_PROBE_TEST_KEYCODE, kShiftModifier, "Shift+F6", false, true, "abc",
       1, false, nullptr, 0, 1},
      // Acceptance 6: backspace / cancel / commit behave exactly as in the rime_raw
      // fixture even with the component at the head of the processor list.
      {kXkBackSpace, 0, "BackSpace", true, true,  "ab",  1, false, nullptr, 0, 1},
      {kXkEscape,    0, "Escape",    true, false, nullptr, 0, false, nullptr, 0, 1},
      {0x0061, 0, "a",         true,  true,  "a",   1, false, nullptr, 0, 1},
      {0x0062, 0, "b",         true,  true,  "ab",  1, false, nullptr, 0, 1},
      {0x0063, 0, "c",         true,  true,  "abc", 1, false, nullptr, 0, 1},
      {kXkReturn,  0, "Return",      true,  false, nullptr, 0, true, "abc", 0, 1},
  };
  // clang-format on

  EpoCanvaProbeCounters before = counters;
  int index = 0;
  for (const EventSpec& e : kMatrix) {
    ++index;
    EventResult r;
    const ExitCode erc =
        SendKey(api, surface, a, e.keycode, e.mask, balance, before, &r);
    if (erc != kOk) {
      api->destroy_session(a);
      return erc;
    }
    ReportEvent(index, e.keycode, e.mask, e.name, r);
    CheckEvent(checks, e.name, r, e.expect_handled, e.expect_preedit,
               e.expect_preedit_text, e.expect_candidates, e.expect_commit,
               e.expect_commit_text, e.expect_probe_consumed,
               e.expect_probe_passed);
    before = r.counters;
  }

  // A commit must not be re-readable: read the state again, sending no key.
  {
    ObservedState s;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, a, balance, &s, &failure)) {
      api->destroy_session(a);
      return failure;
    }
    std::printf("[probe] re-read after commit   %s\n", DescribeState(s).c_str());
    checks->Check(!s.commit_present, "re-read after commit: commit present",
                  s.commit_present ? s.commit_text : std::string("none"));
    checks->Check(s.preedit == "" || !s.has_preedit,
                  "re-read after commit: preedit still cleared", s.preedit);
  }

  // Acceptance 7: session isolation, including isolation of the consumed key.
  const RimeSessionId b = CreateSessionOrZero(api, "B");
  if (b == kNoSession) {
    api->destroy_session(a);
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(api, b, balance, checks, "session B");
  if (rc != kOk) {
    api->destroy_session(a);
    api->destroy_session(b);
    return rc;
  }
  {
    EventResult r;
    struct Step {
      int keycode;
      const char* name;
      const char* expect_preedit;
      bool expect_consumed;
    };
    const Step steps[] = {
        {0x0078, "x on B", "x", false},
        {0x0079, "y on B", "xy", false},
        {EPOCANVA_PROBE_TEST_KEYCODE, "F6 on B", "xy", true},
    };
    int n = 12;
    for (const Step& st : steps) {
      const ExitCode erc =
          SendKey(api, surface, b, st.keycode, 0, balance, before, &r);
      if (erc != kOk) {
        api->destroy_session(a);
        api->destroy_session(b);
        return erc;
      }
      ReportEvent(n++, st.keycode, 0, st.name, r);
      checks->Check(r.handled == true, std::string(st.name) + ": handled",
                    r.handled ? "True" : "False");
      checks->Check(r.state.has_preedit && r.state.preedit == st.expect_preedit,
                    std::string(st.name) + ": B's preedit", r.state.preedit);
      checks->Check(r.delta.test_keys_consumed == (st.expect_consumed ? 1 : 0),
                    std::string(st.name) + ": probe consumed delta",
                    std::to_string(r.delta.test_keys_consumed));
      before = r.counters;
    }
    // The same key sent to A: A owns its own instance and its own composition, and
    // B's "xy" must be untouched by what happened on A.
    const ExitCode erc =
        SendKey(api, surface, a, EPOCANVA_PROBE_TEST_KEYCODE, 0, balance, before, &r);
    if (erc != kOk) {
      api->destroy_session(a);
      api->destroy_session(b);
      return erc;
    }
    ReportEvent(15, EPOCANVA_PROBE_TEST_KEYCODE, 0, "F6 on A", r);
    checks->Check(r.handled, "F6 on A consumed", r.handled ? "True" : "False");
    checks->Check(!r.state.has_preedit,
                  "A has no preedit while B holds xy (session isolation)",
                  r.state.has_preedit ? r.state.preedit : std::string("absent"));
    checks->Check(!r.state.commit_present, "A has no commit",
                  r.state.commit_present ? r.state.commit_text
                                         : std::string("none"));
    ObservedState bs;
    ExitCode failure = kOk;
    if (!ReadObservedState(api, b, balance, &bs, &failure)) {
      api->destroy_session(a);
      api->destroy_session(b);
      return failure;
    }
    checks->Check(bs.has_preedit && bs.preedit == "xy",
                  "B still holds xy after A consumed F6", bs.preedit);
    before = r.counters;
  }

  // Acceptances 8 and 9: instances are released, and the live count returns to the
  // value observed before the round. No absolute construction count is assumed --
  // every assertion here is a relation between two observed snapshots.
  EpoCanvaProbeCounters round_start;
  if (!ReadCounters(surface, &round_start)) {
    api->destroy_session(a);
    api->destroy_session(b);
    return kProbeCounterReadFailed;
  }
  std::printf("[probe] lifetime.round.start %s\n",
              DescribeCounters(round_start).c_str());

  rc = DestroySessionAndCheck(api, b, checks, "session B");
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  {
    EpoCanvaProbeCounters after_B_gone;
    if (!ReadCounters(surface, &after_B_gone)) {
      api->destroy_session(a);
      return kProbeCounterReadFailed;
    }
    std::printf("[probe] lifetime.after_B   %s\n",
                DescribeCounters(after_B_gone).c_str());
    checks->Check(after_B_gone.live_instances < round_start.live_instances,
                  "destroying B released an instance",
                  std::to_string(after_B_gone.live_instances));
    checks->Check(
        after_B_gone.component_destroyed > round_start.component_destroyed,
        "the destroy counter rose when B was released",
        std::to_string(after_B_gone.component_destroyed));
    round_start = after_B_gone;
  }

  for (int round = 1; round <= 3; ++round) {
    EpoCanvaProbeCounters pre;
    if (!ReadCounters(surface, &pre)) {
      api->destroy_session(a);
      return kProbeCounterReadFailed;
    }
    const std::string label = "round " + std::to_string(round);
    const RimeSessionId sid = CreateSessionOrZero(api, label);
    if (sid == kNoSession) {
      api->destroy_session(a);
      return kCreateSessionFailed;
    }
    EventResult r;
    ExitCode erc = SendKey(api, surface, sid, 0x0071, 0, balance, pre, &r);
    if (erc != kOk) {
      api->destroy_session(a);
      api->destroy_session(sid);
      return erc;
    }
    ReportEvent(100 + round, 0x0071, 0, "q in round", r);
    checks->Check(r.handled && r.state.has_preedit && r.state.preedit == "q",
                  label + ": the new session composes with a live component",
                  DescribeState(r.state));
    checks->Check(r.delta.process_key_calls >= 1,
                  label + ": the new instance received the key",
                  std::to_string(r.delta.process_key_calls));
    EpoCanvaProbeCounters mid;
    if (!ReadCounters(surface, &mid)) {
      api->destroy_session(a);
      api->destroy_session(sid);
      return kProbeCounterReadFailed;
    }
    checks->Check(mid.live_instances > pre.live_instances,
                  label + ": live instances rose for this session",
                  std::to_string(mid.live_instances));
    const ExitCode crc = DestroySessionAndCheck(api, sid, checks, label);
    if (crc != kOk) {
      api->destroy_session(a);
      return crc;
    }
    EpoCanvaProbeCounters post;
    if (!ReadCounters(surface, &post)) {
      api->destroy_session(a);
      return kProbeCounterReadFailed;
    }
    checks->Check(post.live_instances == pre.live_instances,
                  label + ": live instances returned to this round's start value",
                  std::to_string(post.live_instances));
    checks->Check(post.component_constructed - post.component_destroyed ==
                      pre.component_constructed - pre.component_destroyed,
                  label + ": the round's constructions were all matched by "
                          "destructions",
                  std::to_string(post.component_constructed -
                                 post.component_destroyed));
    std::printf("[probe] lifetime.round.%d.done %s\n", round,
                DescribeCounters(post).c_str());
  }

  rc = DestroySessionAndCheck(api, a, checks, "session A");
  if (rc != kOk) {
    return rc;
  }

  // Final snapshot BEFORE finalize(): after finalize() nothing here is touched
  // again, because finalize() unloads modules and clears the registry.
  {
    EpoCanvaProbeCounters last;
    if (!ReadCounters(surface, &last)) {
      return kProbeCounterReadFailed;
    }
    std::printf("[probe] counters.final       %s\n", DescribeCounters(last).c_str());
    checks->Check(last.live_instances == 0,
                  "no instance is alive once every session is destroyed",
                  std::to_string(last.live_instances));
    checks->Check(last.component_constructed == last.component_destroyed,
                  "constructed and destroyed agree at the end",
                  std::to_string(last.component_constructed) + " vs " +
                      std::to_string(last.component_destroyed));
    checks->Check(last.module_finalized == 0,
                  "module not finalized while the surface is still held",
                  std::to_string(last.module_finalized));
    // A real invariant: every key the component entered is either released or
    // consumed, never silently dropped.
    checks->Check(last.process_key_calls ==
                      last.keys_passed_through + last.test_keys_consumed,
                  "process_key_calls == keys_passed_through + test_keys_consumed",
                  std::to_string(last.process_key_calls) + " vs " +
                      std::to_string(last.keys_passed_through +
                                     last.test_keys_consumed));
    checks->Check(last.test_keys_consumed == 3,
                  "exactly the three bare-F6 presses were consumed (matrix, B, A)",
                  std::to_string(last.test_keys_consumed));
    // 15 = 10 released by the 11 matrix keys (all but its F6) + x and y on B +
    // three 'q' in the lifetime rounds. Every one of these is already asserted
    // individually per event above; this total is what catches a key that somehow
    // reached the component twice.
    checks->Check(last.keys_passed_through == 15,
                  "exactly fifteen keys were released by the component",
                  std::to_string(last.keys_passed_through));
    checks->Check(last.process_key_calls == 18,
                  "the component was entered exactly eighteen times (11 matrix + "
                  "4 on A/B in the isolation block + 3 in the lifetime rounds)",
                  std::to_string(last.process_key_calls));
  }
  return kOk;
}

// --- mode: expect-module-absent (negative test 1, the baseline engine) -------------

ExitCode RunExpectModuleAbsent(RimeApi* api, const ProbeSurface& surface,
                               const std::filesystem::path& schema_source,
                               const std::string& staging_dir, ApiBalance* balance,
                               Checker* checks) {
  checks->Check(!surface.module_found,
                "baseline engine must not provide the probe module",
                surface.module_found ? "FOUND" : "absent");
  checks->Check(!surface.api_available,
                "baseline engine must not provide the probe diagnostics table",
                surface.api_available ? "AVAILABLE" : "absent");
  if (surface.module_found) {
    std::printf("[probe] FAIL the module '%s' is present in what this run expected "
                "to be the baseline engine\n",
                EPOCANVA_PROBE_MODULE_NAME);
    return kNegativeRunNotReproducible;
  }
  std::printf("[probe] find_module('%s')  NULL -- required for the baseline "
              "engine\n",
              EPOCANVA_PROBE_MODULE_NAME);

  ExitCode rc = DeployFixture(api, schema_source, staging_dir, checks);
  if (rc != kOk) {
    return rc;
  }
  const RimeSessionId a = CreateSessionOrZero(api, "neg1.A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(api, a, balance, checks, "neg1 session A");
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  const EpoCanvaProbeCounters before = ZeroCounters();
  EventResult r;
  // The schema still lists the component. An unregistered component name is
  // skipped by CreateComponentsFromList (src/rime/engine.cc:310-315), so the rest
  // of the chain must keep working -- otherwise this run would say nothing about
  // the missing component in particular.
  rc = SendKey(api, surface, a, 0x0061, 0, balance, before, &r);
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  ReportEvent(1, 0x0061, 0, "a", r);
  checks->Check(r.handled && r.state.has_preedit && r.state.preedit == "a",
                "letters still compose without the component",
                DescribeState(r.state));
  rc = SendKey(api, surface, a, EPOCANVA_PROBE_TEST_KEYCODE, 0, balance, r.counters,
               &r);
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  ReportEvent(2, EPOCANVA_PROBE_TEST_KEYCODE, 0, "F6", r);
  checks->Check(!r.handled, "F6 must NOT be consumed by an absent component",
                r.handled ? "consumed" : "not consumed");
  checks->Check(r.state.has_preedit && r.state.preedit == "a",
                "F6 left the preedit alone",
                r.state.has_preedit ? r.state.preedit : std::string("absent"));
  checks->Check(!r.state.commit_present, "F6 produced no commit",
                r.state.commit_present ? r.state.commit_text
                                       : std::string("none"));
  if (r.handled) {
    std::printf("[probe] FAIL the baseline engine consumed the probe's key\n");
    return kNegativeRunNotReproducible;
  }
  return DestroySessionAndCheck(api, a, checks, "neg1 session A");
}

// --- mode: expect-component-absent (negative test 2) --------------------------------

ExitCode RunExpectComponentAbsent(RimeApi* api, const ProbeSurface& surface,
                                  const std::filesystem::path& schema_source,
                                  const std::string& staging_dir,
                                  ApiBalance* balance, Checker* checks) {
  if (!surface.api_available) {
    Fail(kProbeSurfaceUnavailable, "probe.surface",
         "this run needs the probe ENGINE (the module present) with the component "
         "absent from the schema; the loaded engine exposes no probe module at all, "
         "so it would not be testing what its name says");
    return kProbeSurfaceUnavailable;
  }
  EpoCanvaProbeCounters c;
  if (!ReadCounters(surface, &c)) {
    return kProbeCounterReadFailed;
  }
  std::printf("[probe] counters.after_init  %s\n", DescribeCounters(c).c_str());
  checks->Check(c.module_initialized >= 1,
                "the module itself is still loaded (only the component is gone)",
                std::to_string(c.module_initialized));
  checks->Check(c.process_key_calls == 0, "nothing entered the component yet",
                std::to_string(c.process_key_calls));

  ExitCode rc = DeployFixture(api, schema_source, staging_dir, checks);
  if (rc != kOk) {
    return rc;
  }
  const RimeSessionId a = CreateSessionOrZero(api, "neg2.A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(api, a, balance, checks, "neg2 session A");
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  EpoCanvaProbeCounters after_select;
  if (!ReadCounters(surface, &after_select)) {
    api->destroy_session(a);
    return kProbeCounterReadFailed;
  }
  const CounterDelta d = DeltaOf(c, after_select);
  std::printf("[probe] constructed.delta    %lld (expected 0)\n", d.constructed);
  checks->Check(d.constructed == 0,
                "no component instance was constructed for this schema",
                std::to_string(d.constructed));

  EventResult r;
  rc = SendKey(api, surface, a, 0x0061, 0, balance, after_select, &r);
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  ReportEvent(1, 0x0061, 0, "a", r);
  checks->Check(r.handled && r.state.has_preedit && r.state.preedit == "a",
                "letters still compose without the component",
                DescribeState(r.state));
  rc = SendKey(api, surface, a, EPOCANVA_PROBE_TEST_KEYCODE, 0, balance, r.counters,
               &r);
  if (rc != kOk) {
    api->destroy_session(a);
    return rc;
  }
  ReportEvent(2, EPOCANVA_PROBE_TEST_KEYCODE, 0, "F6", r);
  checks->Check(!r.handled, "F6 must NOT be consumed by an absent component",
                r.handled ? "consumed" : "not consumed");
  checks->Check(r.delta.process_key_calls == 0, "the component was never entered",
                std::to_string(r.delta.process_key_calls));
  checks->Check(r.delta.test_keys_consumed == 0, "nothing was consumed",
                std::to_string(r.delta.test_keys_consumed));
  checks->Check(r.state.has_preedit && r.state.preedit == "a",
                "F6 left the preedit alone",
                r.state.has_preedit ? r.state.preedit : std::string("absent"));
  if (r.handled || r.delta.process_key_calls != 0) {
    std::printf("[probe] FAIL a schema that does not list the component still "
                "reached it\n");
    return kNegativeRunNotReproducible;
  }
  return DestroySessionAndCheck(api, a, checks, "neg2 session A");
}

// --- teardown shared by every mode -------------------------------------------------

ExitCode VerifyAfterFinalize(RimeApi* api, Checker* checks) {
  // Deliberately nothing is read from the probe surface here: finalize() runs
  // ModuleManager::UnloadModules() and Registry::Clear()
  // (src/rime_api_impl.h:58-63), so pointers obtained through the engine are not
  // promised to remain valid.
  Log("post.finalize", "the probe surface is not touched after finalize()");
  const RimeSessionId late = api->create_session();
  if (late != kNoSession) {
    api->destroy_session(late);
    checks->Check(false, "create_session() after finalize()", "succeeded");
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
    std::printf(
        "usage: rime_component_probe --expect-dll <path> --fixture-dir <path> "
        "--run-root <path> [--mode probe|expect-module-absent|"
        "expect-component-absent]\n");
    std::printf("[probe] FAIL arguments: %s\n", error.c_str());
    return kUsage;
  }

  std::printf("[probe] mode                 %s\n", ModeName(opt.mode));
  std::printf("[probe] module name          %s\n", EPOCANVA_PROBE_MODULE_NAME);
  std::printf("[probe] component name       %s\n", EPOCANVA_PROBE_PROCESSOR_NAME);
  std::printf("[probe] test key             XK_F6 = 0x%04x with mask 0\n",
              EPOCANVA_PROBE_TEST_KEYCODE);
  std::printf("[probe] units                keysyms are 32-bit X11 identifiers; "
              "mask is librime's modifier bits (src/rime/key_table.h:13-43); "
              "preedit length is a byte (UTF-8 code unit) offset; counters are "
              "event counts\n");
  std::printf("[probe] sentinels            preedit=- means "
              "composition.preedit == NULL; commit=- means get_commit() returned "
              "False\n");
  std::printf("[probe] scope                a compiled-in component reachability "
              "check only -- NOT evidence of completion, prediction, correction, "
              "segmentation or any other language capability\n");

  std::filesystem::path shared_dir, user_dir, log_dir;
  ExitCode rc = PrepareRunRoot(opt, &shared_dir, &user_dir, &log_dir);
  if (rc != kOk) {
    return rc;
  }
  std::filesystem::path schema_source;
  rc = StageFixture(opt, shared_dir, &schema_source);
  if (rc != kOk) {
    if (rc == kFixturePreconditionFailed) {
      Fail(rc, "fixture.precondition",
           "required fixture files are missing from --fixture-dir");
    }
    return rc;
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

  // Observed BEFORE any engine call. RIME_REGISTER_CUSTOM_MODULE's .CRT$XCU
  // initializer runs when rime.dll is loaded (src/rime_api.h:528-533), so a
  // compiled-in module is already findable here while its initialize() has not run
  // yet. A find that succeeds here AND non-zero module_initialized later can only
  // have come from LoadModules(), which is the mechanism under test.
  const ProbeSurface before_load = AcquireProbeSurface(api);
  std::printf("[probe] module.before_setup  %s\n",
              before_load.module_found ? "registered by DLL load" : "absent");

  const std::string shared_s = shared_dir.string();
  const std::string user_s = user_dir.string();
  const std::string log_s = log_dir.string();

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared_s.c_str();
  traits.user_data_dir = user_s.c_str();
  traits.log_dir = log_s.c_str();
  traits.prebuilt_data_dir = nullptr;  // -> <shared>/build (src/rime/setup.cc:58-61)
  traits.staging_dir = nullptr;        // -> <user>/build  (src/rime/setup.cc:62-65)
  traits.app_name = "rime.epocanva.component_probe";
  traits.distribution_name = "EpoCanva";
  traits.distribution_code_name = "epocanva";
  traits.distribution_version = "0.0.0-p1.4";
  traits.min_log_level = 3;            // FATAL; inert while logging is OFF
  // "default" = core + dict + gears (src/rime/setup.cc:30); "levers" adds the
  // deployment task components (src/rime/lever/levers_module.cc:20-31).
  // EPOCANVA_PROBE_MODULE_NAME is EpoCanva's own module, and naming it here is what
  // loads it: RimeInitialize passes traits->modules straight to LoadModules
  // (src/rime_api_impl.h:51-54). It is listed in EVERY mode, including the baseline
  // run, because that is exactly the front-end configuration the negative test is
  // about: a name the engine does not know is skipped silently
  // (src/rime/setup.cc:33-40), which is why a behavioural assertion -- not a return
  // code -- is required to tell the two engines apart.
  static const char* kModules[] = {"default", "levers", EPOCANVA_PROBE_MODULE_NAME,
                                   nullptr};
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
    // terminate on truncation, so a full buffer is treated as failure.
    std::vector<char> buffer(kPathBuffer, '\0');
    api->get_shared_data_dir_s(buffer.data(), buffer.size());
    const bool shared_ok =
        strnlen(buffer.data(), buffer.size()) < buffer.size() - 1 &&
        std::strcmp(buffer.data(), shared_s.c_str()) == 0;
    buffer.assign(kPathBuffer, '\0');
    api->get_user_data_dir_s(buffer.data(), buffer.size());
    const bool user_ok =
        strnlen(buffer.data(), buffer.size()) < buffer.size() - 1 &&
        std::strcmp(buffer.data(), user_s.c_str()) == 0;
    if (!shared_ok || !user_ok) {
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
        std::printf("[probe] staging_dir          %s\n", staging_dir.c_str());
      }
    }
  }

  Checker checks;
  ApiBalance balance;
  if (rc == kOk) {
    const ProbeSurface surface = AcquireProbeSurface(api);
    std::printf("[probe] module.after_init  %s  diagnostics=%s\n",
                surface.module_found ? "found" : "absent",
                surface.api_available ? "available" : "unavailable");
    switch (opt.mode) {
      case Mode::kProbe:
        rc = RunProbeMode(api, surface, schema_source, staging_dir, &balance,
                          &checks);
        break;
      case Mode::kExpectModuleAbsent:
        rc = RunExpectModuleAbsent(api, before_load, schema_source, staging_dir,
                                   &balance, &checks);
        break;
      default:
        rc = RunExpectComponentAbsent(api, surface, schema_source, staging_dir,
                                      &balance, &checks);
        break;
    }
  }

  if (initialize_called) {
    api->finalize();
    Log("cleanup.finalize", "finalize() returned");
    if (rc == kOk) {
      const ExitCode post = VerifyAfterFinalize(api, &checks);
      if (post != kOk) {
        rc = post;
      }
    }
  } else if (rc == kOk) {
    rc = kApiUnavailable;
    Fail(rc, "cleanup", "initialize() was never called");
  }

  if (!balance.balanced()) {
    std::printf("[probe] FAIL api.balance: %d result(s) obtained and not freed\n",
                balance.outstanding());
    if (rc == kOk) {
      rc = kApiResultUnbalanced;
    }
  } else {
    std::printf("[probe] api.balance          status %d/%d context %d/%d "
                "commit %d/%d -- paired\n",
                balance.status_get, balance.status_free, balance.context_get,
                balance.context_free, balance.commit_get, balance.commit_free);
  }

  {
    const int files = CountFilesBeneath(log_dir);
    // ENABLE_LOGGING=OFF compiles SetupLogging()'s body away
    // (src/rime/setup.cc:71-95) and LOG()/DLOG() expand to no-ops
    // (src/rime/no_logging.h). Nothing may land here, and the probe component
    // itself contains no log statement at all.
    if (files != 0) {
      std::printf("[probe] FAIL post.finalize.log: %d file(s) under the log "
                  "directory\n",
                  files);
      if (rc == kOk) {
        rc = kLogFilesWritten;
      }
    } else {
      Log("post.finalize", "log directory stayed empty");
    }
  }

  std::printf("[probe] assertions           %d checked, %d failed\n",
              checks.total(), checks.failed());
  if (rc == kOk && checks.failed() > 0) {
    rc = (opt.mode == Mode::kProbe) ? kAssertionFailed
                                    : kNegativeRunNotReproducible;
  }
  if (rc == kOk) {
    Log("RESULT", "PASS");
  }
  return rc;
}
