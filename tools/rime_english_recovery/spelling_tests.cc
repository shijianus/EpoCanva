// EpoCanva P2.6-recovery -- one-edit spelling suggestions, per keystroke.
// See spelling_tests.h for the contract table and for the two independent routes every
// expectation here comes from. Nothing in this file asks the suggester what it thinks: the
// reference below walks the fixture's rows in the reverse direction and restates the gate
// order, and the literal menus are transcribed from tests/fixtures/english_spelling/README.md
// section 4.
#include "spelling_tests.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// ---------------------------------------------------------------- the fixture data
// The 16 rows of tests/fixtures/english_spelling/lexicon_spell.tsv, transcribed by hand
// from the file and documented in tests/fixtures/english_spelling/README.md section 2.
// This is CONTROLLED TEST DATA, not English: each row exists so one edit, one tie or one
// budget cut is observable. The weights make ranking assertable; they are not frequencies
// and are never described as probabilities of a typo.
struct Row {
  const char* word;
  long long weight;
};

constexpr Row kRows[] = {
    {"hello", 9},
    {"help", 8},
    {"hollo", 7},
    {"hallo", 6},
    {"hillo", 6},
    {"hella", 6},
    {"world", 7},
    {"works", 6},
    {"word", 4},
    {"test", 5},
    {"tests", 3},
    {"hell", 4},
    {"helium", 3},
    {"don't", 5},
    {"co-operate", 2},
    {"abcdefghijklmnopqrstuvwxyzabcdef", 1},
};
constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// The window and the budget, restated from the two places that own them. Deliberately
// written out again here: an expectation that was computed from the same expression as the
// implementation could not notice either number changing.
constexpr std::size_t kWindowMinBytes = 4;   // kMinSuggestionWordBytes
constexpr std::size_t kWindowMaxBytes = 32;  // kMaxSuggestionWordBytes
constexpr std::size_t kSuggestionBudget = 3;  // the fixture schema's max_suggestions

// ---------------------------------------------------------------- the named states
// One entry per spelling counter the engine records, with the single-character code the
// reference below uses. The field names are read off engine/rime_components/english/
// english_diagnostics.h:230-250 and the outcome names off
// engine/languages/en/spelling_suggester.cc:280-312. kDisabled has no entry because this
// schema switches the feature on and the translator only asks the core when it is on -- a
// disabled report reaching the counters would be a caller bug, and the per-frame identity
// below would stop balancing.
struct SpellState {
  char code;
  const char* name;
  unsigned long long EpoCanvaEnglishCounters::*field;
};

constexpr SpellState kStates[] = {
    {'s', "suggested", &EpoCanvaEnglishCounters::spelling_suggested},
    {'n', "no-candidate", &EpoCanvaEnglishCounters::spelling_no_candidate},
    {'w', "word-in-lexicon", &EpoCanvaEnglishCounters::spelling_skipped_word_in_lexicon},
    {'c', "completion-available", &EpoCanvaEnglishCounters::spelling_skipped_completion},
    {'p', "protected-input", &EpoCanvaEnglishCounters::spelling_skipped_protected},
    {'d', "delimited-form", &EpoCanvaEnglishCounters::spelling_skipped_delimited},
    {'m', "mixed-case", &EpoCanvaEnglishCounters::spelling_skipped_mixed_case},
    {'T', "too-short", &EpoCanvaEnglishCounters::spelling_skipped_too_short},
    {'L', "too-long", &EpoCanvaEnglishCounters::spelling_skipped_too_long},
    {'u', "unsupported-byte", &EpoCanvaEnglishCounters::spelling_skipped_unsupported_byte},
    {'a', "non-ascii-input", &EpoCanvaEnglishCounters::spelling_skipped_non_ascii},
    {'e', "empty-input", &EpoCanvaEnglishCounters::spelling_skipped_empty},
    {'x', "lexicon-unavailable", &EpoCanvaEnglishCounters::spelling_lexicon_unavailable},
};
constexpr std::size_t kStateCount = sizeof(kStates) / sizeof(kStates[0]);

const SpellState* StateFor(char code) {
  for (std::size_t i = 0; i < kStateCount; ++i) {
    if (kStates[i].code == code) {
      return &kStates[i];
    }
  }
  return nullptr;
}

const char* OpName(char op) {
  switch (op) {
    case 'd':
      return "dele";
    case 'i':
      return "inse";
    case 's':
      return "subs";
    case 't':
      return "tran";
  }
  return "none";
}

