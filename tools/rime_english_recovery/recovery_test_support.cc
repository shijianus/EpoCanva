// EpoCanva P2.6-recovery -- implementation of the shared integration-test harness.
// See recovery_test_support.h for what this observes and the units it asserts in.
#include "recovery_test_support.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace epocanva {
namespace recovery {
namespace {

constexpr std::size_t kPathBuffer = 4096;

constexpr char kFixtureDefaultYaml[] = "default.yaml";
constexpr char kFixtureSchemaYaml[] = "epocanva_test_english.schema.yaml";
constexpr char kFixtureLexicon[] = "lexicon.tsv";
// The name the fixture's english/lexicon_path key asks for, relative to the engine's
// own shared data directory (src/rime/setup.cc:46-47, src/rime/deployer.h:35) -- never
// the process working directory.
constexpr char kStagedLexicon[] = "lexicon/epocanva_test_lexicon.tsv";
// The name the compiler writes the staged schema under: <schema_id>.schema.yaml, which
// for this fixture is the same file name the fixture directory uses.
constexpr char kCompiledSchemaYaml[] = "epocanva_test_english.schema.yaml";

// A run directory may only be deleted when its parent carries exactly this name, which
// nothing outside this project's generated CTest commands produces.
constexpr char kRunRootGuard[] = "rime-english-recovery-run";

std::wstring NormalizeWide(const std::wstring& raw) {
  std::vector<wchar_t> buffer(kPathBuffer, L'\0');
  const DWORD n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(buffer.size()),
                                   buffer.data(), nullptr);
  if (n == 0 || n >= buffer.size()) {
    return raw;
  }
  return std::wstring(buffer.data(), n);
}

// The module that actually supplies the code behind a function pointer, so a stale
// rime.dll reachable through PATH can never be mistaken for the one this run was built
// against.
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

// Copies one fixture file into this run's own directory. tests/fixtures/ is only ever
// read, so a negative case cannot leave a modified fixture behind for the next run.
ExitCode CopyInto(const std::filesystem::path& src,
                  const std::filesystem::path& dst) {
  std::error_code ec;
  std::filesystem::create_directories(dst.parent_path(), ec);
  if (ec) {
    Fail(kRunDirectoryFailed, "fixture.mkdir", dst.parent_path().string());
    return kRunDirectoryFailed;
  }
  std::filesystem::copy_file(src, dst,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    Fail(kRunDirectoryFailed, "fixture.copy",
         src.string() + " -> " + dst.string() + " (" + ec.message() + ")");
    return kRunDirectoryFailed;
  }
  const std::uintmax_t size = std::filesystem::file_size(dst, ec);
  if (ec) {
    Fail(kRunDirectoryFailed, "fixture.verify",
         dst.string() + " (" + ec.message() + ")");
    return kRunDirectoryFailed;
  }
  Log("fixture.staged",
      dst.string() + " (" + std::to_string(static_cast<unsigned long long>(size)) +
          " bytes)");
  return kOk;
}

ExitCode PrepareRunRoot(const Options& opt, std::filesystem::path* shared_dir,
                        std::filesystem::path* user_dir,
                        std::filesystem::path* log_dir) {
  const std::filesystem::path parent = opt.run_root.parent_path();
  if (parent.filename().u8string() != kRunRootGuard) {
    std::printf(
        "[english] FAIL run.root: --run-root %s is not a leaf under a directory "
        "named '%s'; refusing to delete anything here\n",
        opt.run_root.string().c_str(), kRunRootGuard);
    return kRunDirectoryFailed;
  }
  std::error_code ec;
  if (std::filesystem::exists(opt.run_root, ec)) {
    std::filesystem::remove_all(opt.run_root, ec);
    if (ec) {
      std::printf("[english] FAIL run.root: cannot remove the previous run "
                  "directory %s (%s)\n",
                  opt.run_root.string().c_str(), ec.message().c_str());
      return kRunDirectoryFailed;
    }
  }
  *shared_dir = opt.run_root / "shared";
  *user_dir = opt.run_root / "user";
  *log_dir = opt.run_root / "log";
  for (const std::filesystem::path& d : {*shared_dir, *user_dir, *log_dir}) {
    std::filesystem::create_directories(d, ec);
    if (ec) {
      std::printf("[english] FAIL run.root: cannot create %s (%s)\n",
                  d.string().c_str(), ec.message().c_str());
      return kRunDirectoryFailed;
    }
  }
  std::printf("[english] run-root                 %s (recreated empty)\n",
              opt.run_root.string().c_str());
  return kOk;
}

ExitCode StageFixture(const Options& opt, const std::filesystem::path& shared_dir,
                      std::filesystem::path* schema_source) {
  const std::filesystem::path default_src = opt.fixture_dir / kFixtureDefaultYaml;
  const std::filesystem::path schema_src = opt.fixture_dir / kFixtureSchemaYaml;
  const std::filesystem::path lexicon_src = opt.fixture_dir / kFixtureLexicon;
  std::error_code ec;
  bool absent = false;
  for (const std::filesystem::path& p : {default_src, schema_src, lexicon_src}) {
    if (!std::filesystem::is_regular_file(p, ec) || ec) {
      std::printf("[english] fixture.absent             %s\n", p.string().c_str());
      absent = true;
    }
  }
  if (absent) {
    return kFixtureMissing;
  }
  ExitCode rc = CopyInto(default_src, shared_dir / kFixtureDefaultYaml);
  if (rc != kOk) {
    return rc;
  }
  rc = CopyInto(lexicon_src, shared_dir / kStagedLexicon);
  if (rc != kOk) {
    return rc;
  }
  rc = CopyInto(schema_src, shared_dir / kFixtureSchemaYaml);
  if (rc != kOk) {
    return rc;
  }
  *schema_source = shared_dir / kFixtureSchemaYaml;
  return kOk;
}

ExitCode VerifyLoadedModule(const Options& opt, RimeApi* api) {
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
  if (_wcsicmp(actual.c_str(), NormalizeWide(opt.expect_dll).c_str()) != 0) {
    std::printf("[english] FAIL module.identity\n");
    std::printf("  expected: %ls\n  actual  : %ls\n", opt.expect_dll.c_str(),
                actual.c_str());
    Fail(kLoadedModuleMismatch, "module.identity",
         "the loaded module is not the artefact this run was told to test");
    return kLoadedModuleMismatch;
  }
  // Recorded, not merely used: the report must name the DLL the assertions ran
  // against, whoever reads the log later.
  std::printf("[english] module-loaded            %ls\n", actual.c_str());
  std::printf("[english] librime-version          %s\n", api->get_version());
  return kOk;
}

// Reads one of the engine's own directory slots. The caller proves with
// RIME_API_AVAILABLE that the slot exists before handing its address over, because
// reading a member the loaded table does not have is exactly the out-of-bounds read
// that check exists to prevent. buffer_size counts bytes and the implementation
// strncpy()s without guaranteeing termination, so a full buffer is a failure rather
// than a truncation.
ExitCode ReadDirSlot(void (*slot)(char*, std::size_t), const char* what,
                     std::string* out) {
  if (slot == nullptr) {
    Fail(kTraitsNotApplied, "traits.readback",
         std::string(what) + ": the loaded API table has no such slot");
    return kTraitsNotApplied;
  }
  std::vector<char> buffer(kPathBuffer, '\0');
  slot(buffer.data(), buffer.size());
  if (buffer[kPathBuffer - 1] != '\0') {
    Fail(kTraitsNotApplied, "traits.readback",
         std::string(what) + ": the path did not fit the read buffer");
    return kTraitsNotApplied;
  }
  out->assign(buffer.data());
  std::printf("[english] %-27s %s\n", what, out->c_str());
  return kOk;
}

}  // namespace

