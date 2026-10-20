// EpoCanva P2.6-recovery -- shared harness for the rebuilt English integration tests.
//
// WHY THIS FILE EXISTS. The original tools/rime_english_completion/main.cc held the
// harness and every mode's expectations in one ~5,100 line translation unit, and that
// file is gone. This is the harness half of its replacement: engine lifecycle, fixture
// staging, observation of the public C API, resource accounting and reporting. It
// contains no linguistic expectation -- those live in completion_tests.cc.
//
// WHAT IT OBSERVES. Every asserted preedit, candidate, comment, commit string and
// handled flag is read back from RimeApi. Nothing is synthesised, replayed or
// inferred from the source under test, and an API failure returns a stage code instead
// of continuing with an empty value (AGENTS.md section 3).
//
// UNITS (AGENTS.md section 5). Strings crossing the C boundary are UTF-8 code units
// (bytes) and every buffer_size counts bytes. RimeComposition.length/.cursor_pos are
// BYTE offsets into `preedit`. Keycodes are X11 keysyms (32-bit identifiers), `mask` is
// librime's own modifier bit set (src/rime/key_table.h:14-45). Every counter in
// EpoCanvaEnglishCounters is an event count, not an offset or a length.
//
// OWNERSHIP. get_status/get_context/get_commit allocate inside rime.dll and the
// matching free_* releases them (src/rime_api_impl.h:153-155, 220-224); get_input has
// no free slot, so it is copied and never released. ApiBalance records the pairing and
// RunHarness refuses a run whose results were not all freed. After finalize() the
// English surface is deliberately not touched: finalize() runs
// ModuleManager::UnloadModules() and Registry::Clear() (src/rime_api_impl.h:58-63).
#ifndef EPOCANVA_RECOVERY_TEST_SUPPORT_H_
#define EPOCANVA_RECOVERY_TEST_SUPPORT_H_

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "english_diagnostics.h"
#include "rime_api.h"

namespace epocanva {
namespace recovery {

// Stage codes. 2..21 and 25 keep the meanings recorded in
// docs/testing/rime-english-completion.md section 10, so a code in a recovery report
// still reads the same as the pre-incident one.
enum ExitCode : int {
  kOk = 0,
  kUsage = 2,
  kApiUnavailable = 3,
  kLoadedModuleMismatch = 4,
  kFixtureMissing = 5,
  kRunDirectoryFailed = 6,
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
  kNegativeExpectationNotMet = 18,
  kProcessKeyIndeterminate = 19,
  kEnglishSurfaceUnavailable = 20,
  kCounterReadFailed = 21,
  kSelectCandidateRefused = 25,
};

// X11 keysyms, read out of the pinned checkout's vendored header rather than from
// memory: third_party/librime/include/X11/keysymdef.h -- XK_BackSpace 0xff08 (:124),
// XK_Return 0xff0d (:128), XK_Escape 0xff1b (:132). Printable ASCII keeps its code
// point, so a letter or a technical mark is typed as its own byte.
inline constexpr int kXkBackSpace = 0xff08;
inline constexpr int kXkReturn = 0xff0d;
inline constexpr int kXkEscape = 0xff1b;

// Row count of tests/fixtures/english_completion/lexicon.tsv, counted in the fixture
// itself. It is stated here rather than re-derived from the loader, so a component that
// silently drops a row cannot agree with its own test.
inline constexpr long long kExpectedLexiconRows = 54;

// Identity of the surviving hand-written completion fixture.
inline constexpr char kCompletionSchemaId[] = "epocanva_test_english";
inline constexpr char kExpectedBuildId[] = "epocanva-english/0.0.0-p2.6";

// src/rime_api_impl.h:153-155 treats session id 0 as unknown; rime_api.h names no
// invalid-session constant, so the harness defines the sentinel it compares against.
inline constexpr RimeSessionId kNoSession = 0;

// The modes this program implements. Adding a mode means adding a function in one of
// the mode files and one entry in main.cc's table; no mode is stubbed in advance.
enum class Mode {
  kCompletion,
  kExpectModuleAbsent,
  kSelfCheckFail,
  kCasePreservation,
  kCaseSelfCheckFail,
  kTokenProtection,
  kTokenSelfCheckFail,
  kSpaceCommit,
  kSpaceSelfCheckFail,
};

struct Options {
  std::wstring expect_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path run_root;
  Mode mode = Mode::kCompletion;
};

// Everything one key event makes observable, as returned by the API.
struct StateSnapshot {
  std::string schema_id;
  bool status_composing = false;
  bool status_disabled = false;
  bool has_preedit = false;
  std::string preedit;
  long preedit_length = -1;  // UTF-8 code units into `preedit`
  long num_candidates = -1;
  long page_size = -1;
  long highlighted = -1;
  std::vector<std::string> candidates;  // menu order, index 0 first
  std::vector<std::string> comments;    // one per candidate; absent comment is ""
  std::string raw_input;                // get_input(), never preedit
  // Context::GetCommitText() as the C API reports it. It moves for a HIGHLIGHT as well
  // as for a selection (Segment::GetSelectedCandidate() reads selected_index without
  // consulting Segment::status -- src/rime/segmentation.cc:51-53), so it is reported for
  // the record and never used as a judgement about what a space will commit.
  bool has_commit_text_preview = false;
  std::string commit_text_preview;
  bool commit_present = false;
  std::string commit_text;
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

// A failed check is recorded, reported inline and turns the run red at the end; the
// program never aborts on the first mismatch, because a report that names only the
// first failure is much less useful than one that names all of them.
class Checker {
 public:
  void Check(bool condition, const std::string& what, const std::string& got);
  void CheckStr(const std::string& got, const std::string& want,
                const std::string& what);
  void CheckNum(long long got, long long want, const std::string& what);
  // The whole menu, in menu order, as one pipe-joined string -- the form the fixture
  // documentation writes its expectations in.
  void CheckMenu(const StateSnapshot& state, const std::string& want_pipe_list,
                 const std::string& what);
  int total() const { return total_; }
  int failed() const { return failed_; }

