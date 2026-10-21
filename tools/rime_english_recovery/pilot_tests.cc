// EpoCanva P2.4-recovery -- the pilot word-list trial. See pilot_tests.h for what is
// compared against what, how data identity is established, and which exit code means which
// kind of refusal.
#include "pilot_tests.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "english_diagnostics.h"
#include "pilot_reference.h"
#include "pilot_sha256.h"

#ifndef EPOCANVA_PILOT_EXPECTED_SHA256
// This build directory was configured without -DEPOCANVA_ENGLISH_PILOT_LEXICON, so it has
// no identity expectations compiled in. No pilot case is registered in such a build; the
// fallbacks below exist only so a hand-run reports itself instead of inventing a number.
#define EPOCANVA_PILOT_UNCONFIGURED 1
#define EPOCANVA_PILOT_EXPECTED_SHA256 ""
#define EPOCANVA_PILOT_EXPECTED_BYTES 0
#define EPOCANVA_PILOT_EXPECTED_ROWS 0
#define EPOCANVA_PILOT_SOURCE_PATH "(not configured)"
#define EPOCANVA_PILOT_PINNED_COMMIT "(not configured)"
#endif

namespace epocanva {
namespace recovery {
namespace {

constexpr std::size_t kTotalCandidates = 8;  // 1 original + at most 7 completions
constexpr int kXkSpace = 0x0020;

long long LL(unsigned long long v) { return static_cast<long long>(v); }

// Items in a '|'-joined menu: one more item than separators, and zero when empty.
std::size_t MenuItems(const std::string& menu) {
  if (menu.empty()) {
    return 0;
  }
  std::size_t bars = 0;
  for (const char c : menu) {
    if (c == '|') {
      ++bars;
    }
  }
  return bars + 1;
}

// --- data identity ---------------------------------------------------------------

struct Preflight {
  bool ok = false;
  std::string reason;   // one named condition; never a bare false
  std::string detail;
  std::string digest;
  long long bytes = -1;
  long long rows = -1;
  PilotReference reference;  // valid only when ok
};

Preflight VerifyPilotData(const std::filesystem::path& tsv) {
  Preflight out;
  std::error_code ec;
  if (!std::filesystem::exists(tsv, ec) || ec) {
    out.reason = "file-absent";
    out.detail = tsv.string();
    return out;
  }
  if (!std::filesystem::is_regular_file(tsv, ec) || ec) {
    out.reason = "not-a-regular-file";
    out.detail = tsv.string();
    return out;
  }
  out.bytes = static_cast<long long>(std::filesystem::file_size(tsv, ec));
  if (ec) {
    out.reason = "size-unreadable";
    out.detail = ec.message();
    return out;
  }
#ifdef EPOCANVA_PILOT_UNCONFIGURED
  out.reason = "identity-expectations-absent";
  out.detail = "this build directory was configured without the pilot option, so no "
               "manifest digest is compiled in to check against";
  return out;
#else
  if (out.bytes != static_cast<long long>(EPOCANVA_PILOT_EXPECTED_BYTES)) {
    out.reason = "size-mismatch";
    out.detail = std::to_string(out.bytes) + " bytes; the manifest expects " +
                 std::to_string(static_cast<long long>(EPOCANVA_PILOT_EXPECTED_BYTES));
    return out;
  }
  out.digest = Sha256HexOfFile(tsv);
  if (out.digest.empty()) {
    out.reason = "digest-unreadable";
    out.detail = tsv.string();
    return out;
  }
  if (out.digest != EPOCANVA_PILOT_EXPECTED_SHA256) {
    out.reason = "digest-mismatch";
    out.detail = "computed " + out.digest + "; the manifest expects " +
                 EPOCANVA_PILOT_EXPECTED_SHA256;
    return out;
  }
  std::string error;
  if (!out.reference.Load(tsv, &error)) {
    out.reason = "row-grammar";
    out.detail = error;
    return out;
  }
  out.rows = static_cast<long long>(out.reference.row_count());
  if (out.rows != static_cast<long long>(EPOCANVA_PILOT_EXPECTED_ROWS)) {
    out.reason = "row-count-mismatch";
    out.detail = std::to_string(out.rows) + " legal rows read; the manifest expects " +
                 std::to_string(static_cast<long long>(EPOCANVA_PILOT_EXPECTED_ROWS));
    return out;
  }
  out.ok = true;
  out.reason = "verified";
  return out;
#endif
}

void ReportPreflight(const Preflight& p, const std::filesystem::path& tsv) {
  std::printf("[english] pilot.data                %s\n", tsv.string().c_str());
  std::printf("[english] pilot.manifest            sha256 %s | %lld bytes | %lld rows | "
              "source %s | commit %s\n",
              EPOCANVA_PILOT_EXPECTED_SHA256,
              static_cast<long long>(EPOCANVA_PILOT_EXPECTED_BYTES),
              static_cast<long long>(EPOCANVA_PILOT_EXPECTED_ROWS),
              EPOCANVA_PILOT_SOURCE_PATH, EPOCANVA_PILOT_PINNED_COMMIT);
  std::printf("[english] pilot.identity            %s: %s\n",
              p.ok ? "VERIFIED" : "REJECTED", p.reason.c_str());
  if (!p.detail.empty()) {
    std::printf("[english] pilot.identity.detail     %s\n", p.detail.c_str());
  }
  if (p.bytes >= 0) {
    std::printf("[english] pilot.actual              %lld bytes | %lld rows | sha256 %s\n",
                p.bytes, p.rows, p.digest.c_str());
  }
}

// One row of the fixed query set. `class_name` says which capability the row exercises, and
// `requires_completion` marks the rows where the DATA must supply at least one completion:
// a completable prefix that finds none is reported as a data absence, not passed over.
struct QueryCase {
  const char* input;
  const char* class_name;
  bool requires_completion;
};

const QueryCase kQueries[] = {
    {"a", "single letter", true},
    {"z", "single letter, sparse tail", true},
    {"pr", "two letters, budget cut inside one weight tier", true},
    {"pro", "three letters, exact row plus completions", true},
    {"int", "three letters, no exact row", true},
    {"electro", "long prefix, several tiers", true},
    {"internation", "longer prefix, small set", true},
    {"hello", "exact row only: deduped to the original", false},
    {"world", "exact row plus a delimiter-bearing tail", true},
    {"zz", "no match", false},
    {"zqvkxbjw", "no match, random letters", false},
    {"don'", "trailing apostrophe prefix", true},
    {"can'", "trailing apostrophe prefix, other stem", true},
    {"won'", "trailing apostrophe prefix, third stem", true},
    {"co-", "trailing hyphen prefix", true},
    {"re-", "trailing hyphen prefix, other stem", true},
    {"Pro", "initial capital over a completable prefix", true},
    {"PRO", "uppercase over a completable prefix", true},
    {"Don'", "initial capital with a delimiter", true},
    {"CO-", "uppercase with a delimiter", true},
    {"heL", "mixed case: refused", false},
    {"co--operate", "two delimiters: refused as a word form", false},
    {"'hello", "leading delimiter: refused", false},
    {"user@example.com", "protected: e-mail", false},
    {"https://example.com/a?x=1&y=2#z", "protected: URL", false},
    {"C:\\tmp\\file.txt", "protected: Windows path", false},
    {"foo_bar", "protected: snake case", false},
    {"v1.2", "protected: version number", false},
};

// The per-run machinery every pilot mode shares: counted keys (so the space processor's own
// event count can be checked), the last event, and the timing samples.
class PilotRun {
 public:
  PilotRun(Harness* h, EpoCanvaEnglishCounters counters)
      : h_(h), c_(counters) {}