// ---------------------------------------------------------- byte classes, restated
// Classes, not characters: english_token.h:71-76 lists digits and these eleven marks as
// technical text, the apostrophe and the hyphen-minus as the two word delimiters, and
// everything else printable-but-unrecognised as an unsupported byte. The order matters
// because it is the token layer's own order (english_token.h:148-157).
bool IsUpperByte(char c) { return c >= 'A' && c <= 'Z'; }
bool IsLowerByte(char c) { return c >= 'a' && c <= 'z'; }
bool IsLetterByte(char c) { return IsUpperByte(c) || IsLowerByte(c); }

bool IsProtectedMarkByte(char c) {
  if (c >= '0' && c <= '9') {
    return true;
  }
  const char marks[] = "@._:/\\?&=#%+";
  for (const char m : marks) {
    if (c == m) {
      return true;
    }
  }
  return false;
}

bool IsDelimiterByte(char c) { return c == '\'' || c == '-'; }

// The three accepted shapes, restated here rather than borrowed from english_case.h so the
// reference really is a second opinion.
enum class Shape { kLower, kInitial, kUpper, kOther };

Shape ShapeOf(const std::string& text) {
  bool any_letter = false;
  bool any_upper = false;
  bool any_lower = false;
  bool seen_letter = false;
  bool upper_after_first = false;
  for (const char c : text) {
    if (!IsLetterByte(c)) {
      return Shape::kOther;  // another gate owns non-letters
    }
    any_letter = true;
    any_upper = any_upper || IsUpperByte(c);
    any_lower = any_lower || IsLowerByte(c);
    if (IsUpperByte(c) && seen_letter) {
      upper_after_first = true;
    }
    seen_letter = true;
  }
  if (!any_letter) {
    return Shape::kOther;
  }
  if (any_upper && any_lower) {
    return upper_after_first ? Shape::kOther : Shape::kInitial;
  }
  return any_upper ? Shape::kUpper : Shape::kLower;
}