// --- plumbing -----------------------------------------------------------------

void Log(const char* stage, const std::string& detail) {
  std::printf("[english] %-27s %s\n", stage, detail.c_str());
}

void Fail(ExitCode code, const char* stage, const std::string& detail) {
  std::printf("[english] FAIL %-22s %s (stage code %d)\n", stage, detail.c_str(),
              static_cast<int>(code));
}

void Checker::Check(bool condition, const std::string& what,
                    const std::string& got) {
  ++total_;
  if (condition) {
    return;
  }
  ++failed_;
  std::printf("[english]   ASSERT FAIL %s: got \"%s\"\n", what.c_str(), got.c_str());
}

void Checker::CheckStr(const std::string& got, const std::string& want,
                       const std::string& what) {
  Check(got == want, what, got.empty() ? "(empty)" : got.c_str());
}

void Checker::CheckNum(long long got, long long want, const std::string& what) {
  Check(got == want, what, std::to_string(got));
}

void Checker::CheckMenu(const StateSnapshot& state, const std::string& want_pipe_list,
                        const std::string& what) {
  const std::vector<std::string> want = SplitPipeList(want_pipe_list);
  Check(static_cast<long long>(state.candidates.size()) ==
            static_cast<long long>(want.size()),
        what + " [item count]", std::to_string(state.candidates.size()));
  for (std::size_t i = 0; i < want.size(); ++i) {
    const std::string got = i < state.candidates.size() ? state.candidates[i]
                                                        : std::string("(absent)");
    Check(i < state.candidates.size() && got == want[i],
          what + " [index " + std::to_string(i) + "]", got);
  }
}

std::vector<std::string> SplitPipeList(const std::string& joined) {
  std::vector<std::string> out;
  if (joined.empty()) {
    return out;
  }
  std::size_t pos = 0;
  while (pos <= joined.size()) {
    const std::size_t bar = joined.find('|', pos);
    if (bar == std::string::npos) {
      out.push_back(joined.substr(pos));
      break;
    }
    out.push_back(joined.substr(pos, bar - pos));
    pos = bar + 1;
    if (pos == joined.size()) {
      out.push_back(std::string());  // a trailing separator means an empty last item
      break;
    }
  }
  return out;
}