  Harness* harness() { return h_; }
  EpoCanvaEnglishCounters& counters() { return c_; }
  long long keys() const { return keys_; }

  ExitCode Press(RimeSessionId sid, int keysym, int mask) {
    ++keys_;
    const auto t0 = std::chrono::steady_clock::now();
    const ExitCode prc = SendKey(h_->api, sid, keysym, mask, &h_->balance, &ev_);
    const auto t1 = std::chrono::steady_clock::now();
    if (prc == kOk) {
      samples_.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 1000.0);
    }
    return prc;
  }

  const KeyEvent& last() const { return ev_; }

  ExitCode Type(RimeSessionId sid, const std::string& text,
                long long* letters) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const ExitCode prc =
          Press(sid, static_cast<int>(static_cast<unsigned char>(text[i])), 0);
      if (prc != kOk) {
        return prc;
      }
      ++letters_local_;
      if (letters != nullptr) {
        ++*letters;
      }
      const std::string raw = text.substr(0, i + 1);
      h_->checks.CheckStr(ev_.state.raw_input, raw,
                          std::string("get_input() after '") + raw + "'");
    }
    return kOk;
  }

  long long letters() const { return letters_local_; }

  void ReportTiming() {
    if (samples_.empty()) {
      return;
    }
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (const double v : sorted) {
      total += v;
    }
    // Descriptive only: one host, one run, process_key() plus the state read-back. Not a
    // benchmark, not comparable, and judged by no assertion.
    std::printf("[english] pilot.timing(descriptive)  keys=%zu min=%.1fus med=%.1fus "
                "p95=%.1fus max=%.1fus mean=%.1fus -- single Windows x64 host, not a "
                "benchmark\n",
                sorted.size(), sorted.front(), sorted[sorted.size() / 2],
                sorted[static_cast<std::size_t>(
                    0.95 * static_cast<double>(sorted.size() - 1))],
                sorted.back(), total / static_cast<double>(sorted.size()));
  }