std::string LowerCopy(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (IsUpperByte(c)) {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// The dictionary's own spelling, rendered in the shape that was typed.
std::string RenderInShape(const std::string& canonical, Shape shape) {
  std::string out = canonical;
  if (shape == Shape::kUpper) {
    for (char& c : out) {
      if (IsLowerByte(c)) {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    return out;
  }
  if (shape == Shape::kInitial && !out.empty() && IsLowerByte(out[0])) {
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  }
  return out;
}

// ------------------------------------------------- the reverse-direction one-edit test
// Answers "is this ROW reachable from this INPUT by exactly one admitted operation?", by
// comparing the two strings. The implementation under test goes the other way: it generates
// variants of the input and looks each one up. Two directions that agree on a set are
// evidence; a generator certifying its own output would not be.
//
// The operation is decided by the length difference alone, which is why no generation-order
// tie-break is needed here: deletion shortens by one, insertion lengthens by one, and
// substitution and transposition both preserve length but differ in how many positions
// disagree (one versus two adjacent crossed pairs).
//
// Rows holding a delimiter can never be a target: the suggester's insertions and
// substitutions are limited to a-z, so "don't" and "co-operate" are unreachable however the
// input is typed. The fixture keeps those two rows so that fact is asserted rather than
// vacuous.
bool OneEditAway(const std::string& word, const std::string& key, char* op) {
  if (word.find('\'') != std::string::npos || word.find('-') != std::string::npos) {
    return false;
  }
  const long long diff = static_cast<long long>(word.size()) - static_cast<long long>(key.size());
  if (diff == 0) {
    std::size_t mismatches = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < key.size(); ++i) {
      if (key[i] != word[i]) {
        if (mismatches == 0) {
          first = i;
        }
        ++mismatches;
        if (mismatches > 2) {
          return false;
        }
      }
    }
    if (mismatches == 1) {
      *op = 's';
      return true;
    }
    if (mismatches == 2 && first + 1 < key.size() && key[first] == word[first + 1] &&
        key[first + 1] == word[first]) {
      *op = 't';
      return true;
    }
    return false;
  }
  if (diff != -1 && diff != 1) {
    return false;
  }
  // Exactly one byte of the longer string must be absent from the shorter one, and the rest
  // must line up in order.
  const std::string& longer = (diff == 1) ? word : key;
  const std::string& shorter = (diff == 1) ? key : word;
  std::size_t matched = 0;
  std::size_t skipped = 0;
  for (std::size_t i = 0; i < longer.size(); ++i) {
    if (matched < shorter.size() && longer[i] == shorter[matched]) {
      ++matched;
      continue;
    }
    ++skipped;
    if (skipped > 1) {
      return false;
    }
  }
  if (matched != shorter.size() || skipped != 1) {
    return false;
  }
  *op = (diff == -1) ? 'd' : 'i';
  return true;
}

// ---------------------------------------------------------------- the reference answer
struct Candidate {
  std::string canonical;
  long long weight = 0;
  char op = 'n';
};

struct Reference {
  char state = '?';
  // Only meaningful when state == 's': ranked, cut to the budget.
  std::vector<Candidate> suggestions;

  std::string Describe() const {
    std::string out;
    for (const Candidate& c : suggestions) {
      if (!out.empty()) {
        out += " ";
      }
      out += c.canonical + "(" + std::to_string(c.weight) + "," + OpName(c.op) + ")";
    }
    return out;
  }

  // The whole menu the engine must show: the typed text first, then the suggestions in the
  // shape that was typed.
  std::string MenuFor(const std::string& composition) const {
    const Shape shape = ShapeOf(composition);
    std::string out = composition;
    for (const Candidate& c : suggestions) {
      out += "|" + RenderInShape(c.canonical, shape);
    }
    return out;
  }

  std::string CommentsFor() const {
    std::string out;  // the original text carries no comment
    for (std::size_t i = 0; i < suggestions.size(); ++i) {
      out += "|spell";
    }
    return out;
  }
};

// One answer per composition, in the gate order spelling_suggester.h:29-57 documents. The
// input here is what get_input() reported, so the expectation always follows the
// composition the engine actually holds.
Reference ReferenceFor(const std::string& composition) {
  Reference ref;
  if (composition.empty()) {
    ref.state = 'e';
    return ref;
  }
  // Too long is judged before too short and before any content rule, so an over-long token
  // is never classified by a prefix of it.
  if (composition.size() > kWindowMaxBytes) {
    ref.state = 'L';
    return ref;
  }
  if (composition.size() < kWindowMinBytes) {
    ref.state = 'T';
    return ref;
  }
  bool has_delimiter = false;
  for (const char c : composition) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80) {
      ref.state = 'a';
      return ref;
    }
    if (IsProtectedMarkByte(c)) {
      ref.state = 'p';
      return ref;
    }
    if (!IsLetterByte(c) && !IsDelimiterByte(c)) {
      ref.state = 'u';
      return ref;
    }
    if (IsDelimiterByte(c)) {
      has_delimiter = true;
    }
  }
  if (has_delimiter) {
    // Both a legal delimited form and an illegal one arrive here, and both are outside this
    // round: english_token.h's kIllegalForm and the suggester's delimiter check report the
    // same named state.
    ref.state = 'd';
    return ref;
  }
  const Shape shape = ShapeOf(composition);
  if (shape == Shape::kOther) {
    ref.state = 'm';
    return ref;
  }
  const std::string key = LowerCopy(composition);
  for (std::size_t i = 0; i < kRowCount; ++i) {
    if (key == kRows[i].word) {
      ref.state = 'w';
      return ref;
    }
  }
  // Gate 9 is judged on the completion COUNT, which by construction excludes a row equal to
  // the input -- and a row equal to the input was already answered as 'w' above. A strictly
  // longer row that starts with this key is what an ordinary completion would have offered.
  for (std::size_t i = 0; i < kRowCount; ++i) {
    const std::string row = kRows[i].word;
    if (row.size() > key.size() && row.compare(0, key.size(), key) == 0) {
      ref.state = 'c';
      return ref;
    }
  }
  std::vector<Candidate> hits;
  for (std::size_t i = 0; i < kRowCount; ++i) {
    char op = 'n';
    if (OneEditAway(kRows[i].word, key, &op)) {
      hits.push_back({kRows[i].word, kRows[i].weight, op});
    }
  }
  // Weight descending, then canonical text ascending by byte value -- the total order every
  // list in this project uses, restated here as a plain insertion sort over at most 16 rows.
  for (std::size_t i = 1; i < hits.size(); ++i) {
    for (std::size_t j = i; j > 0; --j) {
      const bool earlier = hits[j].weight > hits[j - 1].weight ||
                           (hits[j].weight == hits[j - 1].weight &&
                            hits[j].canonical < hits[j - 1].canonical);
      if (!earlier) {
        break;
      }
      std::swap(hits[j], hits[j - 1]);
    }
  }
  if (hits.empty()) {
    ref.state = 'n';
    return ref;
  }
  ref.state = 's';
  const std::size_t keep = hits.size() < kSuggestionBudget ? hits.size() : kSuggestionBudget;
  ref.suggestions.assign(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep));
  return ref;
}