std::string JoinMenu(const StateSnapshot& state) {
  std::string line;
  for (std::size_t i = 0; i < state.candidates.size(); ++i) {
    if (i != 0) {
      line += "|";
    }
    line += state.candidates[i];
  }
  return line;
}

std::string JoinComments(const StateSnapshot& state) {
  std::string line;
  for (std::size_t i = 0; i < state.comments.size(); ++i) {
    if (i != 0) {
      line += "|";
    }
    line += state.comments[i];
  }
  return line;
}

void CheckFrame(Checker* checks, const KeyEvent& e, const std::string& want_raw,
                 const std::string& want_menu, long want_page_size,
                 const std::string& label) {
  const std::vector<std::string> want = SplitPipeList(want_menu);
  checks->Check(e.handled, label + ": process_key() handled the key",
                e.handled ? "True" : "False");
  checks->CheckStr(e.state.raw_input, want_raw,
                   label + ": get_input() raw composition");
  checks->Check(e.state.has_preedit, label + ": preedit present",
                e.state.has_preedit ? e.state.preedit : std::string("absent"));
  checks->CheckStr(e.state.preedit, want_raw, label + ": preedit text");
  checks->CheckNum(e.state.preedit_length,
                   static_cast<long long>(want_raw.size()),
                   label + ": preedit length (UTF-8 code units)");
  checks->CheckNum(e.state.num_candidates, static_cast<long long>(want.size()),
                   label + ": candidate count");
  checks->CheckNum(e.state.page_size, want_page_size, label + ": menu page_size");
  checks->CheckMenu(e.state, want_menu, label + ": menu");
  // An empty comment column, one empty field per candidate. A label here would mean
  // this run deployed a schema with the suggestion feature on.
  checks->CheckStr(
      JoinComments(e.state),
      want.empty() ? std::string() : std::string(want.size() - 1, '|'),
      label + ": empty comment column");
  if (!want.empty()) {
    checks->CheckStr(e.state.candidates[0], want_raw,
                     label + ": candidate 0 is the typed text");
  }
}

std::string DescribeState(const StateSnapshot& s) {  char buf[96];
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
  std::snprintf(buf, sizeof(buf), "cands=%ld page=%ld raw=\"%s\" ", s.num_candidates,
                s.page_size, s.raw_input.c_str());
  line += buf;
  line += "list=" + JoinMenu(s);
  const std::string comments = JoinComments(s);
  if (!comments.empty()) {
    line += " cmts=" + comments;
  }
  line += " ";
  line += s.commit_present ? ("commit=\"" + s.commit_text + "\"") : "commit=-";
  return line;
}

std::string DescribeCounters(const EpoCanvaEnglishCounters& c) {
  char buf[896];
  std::snprintf(buf, sizeof(buf),
                "mod_init=%llu mod_fin=%llu xl_ctor=%llu xl_dstr=%llu xl_live=%llu "
                "attempts=%llu ok=%llu fail=%llu entries=%llu errline=%llu "
                "queries=%llu with_index=%llu rejected=%llu orig=%llu compl=%llu "
                "cl=%llu ci=%llu cu=%llu delimited=%llu "
                "nomatch=%llu mixed=%llu notalpha=%llu toolong=%llu illegal=%llu "
                "protected=%llu nonascii=%llu unsup=%llu "
                "whole=%llu part=%llu spell=%lld",
                static_cast<unsigned long long>(c.module_initialized),
                static_cast<unsigned long long>(c.module_finalized),
                static_cast<unsigned long long>(c.translator_constructed),
                static_cast<unsigned long long>(c.translator_destroyed),
                static_cast<unsigned long long>(c.translator_live),
                static_cast<unsigned long long>(c.lexicon_load_attempts),
                static_cast<unsigned long long>(c.lexicon_load_successes),
                static_cast<unsigned long long>(c.lexicon_load_failures),
                static_cast<unsigned long long>(c.loaded_entry_count),
                static_cast<unsigned long long>(c.last_load_error_line),
                static_cast<unsigned long long>(c.queries),
                static_cast<unsigned long long>(c.queries_with_index),
                static_cast<unsigned long long>(c.queries_rejected),
                static_cast<unsigned long long>(c.original_candidates),
                static_cast<unsigned long long>(c.completion_candidates),
                static_cast<unsigned long long>(c.completions_lowercase),
                static_cast<unsigned long long>(c.completions_initial_capital),
                static_cast<unsigned long long>(c.completions_uppercase),
                static_cast<unsigned long long>(c.completions_with_delimiter),
                static_cast<unsigned long long>(c.queries_no_match),
                static_cast<unsigned long long>(c.queries_mixed_case),
                static_cast<unsigned long long>(c.queries_not_alphabetic),
                static_cast<unsigned long long>(c.queries_too_long),
                static_cast<unsigned long long>(c.queries_illegal_word_form),
                static_cast<unsigned long long>(c.queries_protected),
                static_cast<unsigned long long>(c.queries_non_ascii),
                static_cast<unsigned long long>(c.queries_unsupported_byte),
                static_cast<unsigned long long>(c.queries_covering_whole_input),
                static_cast<unsigned long long>(c.queries_covering_part_of_input),
                SpellingTotal(c));
  return std::string(buf);
}