 private:
  int total_ = 0;
  int failed_ = 0;
};

// Reachability of EpoCanva's own component inside the loaded engine. Reached through
// the module's get_api() slot, so no permanent EpoCanva ABI is created (docs/adr/0004).
struct EnglishSurface {
  bool module_found = false;
  bool api_available = false;
  RimeModule* module = nullptr;
  const EpoCanvaEnglishApi* api = nullptr;
};

// One injected key: the return value of process_key() and everything readable after it.
struct KeyEvent {
  bool handled = false;
  StateSnapshot state;
};

// Asserts everything one keystroke frame must show: that the key was handled, the raw
// composition as get_input() returned it, the preedit text and its length in UTF-8 code
// units, the whole candidate menu in menu order, an empty comment column, and candidate 0
// equal to the typed text. Expected menus are written the way the fixture documentation
// writes them -- pipe-joined, index 0 first.
//
// `want_page_size` is the schema's menu/page_size and is asserted per frame, because a
// frame whose page size reads 0 is an engine that never built the menu this run claims to
// have read.
void CheckFrame(Checker* checks, const KeyEvent& e, const std::string& want_raw,
                 const std::string& want_menu, long want_page_size,
                 const std::string& label);

// The engine, the fixture and the accounting a mode is allowed to touch.
struct Harness {  RimeApi* api = nullptr;
  EnglishSurface surface;
  std::filesystem::path schema_source;  // staged copy, in the shared dir
  std::filesystem::path shared_dir;
  std::string staging_dir;              // read back from the engine, not assumed
  ApiBalance balance;
  Checker checks;
  // Which code an unmet assertion becomes: a mode that asserts an ABSENCE reports 18,
  // so its failure means "the engine is more capable than this run claimed".
  ExitCode assertion_failure_code = kAssertionFailed;
};

using ModeFn = ExitCode (*)(Harness&);

// --- plumbing -----------------------------------------------------------------
void Log(const char* stage, const std::string& detail);
void Fail(ExitCode code, const char* stage, const std::string& detail);

// Names accepted on the command line. Returns false for anything else so an unknown
// mode can never reach a mode function.
bool ModeFromName(const std::string& name, Mode* out);
const char* ModeName(Mode mode);
bool ParseArgs(int argc, char** argv, Options* out, std::string* error);

std::string JoinMenu(const StateSnapshot& state);
std::string JoinComments(const StateSnapshot& state);
std::vector<std::string> SplitPipeList(const std::string& joined);
std::string DescribeCounters(const EpoCanvaEnglishCounters& c);
std::string DescribeDelta(const EpoCanvaEnglishCounters& before,
                          const EpoCanvaEnglishCounters& after);

// --- observation --------------------------------------------------------------
// false only when an API call itself failed; the caller turns that into a stage code.
bool ReadState(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
               StateSnapshot* out, ExitCode* failure);
std::string DescribeState(const StateSnapshot& s);
// Reads one pending commit. get_commit() consumes, so this exists both for the
// post-key read and for the "the same commit is not offered twice" assertion.
ExitCode ReadCommitOnce(RimeApi* api, RimeSessionId sid, ApiBalance* balance,
                        bool* present, std::string* text);

EnglishSurface AcquireEnglishSurface(RimeApi* api);
bool ReadCounters(const EnglishSurface& surface, EpoCanvaEnglishCounters* out);
std::string LastStatusName(const EnglishSurface& surface);
// Signed difference of ONE counter field between two snapshots. A negative delta is
// itself a finding -- it would mean the counters were reset mid-run -- so it must not be
// swallowed by unsigned arithmetic. `field` is a member of the C struct, named at the
// call site, which keeps this helper from encoding any opinion about which counters
// should move.
long long CounterDelta(const EpoCanvaEnglishCounters& before,
                       const EpoCanvaEnglishCounters& after,
                       unsigned long long EpoCanvaEnglishCounters::*field);
// The sum of every P2.6 spelling counter. Zero is how a mode proves the schema it
// deployed asked for no suggestions at all: "the menu looked right" cannot show that,
// because a suggestion and a completion are indistinguishable in the text column.
long long SpellingTotal(const EpoCanvaEnglishCounters& c);

// --- keys ---------------------------------------------------------------------
ExitCode SendKey(RimeApi* api, RimeSessionId sid, int keycode, int mask,
                 ApiBalance* balance, KeyEvent* out);
// Types each byte of `text` as its own X11 keysym (printable ASCII keeps its code
// point) and appends one event per byte.
ExitCode TypeText(RimeApi* api, RimeSessionId sid, const std::string& text,
                  ApiBalance* balance, std::vector<KeyEvent>* events);
ExitCode SelectCandidate(RimeApi* api, RimeSessionId sid, std::size_t index,
                         bool* accepted);
// Moves the menu highlight WITHOUT confirming a candidate: Context::Highlight() sets
// selected_index and leaves Segment::status alone (src/rime/context.cc:132-149), which is
// the difference a space has to respect. Returns False when the index did not change, so a
// caller that ignores the return value cannot tell "already there" from "worked".
ExitCode HighlightCandidate(RimeApi* api, RimeSessionId sid, std::size_t index,
                            bool* accepted);

// --- session and schema lifecycle ---------------------------------------------
RimeSessionId CreateSessionOrZero(RimeApi* api, const std::string& what);
ExitCode DestroySessionAndCheck(RimeApi* api, RimeSessionId sid, Checker* checks,
                                const std::string& label);
// Proves the selection by reading get_status().schema_id back; select_schema()'s own
// return value is printed but never used as a judgement (docs/testing section 6).
ExitCode SelectAndConfirmSchema(RimeApi* api, RimeSessionId sid,
                                ApiBalance* balance, Checker* checks,
                                const std::string& label, const char* schema_id);
// Deploys this run's staged default.yaml and schema through the public C API, then
// proves the compiled artefacts landed in the engine's own staging directory.
// deploy_config_file() returns True even when its source is missing
// (docs/testing/rime-input-replay.md section 7), so return values alone are never the
// judgement here.
ExitCode DeployFixture(Harness* h);

// --- whole-program lifecycle ---------------------------------------------------
// Parses, prepares the run directory, stages the fixture, starts the engine, runs the
// mode, finalizes, then checks resource balance, the empty log directory and the
// assertion count. main.cc calls this and nothing else.
ExitCode RunHarness(const Options& opt, ModeFn mode_fn);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_TEST_SUPPORT_H_