// ------------------------------------------------------------------- the probe table
// One row per input. `want_state` is the documented result for the COMPLETED input, taken
// from tests/fixtures/english_spelling/README.md section 4; the per-frame states in between
// come from the reference, and this column is what notices a reference that has drifted.
// `wanted_menu` / `wanted_comments` are the final frame, pipe-joined, exactly as the fixture
// documentation writes them; nullptr means "one item: the text that was typed, with no
// comment" -- which is what every suppression row looks like.
struct Probe {
  const char* text;
  char want_state;
  const char* wanted_menu;
  const char* wanted_comments;
  const char* why;
};

// The two length-edge probes below are written as complete 32- and 33-byte literals rather
// than composed from a constant: C++ cannot concatenate a variable with a literal, and a
// silent substitution in a composed string would hide a one-byte mistake.

const Probe kProbes[] = {
    // The four admitted operations, each on an input where it is the only route, or where
    // several routes exist and the fixture documents which one is reported.
    {"helo", 's', "helo|hello|help|hell", "|spell|spell|spell",
     "insertion to hello, substitution to help and to hell"},
    {"hcello", 's', "hcello|hello", "|spell", "one deletion: the inserted c"},
    {"ehllo", 's', "ehllo|hello", "|spell", "transposition of the first pair"},
    {"wrld", 's', "wrld|world", "|spell", "insertion of the missing o"},
    {"wirld", 's', "wirld|world", "|spell", "substitution of the i for an o"},
    {"hxllo", 's', "hxllo|hello|hollo|hallo", "|spell|spell|spell",
     "four rows are one substitution away; hillo(6) loses the tie-break to hallo(6) and is "
     "cut by the budget"},
    {"wrod", 's', "wrod|word", "|spell", "transposition inside the word"},
    {"wordd", 's', "wordd|world|word", "|spell|spell",
     "substitution to world, deletion to word"},
    {"testz", 's', "testz|test|tests", "|spell|spell",
     "deletion to test, substitution to tests"},
    {"helllo", 's', "helllo|hello", "|spell",
     "three deletions reach the same row, which is shown once"},
    // Searched, and the index rejected every variant. hello is TWO edits from hxlo, so it
    // must not appear: that is the no-multi-edit assertion.
    {"hxlo", 'n', nullptr, nullptr, "the search ran and nothing is one edit away"},
    {"zxqj", 'n', nullptr, nullptr, "no row is within one edit of this"},
    // Already-correct words: their own named reason, never "no match".
    {"help", 'w', nullptr, nullptr, "a complete row of the list"},
    {"hello", 'w', nullptr, nullptr, "the very row the suggestions above aim at"},
    {"HELLO", 'w', nullptr, nullptr, "the uppercase of a row is already correct too"},
    // A completion suppresses a suggestion even though help and hell are one edit away.
    {"heli", 'c', "heli|helium", "|", "helium completes, so nothing is suggested"},
    // Both edges of the window, and just outside each -- as complete 32- and 33-byte
    // literals rather than composed from a constant: C++ cannot concatenate a variable with a
    // literal, and a silent substitution in a composed string would hide a one-byte mistake.
    {"hel", 'T', "hel|hello|help|hella|hell|helium", "|||||",
     "three letters is below the window, and the completions appear anyway"},
    {"hlo", 'T', nullptr, nullptr, "below the window and nothing completes it"},
    {"abcdefghijklmnopqrstuvwxyzabcdeq", 's',
     "abcdefghijklmnopqrstuvwxyzabcdeq|abcdefghijklmnopqrstuvwxyzabcdef", "|spell",
     "32 letters is inside the window; one substitution reaches the long row"},
    {"abcdefghijklmnopqrstuvwxyzabcdefz", 'L', nullptr, nullptr,
     "33 letters is refused outright and never cut down first"},
    // Casing.
    {"hEllo", 'm', nullptr, nullptr, "mixed case is never guessed at"},
    {"HEllo", 'm', nullptr, nullptr, "mixed case in the other shape"},
    {"Helo", 's', "Helo|Hello|Help|Hell", "|spell|spell|spell",
     "initial-capital suggestions are rendered in the shape that was typed"},
    {"HELO", 's', "HELO|HELLO|HELP|HELL", "|spell|spell|spell",
     "uppercase suggestions likewise"},
    // Technical text, refused whole at the same lengths the suggestions above use.
    {"h3llo", 'p', nullptr, nullptr, "a digit stops this round"},
    {"h.elo", 'p', nullptr, nullptr, "a full stop stops this round"},
    {"user@example.com", 'p', nullptr, nullptr,
     "an address is refused whole, never completed letter by letter"},
    {"https://example.com/a?x=1", 'p', nullptr, nullptr, "a URL"},
    {"C:\\tmp\\file.txt", 'p', nullptr, nullptr, "a Windows path"},
    // The two word delimiters: refused on FORM, which is judged before membership.
    {"don't", 'd', nullptr, nullptr, "an apostrophe form never enters this round"},
    {"co-op", 'd', "co-op|co-operate", "|",
     "a hyphen form is refused even while a completion exists"},
    {"co-operate", 'd', nullptr, nullptr,
     "a correct delimited row is refused on form, not on membership"},
};
constexpr std::size_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);