long long CounterDelta(const EpoCanvaEnglishCounters& before,
                       const EpoCanvaEnglishCounters& after,
                       unsigned long long EpoCanvaEnglishCounters::*field) {
  return static_cast<long long>(after.*field) - static_cast<long long>(before.*field);
}

long long SpellingTotal(const EpoCanvaEnglishCounters& c) {
  const unsigned long long EpoCanvaEnglishCounters::*fields[] = {
      &EpoCanvaEnglishCounters::spelling_suggested,
      &EpoCanvaEnglishCounters::spelling_no_candidate,
      &EpoCanvaEnglishCounters::spelling_candidates_appended,
      &EpoCanvaEnglishCounters::spelling_variants_generated,
      &EpoCanvaEnglishCounters::spelling_lexicon_lookups,
      &EpoCanvaEnglishCounters::spelling_skipped_word_in_lexicon,
      &EpoCanvaEnglishCounters::spelling_skipped_completion,
      &EpoCanvaEnglishCounters::spelling_skipped_protected,
      &EpoCanvaEnglishCounters::spelling_skipped_delimited,
      &EpoCanvaEnglishCounters::spelling_skipped_mixed_case,
      &EpoCanvaEnglishCounters::spelling_skipped_too_short,
      &EpoCanvaEnglishCounters::spelling_skipped_too_long,
      &EpoCanvaEnglishCounters::spelling_skipped_unsupported_byte,
      &EpoCanvaEnglishCounters::spelling_skipped_non_ascii,
      &EpoCanvaEnglishCounters::spelling_skipped_empty,
      &EpoCanvaEnglishCounters::spelling_lexicon_unavailable,
      &EpoCanvaEnglishCounters::spelling_suggestions_lowercase,
      &EpoCanvaEnglishCounters::spelling_suggestions_initial_capital,
      &EpoCanvaEnglishCounters::spelling_suggestions_uppercase,
  };
  long long total = 0;
  for (const auto& field : fields) {
    total += static_cast<long long>(c.*field);
  }
  return total;
}

std::string DescribeDelta(const EpoCanvaEnglishCounters& before,
                          const EpoCanvaEnglishCounters& after) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "d{queries=%lld with_index=%lld compl=%lld attempts=%lld}",
                static_cast<long long>(after.queries - before.queries),
                static_cast<long long>(after.queries_with_index -
                                       before.queries_with_index),
                static_cast<long long>(after.completion_candidates -
                                       before.completion_candidates),
                static_cast<long long>(after.lexicon_load_attempts -
                                       before.lexicon_load_attempts));
  return std::string(buf);
}

bool ModeFromName(const std::string& name, Mode* out) {
  if (name == "completion") {
    *out = Mode::kCompletion;
    return true;
  }
  if (name == "expect-module-absent") {
    *out = Mode::kExpectModuleAbsent;
    return true;
  }
  if (name == "self-check-fail") {
    *out = Mode::kSelfCheckFail;
    return true;
  }
  if (name == "case-preservation") {
    *out = Mode::kCasePreservation;
    return true;
  }
  if (name == "case-self-check-fail") {
    *out = Mode::kCaseSelfCheckFail;
    return true;
  }
  if (name == "token-protection") {
    *out = Mode::kTokenProtection;
    return true;
  }
  if (name == "token-self-check-fail") {
    *out = Mode::kTokenSelfCheckFail;
    return true;
  }
  return false;
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kCompletion:
      return "completion";
    case Mode::kExpectModuleAbsent:
      return "expect-module-absent";
    case Mode::kSelfCheckFail:
      return "self-check-fail";
    case Mode::kCasePreservation:
      return "case-preservation";
    case Mode::kCaseSelfCheckFail:
      return "case-self-check-fail";
    case Mode::kTokenProtection:
      return "token-protection";
    case Mode::kTokenSelfCheckFail:
      return "token-self-check-fail";
  }
  return "?";
}