 private:
  Harness* h_ = nullptr;
  EpoCanvaEnglishCounters c_;
  KeyEvent ev_;
  long long keys_ = 0;
  long long letters_local_ = 0;
  std::vector<double> samples_;
};

// Reads one pending commit and proves the queue is then empty.
ExitCode QueueEmpty(Harness* h, RimeSessionId sid, const std::string& label) {
  bool present = false;
  std::string text;
  const ExitCode prc = ReadCommitOnce(h->api, sid, &h->balance, &present, &text);
  h->checks.Check(!present, label + ": the commit queue is empty again",
                  present ? text : std::string("absent"));
  return prc;
}

ExitCode EscapeAndCheck(PilotRun* run, RimeSessionId sid) {
  Harness* h = run->harness();
  const ExitCode prc = run->Press(sid, kXkEscape, 0);
  if (prc != kOk) {
    return prc;
  }
  h->checks.Check(!run->last().state.status_composing, "Escape left the session idle",
                  "still composing");
  h->checks.Check(!run->last().state.commit_present, "Escape committed nothing",
                  run->last().state.commit_present
                      ? run->last().state.commit_text
                      : std::string("absent"));
  return kOk;
}

}  // namespace

ExitCode RunPilotLexiconMode(Harness& h) {
  // 1. The component must be in the engine before any word-list question is asked. This is
  //    the difference between code 20 (wrong engine) and code 22 (wrong data).
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "pilot.surface",
         "the loaded DLL exposes no epocanva_english module: a word-list question cannot "
         "be asked of an engine without the component");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  PilotRun run(&h, EpoCanvaEnglishCounters());
  if (!ReadCounters(h.surface, &run.counters())) {
    Fail(kCounterReadFailed, "pilot.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(run.counters().lexicon_load_attempts), 0,
                    "the engine has read no word list before the identity check");

  // 2. Identity first -- the gate a row count or a byte size cannot provide.
  Preflight pre = VerifyPilotData(h.pilot_lexicon);
  ReportPreflight(pre, h.pilot_lexicon);
  if (!pre.ok) {
    Fail(kPilotDataRejected, "pilot.preflight", pre.reason + ": " + pre.detail);
    return kPilotDataRejected;
  }
  const PilotReference& ref = pre.reference;

  // 3. The bytes the engine will resolve are the bytes that were just verified.
  const std::filesystem::path staged = h.shared_dir / kPilotStagedLexiconName;
  h.checks.CheckStr(Sha256HexOfFile(staged), EPOCANVA_PILOT_EXPECTED_SHA256,
                    "the staged copy the engine resolves carries the verified digest");
  std::printf("[english] pilot.staged               %s\n", staged.string().c_str());

  // 4. Deploy, open a session, and prove which schema and which list the engine used.
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "PILOT-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  const RimeSessionId b = CreateSessionOrZero(h.api, "PILOT-B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "PILOT-A",
                                       kPilotSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  if (!ReadCounters(h.surface, &run.counters())) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] pilot.engine.load          %s\n",
              DescribeCounters(run.counters()).c_str());
  h.checks.CheckNum(LL(run.counters().lexicon_load_attempts), 1,
                    "exactly one load for the whole session");
  h.checks.CheckNum(LL(run.counters().lexicon_load_successes), 1, "and it succeeded");
  h.checks.CheckNum(LL(run.counters().lexicon_load_failures), 0,
                    "with no failure recorded");
  h.checks.CheckNum(LL(run.counters().loaded_entry_count), pre.rows,
                    "the engine's index holds exactly the rows this file verified");
  h.checks.CheckNum(LL(run.counters().last_load_error_line), 0, "no load error line");
  h.checks.CheckStr(LastStatusName(h.surface), "ok", "word list status name");
  h.checks.CheckNum(LL(run.counters().config_missing_path), 0,
                    "the pilot schema configured a word-list path");

  // 5. The fixed query set, each answer compared with the independent reference.
  for (const QueryCase& q : kQueries) {
    const std::string input = q.input;
    const std::string want = ref.Menu(input, kTotalCandidates);
    const std::size_t want_items = MenuItems(want);
    if (q.requires_completion && want_items < 2) {
      // Named explicitly so an absent row can never read as a pass.
      h.checks.Check(false,
                     std::string("DATA ABSENCE, not an algorithm result: '") + input +
                         "' (" + q.class_name + ") must complete on this list",
                     "the reference found no row behind the prefix");
    }
    long long letters_before = run.letters();
    rc = run.Type(a, input, nullptr);
    if (rc != kOk) {
      break;
    }
    std::printf("[english] pilot.query              %-28s (%s) n=%zu %s\n",
                input.c_str(), q.class_name, want_items, want.c_str());
    CheckFrame(&h.checks, run.last(), input, want, 8,
               std::string(q.class_name) + " '" + input + "'");
    h.checks.CheckNum(run.last().state.num_candidates,
                      static_cast<long long>(want_items),
                      std::string(q.class_name) + " '" + input +
                          "': engine count == reference count");
    h.checks.CheckNum(run.last().state.page_size, 8,
                      "the fixture's menu/page_size is in effect");
    h.checks.CheckStr(JoinComments(run.last().state),
                      want_items == 0 ? std::string()
                                      : std::string(want_items - 1, '|'),
                      std::string(q.class_name) + " '" + input +
                          "': an empty comment column (this schema enables no "
                          "suggestions)");
    (void)letters_before;
    rc = EscapeAndCheck(&run, a);
    if (rc != kOk) {
      break;
    }
  }

  // 6. Commits: the default keeps the typed text; a selection commits the completion the
  //    reference ranked first -- taken from the reference, not typed in here.
  const std::vector<std::string> pro = ref.Candidates("pro", kTotalCandidates);
  if (rc == kOk) {
    h.checks.Check(pro.size() > 1,
                   "DATA ABSENCE: 'pro' has no completion to select on this list",
                   RefJoinMenu(pro));
    rc = run.Type(a, "pro", nullptr);
  }
  if (rc == kOk) {
    rc = run.Press(a, kXkReturn, 0);
  }
  if (rc == kOk) {
    h.checks.CheckStr(run.last().state.commit_text, "pro",
                      "default Return commits the typed text, not the completion");
    rc = QueueEmpty(&h, a, "default commit");
  }
  if (rc == kOk) {
    rc = run.Type(a, "pro", nullptr);
  }
  if (rc == kOk) {
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    h.checks.Check(accepted, "select_candidate(1) accepted on real data",
                   accepted ? "True" : "False");
  }
  if (rc == kOk) {
    rc = QueueEmpty(&h, a, "after select, before the commit key");
  }
  if (rc == kOk) {
    rc = run.Press(a, kXkReturn, 0);
  }
  if (rc == kOk) {
    h.checks.CheckStr(run.last().state.commit_text, pro.size() > 1 ? pro[1] : "",
                      "the selected completion commits in full");
    h.checks.Check(run.last().state.commit_text != "pro",
                   "and it is not the prefix again", run.last().state.commit_text);
    rc = QueueEmpty(&h, a, "selected commit");
  }

  // 7. The space bar on real data: the word once, then one space -- for both cases.
  if (rc == kOk) {
    rc = run.Type(a, "co-", nullptr);
  }
  if (rc == kOk) {
    rc = run.Press(a, kXkSpace, 0);
  }
  if (rc == kOk) {
    h.checks.CheckStr(run.last().state.commit_text, "co- ",
                      "case A: the partial token and exactly one space");
    h.checks.Check(run.last().state.commit_text.find("co-o") == std::string::npos,
                   "the space did not accept a completion",
                   run.last().state.commit_text);
    rc = QueueEmpty(&h, a, "case A space");
  }
  const std::vector<std::string> co = ref.Candidates("co-", kTotalCandidates);
  if (rc == kOk) {
    rc = run.Type(a, "co-", nullptr);
  }
  if (rc == kOk) {
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    h.checks.Check(accepted, "'co-': index 1 exists on real data",
                   accepted ? "True" : "False");
  }
  if (rc == kOk) {
    rc = run.Press(a, kXkSpace, 0);
  }
  if (rc == kOk) {
    h.checks.CheckStr(run.last().state.commit_text,
                      co.size() > 1 ? co[1] + " " : std::string(" "),
                      "case B: the selected word once, then one space");
    rc = QueueEmpty(&h, a, "case B space");
  }

  // 8. Case mapping must not regress at scale: the uppercase answer differs from the
  //    lowercase one and both are the reference's.
  if (rc == kOk) {
    const std::string lower = ref.Menu("pro", kTotalCandidates);
    const std::string upper = ref.Menu("PRO", kTotalCandidates);
    h.checks.Check(upper != lower,
                   "the reference renders uppercase differently from lowercase", upper);
    rc = run.Type(a, "PRO", nullptr);
  }
  if (rc == kOk) {
    CheckFrame(&h.checks, run.last(), "PRO", ref.Menu("PRO", kTotalCandidates), 8,
               "uppercase at scale");
    rc = EscapeAndCheck(&run, a);
  }

  // 9. Protected text commits whole, and the next ordinary word completes again.
  if (rc == kOk) {
    rc = run.Type(a, "user@example.com", nullptr);
  }
  if (rc == kOk) {
    h.checks.Check(!run.last().state.commit_present,
                   "typing an address committed nothing so far",
                   run.last().state.commit_present ? run.last().state.commit_text
                                                   : std::string("absent"));
    rc = run.Press(a, kXkSpace, 0);
  }
  if (rc == kOk) {
    h.checks.CheckStr(run.last().state.commit_text, "user@example.com ",
                      "protected text + space is the whole token and one space");
    rc = QueueEmpty(&h, a, "protected space");
  }
  if (rc == kOk) {
    rc = run.Type(a, "pro", nullptr);
  }
  if (rc == kOk) {
    CheckFrame(&h.checks, run.last(), "pro", ref.Menu("pro", kTotalCandidates), 8,
               "completions recovered after protected text");
    rc = EscapeAndCheck(&run, a);
  }

  // 10. Two sessions, one mid-completion and one with nothing to complete.
  if (rc == kOk) {
    rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "PILOT-B",
                                kPilotSchemaId);
  }
  if (rc == kOk) {
    rc = run.Type(a, "co-", nullptr);
  }
  if (rc == kOk) {
    rc = run.Type(b, "zz", nullptr);
  }
  if (rc == kOk) {
    StateSnapshot sa, sb;
    ExitCode failure = kOk;
    if (!ReadState(h.api, a, &h.balance, &sa, &failure) ||
        !ReadState(h.api, b, &h.balance, &sb, &failure)) {
      rc = failure;
    } else {
      h.checks.CheckMenu(sa, ref.Menu("co-", kTotalCandidates),
                         "A keeps its own menu while B composes");
      h.checks.CheckMenu(sb, ref.Menu("zz", kTotalCandidates),
                         "B has its own answer, with no cross-talk");
      h.checks.CheckStr(sa.raw_input, "co-", "A's raw composition is its own");
      h.checks.CheckStr(sb.raw_input, "zz", "B's raw composition is its own");
      rc = EscapeAndCheck(&run, a);
      if (rc == kOk) {
        rc = EscapeAndCheck(&run, b);
      }
    }
  }

  // 11. One load for the whole trial, and the identities that must close either way.
  if (rc == kOk && !ReadCounters(h.surface, &run.counters())) {
    rc = kCounterReadFailed;
  }
  if (rc == kOk) {
    const EpoCanvaEnglishCounters& c = run.counters();
    std::printf("[english] pilot.final.counters     %s\n", DescribeCounters(c).c_str());
    h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                      "hundreds of keys and still one word-list read: no per-key reload");
    h.checks.CheckNum(LL(c.loaded_entry_count), pre.rows, "and the same index throughout");
    h.checks.Check(c.queries >= static_cast<unsigned long long>(run.letters()),
                   "every injected letter produced a query", std::to_string(c.queries));
    h.checks.CheckNum(LL(c.original_candidates), LL(c.queries),
                      "every query offered the typed text");
    h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                      "every query was about the whole composition");
    h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                      "no token reached the translator cut in half");
    h.checks.CheckNum(
        LL(c.queries_rejected),
        LL(c.queries_mixed_case) + LL(c.queries_too_long) +
            LL(c.queries_not_alphabetic) + LL(c.queries_illegal_word_form),
        "rejected == mixed + too_long + not_alphabetic + illegal_word_form");
    h.checks.CheckNum(LL(c.queries_not_alphabetic),
                      LL(c.queries_non_ascii) + LL(c.queries_protected) +
                          LL(c.queries_unsupported_byte),
                      "not_alphabetic == non_ascii + protected + unsupported_byte");
    h.checks.CheckNum(LL(c.queries_non_ascii), 0,
                      "every byte typed here is ASCII: this is no evidence about "
                      "non-ASCII text");
    h.checks.CheckNum(LL(c.completion_candidates),
                      LL(c.completions_lowercase) +
                          LL(c.completions_initial_capital) +
                          LL(c.completions_uppercase),
                      "completions == lowercase + initial-capital + uppercase");
    h.checks.Check(c.completions_initial_capital >= 1 && c.completions_uppercase >= 1 &&
                       c.completions_lowercase >= 1,
                   "all three case shapes produced completions on the real list",
                   std::to_string(c.completions_lowercase) + "/" +
                       std::to_string(c.completions_initial_capital) + "/" +
                       std::to_string(c.completions_uppercase));
    h.checks.Check(c.completions_with_delimiter >= 1 &&
                       c.completions_with_delimiter <= c.completion_candidates,
                   "the delimiter rows really came out of the index",
                   std::to_string(c.completions_with_delimiter));
    h.checks.CheckNum(SpellingTotal(c), 0,
                      "this schema names no suggestion feature, so none ran");
    h.checks.CheckNum(LL(c.space_characters_emitted),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection) +
                          LL(c.space_case_c_empty),
                      "spaces emitted == a + b + c");
    h.checks.CheckNum(LL(c.composition_commits_issued),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection),
                      "composition commits issued == a + b");
    h.checks.CheckNum(LL(c.space_events_seen),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection) +
                          LL(c.space_case_c_empty) + LL(c.space_events_refused),
                      "space events seen == a + b + c + refused");
    // Every key this run injected reached the space processor exactly once: it is first in
    // engine/processors and releases everything that is not a plain space.
    h.checks.CheckNum(LL(c.processor_key_events), run.keys(),
                      "processor_key_events == the keys this run injected");
    h.checks.CheckNum(LL(c.module_finalized), 0, "module not finalized during the run");
    run.ReportTiming();
  }

  if (rc == kOk) {
    rc = DestroySessionAndCheck(h.api, b, &h.checks, "PILOT session B");
  }
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "PILOT session A");
  if (done_a != kOk && rc == kOk) {
    rc = done_a;
  }
  if (!ReadCounters(h.surface, &run.counters())) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(run.counters().translator_live), 0,
                    "no translator instance outlived its session");
  h.checks.CheckNum(LL(run.counters().translator_constructed),
                    LL(run.counters().translator_destroyed),
                    "translator constructed == destroyed");
  h.checks.CheckNum(LL(run.counters().space_processor_live), 0,
                    "no space processor outlived its session");
  h.checks.CheckNum(LL(run.counters().space_processor_constructed),
                    LL(run.counters().space_processor_destroyed),
                    "space processor constructed == destroyed");
  return rc;
}