// The documented per-candidate expectations, "(canonical weight,operation)" in list order,
// from tests/fixtures/english_spelling/README.md section 4. Checked against the reference
// and only against the reference: it is how a mistake in the reference is caught instead of
// inherited by every frame it predicts.
struct Documented {
  const char* input;
  const char* expectation;
};

const Documented kDocumented[] = {
    {"helo", "hello(9,inse) help(8,subs) hell(4,subs)"},
    {"hcello", "hello(9,dele)"},
    {"ehllo", "hello(9,tran)"},
    {"wrld", "world(7,inse)"},
    {"wirld", "world(7,subs)"},
    {"hxllo", "hello(9,subs) hollo(7,subs) hallo(6,subs)"},
    {"wrod", "word(4,tran)"},
    {"wordd", "world(7,subs) word(4,dele)"},
    {"testz", "test(5,dele) tests(3,subs)"},
    {"helllo", "hello(9,dele)"},
};
constexpr std::size_t kDocumentedCount = sizeof(kDocumented) / sizeof(kDocumented[0]);

// ------------------------------------------------------------------- small utilities
long long LL(unsigned long long v) { return static_cast<long long>(v); }

std::string U64(unsigned long long v) { return std::to_string(v); }

// How many rows of the comment column carry the fixed spelling label. Candidate 0 and every
// completion carry none, so this counts suggestions actually on screen.
long long LabelledRows(const StateSnapshot& s) {
  long long n = 0;
  for (const std::string& c : s.comments) {
    if (c == EPOCANVA_ENGLISH_SPELLING_COMMENT) {
      ++n;
    }
  }
  return n;
}

// Every comment that is NOT the spelling label must be empty: a third kind of label, or a
// label on a completion, would make the two kinds indistinguishable to a front end. The
// column is joined by the harness's own JoinComments, whose separator rule is index-based --
// a "did I write anything yet" rule would swallow the separator after candidate 0's empty
// comment and make a labelled menu look one field shorter than it is.

// What one keystroke frame is allowed to have cost. Recorded so the report can say what was
// measured rather than what the algorithm is expected to do; the assertions themselves use
// the per-frame deltas, not these totals.
struct FrameTally {
  long long frames = 0;
  long long single_query_frames = 0;
  long long queries = 0;
  long long max_queries_in_a_frame = 0;
};

}  // namespace