bool ParseArgs(int argc, char** argv, Options* out, std::string* error) {
  std::string mode_name = "completion";
  std::string expect_dll, fixture_dir, run_root;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string* slot = nullptr;
    if (arg == "--expect-dll") {
      slot = &expect_dll;
    } else if (arg == "--fixture-dir") {
      slot = &fixture_dir;
    } else if (arg == "--run-root") {
      slot = &run_root;
    } else if (arg == "--mode") {
      slot = &mode_name;
    } else {
      *error = "unrecognised argument '" + arg + "'";
      return false;
    }
    if (i + 1 >= argc) {
      *error = "'" + arg + "' requires a value";
      return false;
    }
    *slot = argv[++i];
  }
  if (expect_dll.empty() || fixture_dir.empty() || run_root.empty()) {
    *error = "--expect-dll, --fixture-dir and --run-root are all required";
    return false;
  }
  // The C API takes const char* and the Windows path APIs here take the ANSI code
  // page, so a path outside printable ASCII is refused instead of silently converted.
  for (const std::string& p : {expect_dll, fixture_dir, run_root}) {
    for (const unsigned char ch : p) {
      if (ch < 0x20 || ch > 0x7E) {
        *error = "paths must be printable ASCII; this program does not guess an "
                 "encoding for the C boundary";
        return false;
      }
    }
  }
  out->expect_dll = std::wstring(expect_dll.begin(), expect_dll.end());
  out->fixture_dir = std::filesystem::path(expect_dll.empty() ? "" : fixture_dir);
  out->run_root = std::filesystem::path(run_root);
  if (!ModeFromName(mode_name, &out->mode)) {
    *error = "--mode '" + mode_name + "' is not a mode this program implements";
    return false;
  }
  return true;
}

// --- observation --------------------------------------------------------------

bool ReadState(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
               StateSnapshot* out, ExitCode* failure) {
  *out = StateSnapshot();  // a previous event's fields must not leak into this one
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
    out->page_size = context.menu.page_size;
    out->highlighted = context.menu.highlighted_candidate_index;
    const long count = context.menu.num_candidates;
    if (count > 0 && context.menu.candidates != nullptr) {
      // The menu array belongs to the context and is released by free_context(), so
      // both columns are copied out before it goes away.
      for (long i = 0; i < count; ++i) {
        out->candidates.push_back(context.menu.candidates[i].text
                                      ? context.menu.candidates[i].text
                                      : std::string());
        out->comments.push_back(context.menu.candidates[i].comment
                                    ? context.menu.candidates[i].comment
                                    : std::string());
      }
    }
    api->free_context(&context);
    ++balance->context_free;
  }
  if (api->get_input) {
    const char* input = api->get_input(sid);
    // No free slot exists for this pointer and any edit invalidates it, so it is
    // copied here and never released.
    out->raw_input = input ? input : "(null)";
  }
  bool present = false;
  std::string text;
  const ExitCode commit_rc = ReadCommitOnce(api, sid, balance, &present, &text);
  if (commit_rc != kOk) {
    *failure = commit_rc;
    return false;
  }
  out->commit_present = present;
  out->commit_text = text;
  return true;
}

ExitCode ReadCommitOnce(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
                        bool* present, std::string* text) {
  *present = false;
  text->clear();
  RIME_STRUCT(RimeCommit, commit);
  const Bool got = api->get_commit(sid, &commit);
  if (got == True) {
    ++balance->commit_get;
    *present = true;
    *text = commit.text ? commit.text : "(null text)";
    api->free_commit(&commit);
    ++balance->commit_free;
    return kOk;
  }
  if (got != False) {
    return kGetCommitFailed;
  }
  return kOk;
}

EnglishSurface AcquireEnglishSurface(RimeApi* api) {
  EnglishSurface s;
  if (!RIME_API_AVAILABLE(api, find_module)) {
    return s;
  }
  s.module = api->find_module(EPOCANVA_ENGLISH_MODULE_NAME);
  s.module_found = (s.module != nullptr);
  if (!s.module || !s.module->get_api) {
    return s;
  }
  RimeCustomApi* raw = s.module->get_api();
  if (raw == nullptr) {
    return s;
  }
  // The same type pun upstream itself uses when handing a table out
  // (src/rime/lever/levers_api_impl.h:279). It is made safe by the data_size check,
  // not by the cast.
  const EpoCanvaEnglishApi* table = reinterpret_cast<const EpoCanvaEnglishApi*>(raw);
  const int payload = static_cast<int>(sizeof(EpoCanvaEnglishApi) - sizeof(int));
  if (table->data_size < payload || table->get_counters == nullptr ||
      table->last_load_status_name == nullptr || table->build_id == nullptr) {
    return s;
  }
  s.api = table;
  s.api_available = true;
  return s;
}

bool ReadCounters(const EnglishSurface& surface, EpoCanvaEnglishCounters* out) {
  if (!surface.api_available) {
    return false;
  }
  std::memset(out, 0, sizeof(*out));
  RIME_STRUCT_INIT(EpoCanvaEnglishCounters, *out);
  return surface.api->get_counters(out) == True;
}