ExitCode RunPilotTruncatedRejectedMode(Harness& h) {
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "pilot.truncated.surface",
         "the loaded DLL exposes no epocanva_english module");
    return kEnglishSurfaceUnavailable;
  }
  // Exit 22 is this case's DESIGNED outcome, so the exit code alone cannot show the case
  // worked. Everything it claims is watched instead: if one of those checks failed, the
  // refusal did not happen as claimed and the run reports 18, which is a different code from
  // both 22 (refused, as designed) and 14 (an engine expectation disagreed).
  const int claims_before = h.checks.failed();
  EpoCanvaEnglishCounters before;
  if (!ReadCounters(h.surface, &before)) {
    return kCounterReadFailed;
  }
  // Build an INDEPENDENT damaged copy inside this run's own directory: the first three
  // quarters of the real list, byte for byte. It parses cleanly, is strictly ascending and
  // has no duplicate -- so only an identity check can refuse it. The original is never
  // opened for writing.
  const std::filesystem::path copy = h.run_root / "pilot_truncated.tsv";
  std::error_code ec;
  {
    std::ifstream in(h.pilot_lexicon, std::ios::binary);
    std::ofstream out(copy, std::ios::binary);
    if (!in || !out) {
      Fail(kFixtureMissing, "pilot.truncated.copy",
           "cannot build the truncated copy inside this run's own directory: " +
               h.pilot_lexicon.string());
      return kFixtureMissing;
    }
    const long long total =
        static_cast<long long>(std::filesystem::file_size(h.pilot_lexicon, ec));
    std::vector<char> buf(static_cast<std::size_t>(total / 4 * 3));
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.write(buf.data(), in.gcount());
    std::printf("[english] pilot.truncated          %s (%lld of %lld bytes)\n",
                copy.string().c_str(), static_cast<long long>(in.gcount()), total);
  }
  const Preflight pre = VerifyPilotData(copy);
  ReportPreflight(pre, copy);
  h.checks.Check(!pre.ok,
                 "a truncated pilot list must be refused before the engine reads it",
                 pre.ok ? "ACCEPTED -- this is the failure this case exists to catch"
                        : pre.reason);
  h.checks.Check(pre.reason == "size-mismatch" || pre.reason == "digest-mismatch" ||
                     pre.reason == "row-count-mismatch",
                 "and refused for an identity reason, named", pre.reason + ": " + pre.detail);
  // The kind of refusal matters: this is pre-validation, so the engine never opened a word
  // list at all -- which is what separates code 22 from a load failure.
  EpoCanvaEnglishCounters after;
  if (!ReadCounters(h.surface, &after)) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(after.lexicon_load_attempts), 0,
                    "the engine attempted no load: this is not a load failure");
  // And the untouched original still verifies, which is how the run shows it did not
  // repair or replace the data it was handed.
  const Preflight again = VerifyPilotData(h.pilot_lexicon);
  std::printf("[english] pilot.original.check       %s\n",
              again.ok ? "still verified" : "CHANGED");
  h.checks.Check(again.ok, "the original pilot file is untouched", again.reason);
  std::filesystem::remove(copy, ec);
  if (h.checks.failed() > claims_before) {
    Fail(kNegativeExpectationNotMet, "pilot.truncated",
         "the truncation refusal this case exists to prove did not happen; see the named "
         "failures above");
    return kNegativeExpectationNotMet;
  }
  Fail(kPilotDataRejected, "pilot.truncated",
       "refusing the truncated copy as designed: " + pre.reason);
  return kPilotDataRejected;
}