ExitCode RunSpellingMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "spelling.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "spelling.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_init        %s\n", DescribeCounters(c).c_str());
  h.checks.Check(c.module_initialized >= 1, "the module's initialize() ran",
                 U64(c.module_initialized));
  // Said before any query exists, so "the counters were zero all along" cannot be a later
  // inference: the feature does not run until a schema asks for it.
  h.checks.CheckNum(SpellingTotal(c), 0, "no spelling counter before the first query");
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_attempts), 0,
                    "no word list read before the first session");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "SPELL-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "spelling A",
                                       kSpellingSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_select      %s\n", DescribeCounters(c).c_str());
  // The candidate list alone cannot tell "loaded the 16 controlled rows" from "loaded
  // nothing and fell back to the typed text", so the load is asserted as a fact of its own.
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1, "the 16-row list was read once");
  h.checks.CheckNum(LL(c.lexicon_load_successes), 1, "and it loaded");
  h.checks.CheckNum(LL(c.lexicon_load_failures), 0, "with no failure recorded");
  h.checks.CheckNum(LL(c.loaded_entry_count), kExpectedSpellingLexiconRows,
                    "every controlled row reached the index");
  h.checks.CheckNum(LL(c.last_load_error_line), 0, "no load error line");
  h.checks.CheckNum(LL(c.config_missing_path), 0,
                    "the schema did name a word list, so this is not a configuration error");
  h.checks.CheckStr(LastStatusName(h.surface), "ok", "spelling word list status");

  // The reference's own agreement with the documented table, asserted before it is used to
  // predict anything.
  for (std::size_t i = 0; i < kDocumentedCount; ++i) {
    const Reference ref = ReferenceFor(kDocumented[i].input);
    h.checks.CheckNum(static_cast<long long>(ref.state),
                      static_cast<long long>('s'),
                      std::string("reference predicts 'suggested' for '") +
                          kDocumented[i].input + "'");
    h.checks.CheckStr(ref.Describe(), kDocumented[i].expectation,
                      std::string("reference vs README section 4 for '") +
                          kDocumented[i].input + "'");
  }

  FrameTally tally;
  KeyEvent ev;

  for (std::size_t p = 0; p < kProbeCount; ++p) {
    const Probe& probe = kProbes[p];
    const std::string text(probe.text);
    const std::string label =
        std::string("spelling '") + probe.text + "' (" + probe.why + ")";

    // Start every input from an observed empty composition rather than from whatever the
    // previous probe left behind.
    rc = SendKey(h.api, a, kXkEscape, 0, &h.balance, &ev);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    h.checks.CheckStr(ev.state.raw_input, "",
                      label + ": [Escape cleared the composition]");

    EpoCanvaEnglishCounters prev;
    if (!ReadCounters(h.surface, &prev)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    const EpoCanvaEnglishCounters start = prev;
    long long frame_queries = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
      const int keycode = static_cast<int>(static_cast<unsigned char>(text[i]));
      rc = SendKey(h.api, a, keycode, 0, &h.balance, &ev);
      if (rc != kOk) {
        h.api->destroy_session(a);
        return rc;
      }
      EpoCanvaEnglishCounters cur;
      if (!ReadCounters(h.surface, &cur)) {
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      const std::string composition = ev.state.raw_input;
      const std::string frame = label + " frame " + std::to_string(i + 1);
      const Reference ref = ReferenceFor(composition);
      const SpellState* want = StateFor(ref.state);
      if (want == nullptr) {
        Fail(kUsage, "spelling.reference", frame + ": the reference produced a state code '" +
                                            std::string(1, ref.state) +
                                            "' that no counter records");
        h.api->destroy_session(a);
        return kUsage;
      }

      const long long dq = CounterDelta(prev, cur, &EpoCanvaEnglishCounters::queries);
      ++tally.frames;
      tally.queries += dq;
      frame_queries += dq;
      if (dq == 1) {
        ++tally.single_query_frames;
      }
      if (dq > tally.max_queries_in_a_frame) {
        tally.max_queries_in_a_frame = dq;
      }
      h.checks.CheckNum(dq >= 1 ? 1 : 0, 1,
                         frame + " [this keystroke ran at least one Query]");

      // One spelling answer per query, and the same one for every query in the frame: the
      // composition did not change in between. Summing over all thirteen counters is what
      // stops a state that no field records from being quietly invented.
      long long summed = 0;
      long long movers = 0;
      const SpellState* moved = nullptr;
      for (std::size_t s = 0; s < kStateCount; ++s) {
        const long long d = CounterDelta(prev, cur, kStates[s].field);
        summed += d;
        if (d != 0) {
          ++movers;
          moved = &kStates[s];
        }
      }
      h.checks.CheckNum(summed, dq,
                        frame + " [every query reported one named state]");
      h.checks.CheckNum(movers, 1, frame + " [exactly one state counter moved]");
      h.checks.Check(moved != nullptr && moved->code == ref.state,
                     frame + " [state is the one the reference predicts: '" +
                         std::string(want->name) + "']",
                     moved == nullptr ? "nothing moved"
                                      : std::string(moved->name) + " x" +
                                            std::to_string(summed));

      const long long appended =
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::spelling_candidates_appended);
      const long long labels = LabelledRows(ev.state);
      const long long variants =
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::spelling_variants_generated);
      const long long lookups =
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::spelling_lexicon_lookups);
      const long long shapes =
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::spelling_suggestions_lowercase) +
          CounterDelta(prev, cur,
                       &EpoCanvaEnglishCounters::spelling_suggestions_initial_capital) +
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::spelling_suggestions_uppercase);

      if (ref.state == 's') {
        // The rows on screen are the rows this frame appended, times the number of times the
        // frame was translated (a re-translation replaces the menu without shortening the
        // counter). The comment column is the only suggestion/completion distinction a front
        // end can see, so it is asserted, not assumed.
        h.checks.CheckNum(appended, labels * dq,
                          frame + " [suggestions appended match the spell column]");
        h.checks.CheckNum(labels >= 1 ? 1 : 0, 1,
                          frame + " [the label is visible through the C API]");
        h.checks.CheckNum(shapes, appended,
                          frame + " [every suggestion carries one case shape]");
        h.checks.Check(variants > 0 && lookups == variants,
                       frame + " [one index lookup per generated variant]",
                       std::to_string(variants) + "/" + std::to_string(lookups));
      } else {
        h.checks.CheckNum(appended, 0,
                          frame + " [state '" + std::string(want->name) +
                              "' suggested nothing]");
        h.checks.CheckNum(labels, 0, frame + " [nothing was labelled in the column]");
        h.checks.CheckNum(shapes, 0,
                          frame + " [state '" + std::string(want->name) +
                              "' rendered nothing]");
        if (ref.state == 'n') {
          // The one non-suggesting state that DID search: refusing to invent a candidate is
          // not the same as refusing to look.
          h.checks.Check(variants > 0 && lookups == variants,
                         frame + " [no-candidate still searched the index]",
                         std::to_string(variants) + "/" + std::to_string(lookups));
        } else {
          // Every other refusal short-circuits before generation, so a keystroke costs no
          // edit work at all -- which is what makes "the gates short-circuit" an observed
          // fact rather than a claim about the code.
          h.checks.CheckNum(variants, 0,
                            frame + " [state '" + std::string(want->name) +
                                "' refused before generating variants]");
          h.checks.CheckNum(lookups, 0,
                            frame + " [state '" + std::string(want->name) +
                                "' made no index lookup]");
        }
      }
      h.checks.CheckNum(
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::lexicon_load_attempts), 0,
          frame + " [no word list re-read for this keystroke]");
      h.checks.CheckNum(
          CounterDelta(prev, cur, &EpoCanvaEnglishCounters::queries_covering_part_of_input),
          0, frame + " [the token reached the translator whole]");

      // Per frame, whatever the state: the text the user typed is candidate 0, the page is
      // the schema's, and no suggestion menu has widened it.
      h.checks.CheckStr(ev.state.preedit, composition, frame + " [preedit text]");
      h.checks.CheckNum(ev.state.preedit_length,
                        static_cast<long long>(composition.size()),
                        frame + " [preedit length (UTF-8 code units)]");
      if (!ev.state.candidates.empty()) {
        h.checks.CheckStr(ev.state.candidates[0], composition,
                          frame + " [candidate 0 is the typed text]");
      }
      h.checks.CheckNum(ev.state.page_size, 8, frame + " [menu page_size]");
      h.checks.CheckNum(ev.state.num_candidates <= 8 ? 1 : 0, 1,
                        frame + " [menu stays within the 8-candidate ceiling]");
      prev = cur;
    }

    // Counters after the last keystroke, so the per-frame sums can be compared with the
    // whole input rather than with an assumed keystroke count.
    EpoCanvaEnglishCounters end_of_probe;
    if (!ReadCounters(h.surface, &end_of_probe)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }

    // ---- the final frame, against literals and against the reference -------------------
    const std::string want_menu =
        probe.wanted_menu != nullptr ? std::string(probe.wanted_menu) : text;
    const std::string want_comments =
        probe.wanted_comments != nullptr ? std::string(probe.wanted_comments) : std::string();
    h.checks.CheckMenu(ev.state, want_menu, label + " [final menu]");
    h.checks.CheckStr(JoinComments(ev.state), want_comments,
                      label + " [final comment column]");
    h.checks.CheckStr(ev.state.raw_input, text, label + " [raw composition read by get_input]");
    const Reference final_ref = ReferenceFor(text);
    h.checks.CheckNum(static_cast<long long>(final_ref.state),
                      static_cast<long long>(probe.want_state),
                      label + " [documented state for the finished input]");
    if (final_ref.state == 's') {
      h.checks.CheckStr(JoinMenu(ev.state), final_ref.MenuFor(text),
                        label + " [menu equals the reference's own derivation]");
      h.checks.CheckStr(JoinComments(ev.state), final_ref.CommentsFor(),
                        label + " [label column equals the reference's own derivation]");
    }
    h.checks.Check(!ev.state.commit_present, label + " [typing committed nothing]",
                   ev.state.commit_present ? ev.state.commit_text : std::string("none"));
    h.checks.CheckNum(frame_queries, LL(end_of_probe.queries - start.queries),
                      label + " [frame accounting covers every query of this input]");
    std::printf("[english] spell %-34s menu=%s cmts=%s\n", probe.text,
                JoinMenu(ev.state).c_str(), JoinComments(ev.state).c_str());
  }

  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "the whole gate table never re-read the word list");

  // ---- BackSpace recomputes; it does not replay a stale list ---------------------------
  rc = SendKey(h.api, a, kXkEscape, 0, &h.balance, &ev);
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'h', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'e', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'l', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'o', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, "helo|hello|help|hell",
                       "'helo' offers suggestions before BackSpace");
    h.checks.CheckStr(JoinComments(ev.state), "|spell|spell|spell",
                      "three labelled rows before BackSpace");
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, kXkBackSpace, 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    // "hel" is below the length window AND has completions, so what comes back is the
    // completion menu with an empty label column: the list was recomputed from the shorter
    // composition instead of being carried over.
    h.checks.CheckMenu(ev.state, "hel|hello|help|hella|hell|helium",
                       "after BackSpace the completion menu is back");
    h.checks.CheckStr(JoinComments(ev.state), "|||||",
                      "after BackSpace no label is left in the column");
    h.checks.CheckStr(ev.state.raw_input, "hel", "and get_input() reads the shorter token");
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'l', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, 'o', 0, &h.balance, &ev);
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, "hello",
                       "retyping to a correct word gets no suggestion and no completion");
    h.checks.CheckStr(JoinComments(ev.state), "", "a correct row carries no label");
  }
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- Escape leaves no suggestion behind ----------------------------------------------
  rc = SendKey(h.api, a, kXkEscape, 0, &h.balance, &ev);
  if (rc == kOk) {
    h.checks.Check(!ev.state.status_composing, "Escape ended the composition",
                   "still composing");
    h.checks.CheckNum(ev.state.num_candidates, 0, "Escape cleared the menu");
    h.checks.CheckNum(LabelledRows(ev.state), 0, "Escape left no labelled row");
    h.checks.Check(!ev.state.commit_present, "cancelling committed nothing",
                   ev.state.commit_present ? ev.state.commit_text : std::string("none"));
  }

  // ---- cumulative identities ------------------------------------------------------------
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.before_destroy    %s\n", DescribeCounters(c).c_str());
  std::printf("[english] spelling.frames            frames=%lld one-query=%lld queries=%lld "
              "max=%lld\n",
              tally.frames, tally.single_query_frames, tally.queries,
              tally.max_queries_in_a_frame);
  h.checks.CheckNum(LL(c.spelling_suggestions_lowercase +
                       c.spelling_suggestions_initial_capital +
                       c.spelling_suggestions_uppercase),
                    LL(c.spelling_candidates_appended),
                    "suggestions appended = the three case shapes");
  // The core reports one lookup per generated variant, and this run never scanned the list,
  // so the two cumulative counters must be EQUAL rather than merely ordered.
  h.checks.CheckNum(LL(c.spelling_lexicon_lookups), LL(c.spelling_variants_generated),
                    "one index lookup per generated variant, cumulatively");
  h.checks.Check(c.spelling_variants_generated > 0,
                 "variants really were generated (zero would mean every query was "
                 "short-circuited by a gate)",
                 U64(c.spelling_variants_generated));
  // Three states this round cannot reach, each named rather than silently absent:
  h.checks.CheckNum(LL(c.spelling_skipped_empty), 0,
                    "an empty composition never reached the suggester");
  h.checks.CheckNum(LL(c.spelling_skipped_unsupported_byte), 0,
                    "the speller's alphabet refuses ';' and friends, so no frame can report "
                    "an unsupported byte through real keys (engine-side scope limit, same as "
                    "P2.2/P2.3)");
  h.checks.CheckNum(LL(c.spelling_skipped_non_ascii), 0,
                    "and likewise no byte >= 0x80: this program types printable ASCII only");
  h.checks.CheckNum(LL(c.spelling_lexicon_unavailable), 0,
                    "the word list this mode deployed loaded, so no frame reported it "
                    "missing (the other mode owns that state)");
  h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                    "every token still reached the translator whole");
  h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                    "and every query covered the whole token");
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "still exactly one word list read at the end");
  h.checks.CheckNum(LL(c.module_finalized), 0, "module not finalized while the surface is held");

  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "spelling session A");
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  std::printf("[english] counters.final             %s\n", DescribeCounters(c).c_str());
  h.checks.CheckNum(LL(c.translator_live), 0, "no translator outlives its session");
  h.checks.CheckNum(LL(c.translator_constructed), LL(c.translator_destroyed),
                    "translator constructed == destroyed");
  return done;
}

}  // namespace recovery
}  // namespace epocanva