std::string LastStatusName(const EnglishSurface& surface) {
  if (!surface.api_available || surface.api->last_load_status_name == nullptr) {
    return "(surface unavailable)";
  }
  const char* name = surface.api->last_load_status_name();
  return name ? std::string(name) : std::string("(null)");
}

// --- keys ---------------------------------------------------------------------

ExitCode SendKey(RimeApi* api, RimeSessionId sid, int keycode, int mask,
                 ApiBalance* balance, KeyEvent* out) {
  const Bool handled = api->process_key(sid, keycode, mask);
  out->handled = (handled == True);
  if (handled != True && handled != False) {
    Fail(kProcessKeyIndeterminate, "key.process",
         "process_key() returned neither True nor False");
    return kProcessKeyIndeterminate;
  }
  ExitCode failure = kOk;
  if (!ReadState(api, sid, balance, &out->state, &failure)) {
    return failure;
  }
  return kOk;
}

ExitCode TypeText(RimeApi* api, RimeSessionId sid, const std::string& text,
                  ApiBalance* balance, std::vector<KeyEvent>* events) {
  for (const char ch : text) {
    KeyEvent event;
    const int keycode = static_cast<int>(static_cast<unsigned char>(ch));
    const ExitCode rc = SendKey(api, sid, keycode, 0, balance, &event);
    if (rc != kOk) {
      return rc;
    }
    std::printf("[english] ev=%02d key=0x%04x '%s'        handled=%d %s\n",
                static_cast<int>(events->size()), keycode, text.c_str(),
                event.handled ? 1 : 0, DescribeState(event.state).c_str());
    events->push_back(event);
  }
  return kOk;
}

ExitCode SelectCandidate(RimeApi* api, RimeSessionId sid, std::size_t index,
                         bool* accepted) {
  if (!RIME_API_AVAILABLE(api, select_candidate)) {
    Fail(kSelectCandidateRefused, "select_candidate.slot",
         "the loaded API table has no select_candidate slot");
    return kSelectCandidateRefused;
  }
  const Bool ok = api->select_candidate(sid, index);
  *accepted = (ok == True);
  std::printf("[english] select_candidate           index=%zu %s\n", index,
              *accepted ? "accepted" : "REFUSED");
  return kOk;
}

// --- session and schema lifecycle ---------------------------------------------