ExitCode RunPilotMissingRejectedMode(Harness& h) {
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "pilot.missing.surface",
         "the loaded DLL exposes no epocanva_english module");
    return kEnglishSurfaceUnavailable;
  }
  // Same reasoning as the truncated case: 22 is the designed result, so a case that failed to
  // refuse must leave a different code behind.
  const int claims_before = h.checks.failed();
  EpoCanvaEnglishCounters before;
  if (!ReadCounters(h.surface, &before)) {
    return kCounterReadFailed;
  }
  // The path CTest handed over does not exist. The mode must NAME that condition and stop
  // before the engine is asked to read anything, rather than deploy a schema and let the
  // loader produce a file-not-found that would then be read as if this were a load test.
  const Preflight pre = VerifyPilotData(h.pilot_lexicon);
  ReportPreflight(pre, h.pilot_lexicon);
  h.checks.Check(!pre.ok, "an absent pilot file must be refused",
                 pre.ok ? "ACCEPTED" : pre.reason);
  h.checks.CheckStr(pre.reason, "file-absent",
                    "and refused for the absent-file reason specifically");
  EpoCanvaEnglishCounters after;
  if (!ReadCounters(h.surface, &after)) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(after.lexicon_load_attempts), 0,
                    "pre-validation stopped it: the engine attempted no load");
  if (h.checks.failed() > claims_before) {
    Fail(kNegativeExpectationNotMet, "pilot.missing",
         "the absent-file refusal this case exists to prove did not happen; see the named "
         "failures above");
    return kNegativeExpectationNotMet;
  }
  Fail(kPilotDataRejected, "pilot.missing",
       "refusing the absent pilot file as designed");
  return kPilotDataRejected;
}