RimeSessionId CreateSessionOrZero(RimeApi* api, const std::string& what) {
  const RimeSessionId sid = api->create_session();
  if (sid == kNoSession) {
    std::printf("[english] FAIL session.create (%s): create_session() returned the "
                "invalid id\n",
                what.c_str());
    return kNoSession;
  }
  std::printf("[english] session.%-24s id=%llu\n", what.c_str(),
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

ExitCode SelectAndConfirmSchema(RimeApi* api, RimeSessionId sid,
                                ApiBalance* balance, Checker* checks,
                                const std::string& label, const char* schema_id) {
  const int failures_before = checks->failed();
  if (!RIME_API_AVAILABLE(api, select_schema)) {
    Fail(kSchemaSelectionFailed, "schema.select", "no select_schema slot");
    return kSchemaSelectionFailed;
  }
  const Bool selected = api->select_schema(sid, schema_id);
  std::printf("[english] select_schema.%-21s -> %d (recorded, not used as a "
              "judgement)\n",
              label.c_str(), static_cast<int>(selected));
  StateSnapshot s;
  ExitCode failure = kOk;
  if (!ReadState(api, sid, balance, &s, &failure)) {
    return failure;
  }
  checks->CheckStr(s.schema_id, schema_id,
                   label + ": get_status().schema_id readback");
  if (checks->failed() > failures_before) {
    return kSchemaSelectionFailed;
  }
  return kOk;
}

ExitCode DeployFixture(Harness* h) {
  const int failures_before = h->checks.failed();
  if (!RIME_API_AVAILABLE(h->api, deploy_config_file) ||
      !RIME_API_AVAILABLE(h->api, deploy_schema)) {
    Fail(kDeployFailed, "deploy.slots",
         "the loaded API table has no deploy_config_file/deploy_schema slot");
    return kDeployFailed;
  }
  // Both slots run their deployment task synchronously in this process
  // (src/rime/deployer.cc:28-40): no maintenance thread, no sleep, no polling.
  const Bool default_ok =
      h->api->deploy_config_file(kFixtureDefaultYaml, "config_version");
  const std::string schema_src = h->schema_source.string();
  const Bool schema_ok = h->api->deploy_schema(schema_src.c_str());
  std::printf("[english] deploy default.yaml        -> %d\n",
              static_cast<int>(default_ok));
  std::printf("[english] deploy test schema         -> %d  (%s)\n",
              static_cast<int>(schema_ok), schema_src.c_str());

  std::error_code ec;
  const std::filesystem::path compiled_default =
      std::filesystem::path(h->staging_dir) / kFixtureDefaultYaml;
  const std::filesystem::path compiled_schema =
      std::filesystem::path(h->staging_dir) / kCompiledSchemaYaml;
  const bool default_written =
      std::filesystem::is_regular_file(compiled_default, ec) && !ec;
  const bool schema_written =
      std::filesystem::is_regular_file(compiled_schema, ec) && !ec;
  std::printf("[english] staging                      %s\n", h->staging_dir.c_str());
  std::printf("[english]   compiled default.yaml      %s\n",
              default_written ? "present" : "absent");
  std::printf("[english]   compiled test schema       %s\n",
              schema_written ? "present" : "absent");

  h->checks.Check(default_ok == True, "deploy_config_file return value",
                  std::to_string(static_cast<int>(default_ok)));
  h->checks.Check(schema_ok == True, "deploy_schema return value",
                  std::to_string(static_cast<int>(schema_ok)));
  h->checks.Check(default_written, "compiled default.yaml exists",
                  default_written ? "present" : "absent");
  h->checks.Check(schema_written, "compiled test schema exists",
                  schema_written ? "present" : "absent");
  // Only this function's own assertions decide its return code; a failure recorded
  // elsewhere must not be reported as a deployment failure.
  if (h->checks.failed() > failures_before) {
    return kDeployFailed;
  }
  return kOk;
}

// --- whole-program lifecycle ---------------------------------------------------
ExitCode RunHarness(const Options& opt, ModeFn mode_fn) {
  std::printf("[english] mode                       %s\n", ModeName(opt.mode));
  std::printf("[english] module name                %s\n",
              EPOCANVA_ENGLISH_MODULE_NAME);
  std::printf("[english] component name             %s\n",
              EPOCANVA_ENGLISH_TRANSLATOR_NAME);
  std::printf("[english] units                      keysyms are 32-bit X11 "
              "identifiers; mask is librime's modifier bits "
              "(src/rime/key_table.h:14-45); preedit length is a byte (UTF-8 code "
              "unit) offset; candidate index is a zero-based menu position; every "
              "counter is an event count\n");

  Harness h;
  std::filesystem::path user_dir, log_dir;
  ExitCode rc = PrepareRunRoot(opt, &h.shared_dir, &user_dir, &log_dir);
  if (rc != kOk) {
    return rc;
  }
  rc = StageFixture(opt, h.shared_dir, &h.schema_source);
  if (rc != kOk) {
    if (rc == kFixtureMissing) {
      Fail(rc, "fixture.precondition",
           "required fixture files are missing from --fixture-dir");
    }
    return rc;
  }

  RimeApi* api = rime_get_api();
  if (api == nullptr) {
    Fail(kApiUnavailable, "api.acquire", "rime_get_api() returned NULL");
    return kApiUnavailable;
  }
  h.api = api;
  Log("api.acquire", "rime_get_api() returned a table");

  rc = VerifyLoadedModule(opt, api);
  if (rc != kOk) {
    return rc;
  }

  // Read BEFORE any engine call. RIME_REGISTER_CUSTOM_MODULE's .CRT$XCU initializer
  // runs when rime.dll is loaded (src/rime_api.h:528-533), so a compiled-in module is
  // findable here while its initialize() has not run yet.
  const EnglishSurface before_setup = AcquireEnglishSurface(api);
  std::printf("[english] module.before_setup        %s\n",
              before_setup.module_found ? "registered by DLL load" : "absent");

  // path::string() is the ANSI view of the path; ParseArgs has already refused
  // anything outside printable ASCII, so narrow and wide views here are identical.
  const std::string shared_s = h.shared_dir.string();
  const std::string user_s = user_dir.string();
  const std::string log_s = log_dir.string();

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared_s.c_str();
  traits.user_data_dir = user_s.c_str();
  traits.log_dir = log_s.c_str();
  traits.prebuilt_data_dir = nullptr;  // -> <shared>/build (src/rime/setup.cc:58-61)
  traits.staging_dir = nullptr;        // -> <user>/build  (src/rime/setup.cc:62-65)
  traits.app_name = "rime.epocanva.english_recovery";
  traits.distribution_name = "EpoCanva";
  traits.distribution_code_name = "epocanva";
  traits.distribution_version = "0.0.0-p2.6-recovery";
  traits.min_log_level = 3;  // FATAL; inert while logging is OFF
  // "default" = core + dict + gears, "levers" adds the deployment tasks
  // (src/rime/setup.cc:30, src/rime/lever/levers_module.cc:20-31). EpoCanva's own
  // module is named in EVERY mode, including the baseline run: an unknown name is
  // skipped silently (src/rime/setup.cc:33-40), which is exactly why the baseline
  // case needs a behavioural assertion and not a return code.
  static const char* kModules[] = {"default", "levers", EPOCANVA_ENGLISH_MODULE_NAME,
                                   nullptr};
  traits.modules = kModules;

  bool initialize_called = false;
  api->setup(&traits);
  Log("engine.setup", "setup() returned");
  api->initialize(&traits);
  initialize_called = true;
  Log("engine.initialize", "initialize() returned");

  if (rc == kOk) {
    if (!RIME_API_AVAILABLE(api, get_shared_data_dir_s) ||
        !RIME_API_AVAILABLE(api, get_user_data_dir_s) ||
        !RIME_API_AVAILABLE(api, get_staging_dir_s)) {
      Fail(kTraitsNotApplied, "traits.readback",
           "the loaded API table has none of the directory read-back slots this run "
           "needs to prove where the engine actually looks");
      rc = kTraitsNotApplied;
    }
  }

  if (rc == kOk) {
    std::string read_shared, read_user;
    rc = ReadDirSlot(api->get_shared_data_dir_s, "shared_data_dir", &read_shared);
    if (rc == kOk && read_shared != shared_s) {
      Fail(kTraitsNotApplied, "traits.readback",
           "the engine's shared_data_dir is not the one this run asked for: " +
               read_shared);
      rc = kTraitsNotApplied;
    }
    if (rc == kOk) {
      rc = ReadDirSlot(api->get_user_data_dir_s, "user_data_dir", &read_user);
    }
    if (rc == kOk && read_user != user_s) {
      Fail(kTraitsNotApplied, "traits.readback",
           "the engine's user_data_dir is not the one this run asked for: " +
               read_user);
      rc = kTraitsNotApplied;
    }
    // The relative lexicon_path in the schema resolves against the directory the
    // engine actually uses, so that directory -- not the one this program passed --
    // is what has to be proven.
    if (rc == kOk) {
      rc = ReadDirSlot(api->get_staging_dir_s, "staging_dir", &h.staging_dir);
    }
  }

  if (rc == kOk) {
    h.surface = AcquireEnglishSurface(api);
    std::printf("[english] module.after_init          %s  diagnostics=%s\n",
                h.surface.module_found ? "found" : "absent",
                h.surface.api_available ? "available" : "unavailable");
    if (h.surface.api_available) {
      std::printf("[english] build_id                   %s\n",
                  h.surface.api->build_id());
    }
    rc = mode_fn(h);
  }

  if (initialize_called) {
    api->finalize();
    Log("cleanup.finalize", "finalize() returned");
    // Deliberately nothing is read from the English surface after this point:
    // finalize() runs ModuleManager::UnloadModules() and Registry::Clear()
    // (src/rime_api_impl.h:58-63).
    Log("post.finalize", "the english surface is not touched after finalize()");
    if (rc == kOk) {
      const RimeSessionId late = api->create_session();
      if (late != kNoSession) {
        api->destroy_session(late);
        h.checks.Check(false, "create_session() after finalize()", "succeeded");
      } else {
        Log("post.finalize", "create_session() refused; the service is down");
      }
    }
  } else if (rc == kOk) {
    rc = kApiUnavailable;
    Fail(rc, "cleanup", "initialize() was never called");
  }

  if (!h.balance.balanced()) {
    std::printf("[english] FAIL api.balance: %d result(s) obtained and not freed\n",
                h.balance.outstanding());
    if (rc == kOk) {
      rc = kApiResultUnbalanced;
    }
  } else {
    std::printf("[english] api.balance                status %d/%d context %d/%d "
                "commit %d/%d -- paired\n",
                h.balance.status_get, h.balance.status_free, h.balance.context_get,
                h.balance.context_free, h.balance.commit_get, h.balance.commit_free);
  }

  {
    const int files = CountFilesBeneath(log_dir);
    // ENABLE_LOGGING=OFF compiles SetupLogging()'s body away (src/rime/setup.cc:71-95)
    // and LOG()/DLOG() expand to no-ops (src/rime/no_logging.h). Nothing may land here.
    if (files != 0) {
      std::printf("[english] FAIL post.finalize.log: %d file(s) under the log "
                  "directory\n",
                  files);
      if (rc == kOk) {
        rc = kLogFilesWritten;
      }
    } else {
      Log("post.finalize", "log directory stayed empty");
    }
  }

  std::printf("[english] assertions               %d checked, %d failed\n",
              h.checks.total(), h.checks.failed());
  if (rc == kOk && h.checks.failed() > 0) {
    rc = h.assertion_failure_code;
  }
  // The marker is one contiguous literal, "RESULT PASS (<mode> mode)", because the
  // CTest wrapper searches for it with a literal substring find: a padded two-column
  // log line would look identical to a human and be invisible to the judgement.
  if (rc == kOk) {
    std::printf("[english] RESULT PASS (%s mode)\n", ModeName(opt.mode));
  } else {
    std::printf("[english] RESULT FAIL (%s mode, stage code %d, %d failed "
                "assertion(s))\n",
                ModeName(opt.mode), static_cast<int>(rc), h.checks.failed());
  }
  return rc;
}

}  // namespace recovery
}  // namespace epocanva