ExitCode RunPilotAssertionSelfCheckFailMode(Harness& h) {
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "pilot.self.surface",
         "this control needs the english engine with the real list");
    return kEnglishSurfaceUnavailable;
  }
  const Preflight pre = VerifyPilotData(h.pilot_lexicon);
  ReportPreflight(pre, h.pilot_lexicon);
  if (!pre.ok) {
    // Not a disguised assertion failure: if the data itself is wrong here, the honest code
    // is the pre-validation one.
    Fail(kPilotDataRejected, "pilot.self.preflight", pre.reason + ": " + pre.detail);
    return kPilotDataRejected;
  }
  const PilotReference& ref = pre.reference;
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "PILOT-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  PilotRun run(&h, EpoCanvaEnglishCounters());
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "PILOT-SELF",
                                       kPilotSchemaId);
  if (rc == kOk) {
    rc = run.Type(a, "pr", nullptr);
  }
  if (rc == kOk) {
    // The healthy half: 'pr' really does answer with the reference's list, so this run gets
    // past setup and data and can only fail on the claim below.
    CheckFrame(&h.checks, run.last(), "pr", ref.Menu("pr", kTotalCandidates), 8,
               "pilot control: the real menu is compared correctly");
  }
  if (rc == kOk) {
    // One claim, wrong on purpose: the same list with its first two completions swapped.
    // Those two rows sit in one weight tier, so only an order comparison can tell.
    std::vector<std::string> good = ref.Candidates("pr", kTotalCandidates);
    std::vector<std::string> swapped = good;
    if (swapped.size() > 2) {
      std::swap(swapped[1], swapped[2]);
    }
    h.checks.CheckMenu(run.last().state, RefJoinMenu(swapped),
                       "deliberately mis-ranked pilot menu");
    rc = EscapeAndCheck(&run, a);
  }
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "PILOT-SELF");
  if (rc != kOk) {
    return rc;
  }
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
