// EpoCanva P2.1 -- unit tests for the English language core.
//
// These exercise engine/languages/en/ directly: no librime header, no engine, no
// Session. That separation is the point -- the completion result is checked
// before any of it is claimed to be a Rime candidate.
//
// EXPECTATIONS ARE WRITTEN OUT, not computed. A test that calls the function
// under test to obtain its own expected value cannot fail, so every expected
// sequence below is a literal derived by hand from kEnglishFixture.
//
// EXIT CODES: 0 = all checks passed; 1 = at least one value assertion failed;
// 2 = the harness could not prepare its scratch directory (reported, not skipped).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "english_lexicon.h"
#include "english_space_policy.h"
#include "prefix_completer.h"
#include "spelling_suggester.h"

namespace {

using epocanva::en::Completion;
using epocanva::en::CompletionResult;
using epocanva::en::EnglishLexicon;
using epocanva::en::kMaxCompletions;
using epocanva::en::kMaxCaseInputBytes;
using epocanva::en::kMaxPrefixBytes;
using epocanva::en::kMaxWordBytes;
using epocanva::en::LexiconEntry;
using epocanva::en::LexiconLoadInfo;
using epocanva::en::LexiconStatus;
using epocanva::en::CaseStyle;
using epocanva::en::CompletionOutcome;
using epocanva::en::DelimiterForm;
using epocanva::en::QueryDecision;
using epocanva::en::StopClass;
using epocanva::en::TokenAnalysis;
using epocanva::en::SpellingEdit;
using epocanva::en::SpellingOutcome;
using epocanva::en::SpellingResult;
using epocanva::en::SpellingSuggestion;
using epocanva::en::kMaxSuggestionWordBytes;
using epocanva::en::kMinSuggestionWordBytes;

int g_failed = 0;
int g_checked = 0;

void ReportFailure(const std::string& label, const std::string& got,
                   const std::string& wanted) {
  ++g_failed;
  std::cout << "  ASSERT FAIL " << label << "\n    got    : " << got
            << "\n    wanted : " << wanted << std::endl;
}

void Check(const std::string& got, const std::string& wanted,
           const std::string& label) {
  ++g_checked;
  if (got != wanted) {
    ReportFailure(label, got, wanted);
  }
}

void CheckNum(long long got, long long wanted, const std::string& label) {
  ++g_checked;
  if (got != wanted) {
    ReportFailure(label, std::to_string(got), std::to_string(wanted));
  }
}

void CheckTrue(bool condition, const std::string& label,
               const std::string& describe) {
  ++g_checked;
  if (!condition) {
    ReportFailure(label, describe, "true");
  }
}

std::string StatusName(LexiconStatus s) {
  return std::string(epocanva::en::LexiconStatusName(s));
}
std::string StatusName(CompletionOutcome o) {
  return epocanva::en::CompletionOutcomeName(o);
}
std::string StatusName(QueryDecision d) {
  return epocanva::en::QueryDecisionName(d);
}
std::string StatusName(DelimiterForm f) {
  return epocanva::en::DelimiterFormName(f);
}
std::string StatusName(StopClass c) {
  return epocanva::en::StopClassName(c);
}
std::string StatusName(CaseStyle s) {
  return std::string(epocanva::en::CaseStyleName(s));
}

// "word/weight,word/weight" -- the full text of a result, so an ordering
// mistake and a content mistake both show up.
std::string Join(const std::vector<Completion>& v) {
  std::string out;
  for (const Completion& c : v) {
    if (!out.empty()) {
      out += ",";
    }
    out += c.display + "/" + std::to_string(c.weight);
  }
  return out;
}

std::string JoinEntries(const std::vector<LexiconEntry>& v) {
  std::string out;
  for (const LexiconEntry& e : v) {
    if (!out.empty()) {
      out += ",";
    }
    out += e.word + "/" + std::to_string(e.weight);
  }
  return out;
}

std::string JoinComma(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) {
    if (!out.empty()) {
      out += ",";
    }
    out += s;
  }
  return out;
}

// Asserts the result contains exactly `wanted`, in that order, ignoring weights;
// weight assertions are made separately so a weight bug does not hide behind a
// word bug.
void CheckWordList(const CompletionResult& r,
                   const std::vector<std::string>& wanted,
                   const std::string& label) {
  std::vector<std::string> got;
  for (const Completion& c : r.completions) {
    got.push_back(c.display);
  }
  Check(JoinComma(got), JoinComma(wanted), label);
}

// 14 entries. Weights are chosen so one query exercises both ordering rules.
//
//   a 5   aa 3   ab 9   abc 7   able 7   about 2   above 9   apple 4
//   banana 6    band 6   bat 1   better 8   cat 4   zebra 5
const char* const kEnglishFixture =
    "banana\t6\n"
    "a\t5\n"
    "better\t8\n"
    "ab\t9\n"
    "able\t7\n"
    "zebra\t5\n"
    "above\t9\n"
    "bat\t1\n"
    "apple\t4\n"
    "abc\t7\n"
    "about\t2\n"
    "cat\t4\n"
    "aa\t3\n"
    "band\t6\n";

// Same 14 rows in a different order; results must not move.
const char* const kShuffledFixture =
    "cat\t4\n"
    "band\t6\n"
    "a\t5\n"
    "zebra\t5\n"
    "able\t7\n"
    "aa\t3\n"
    "above\t9\n"
    "banana\t6\n"
    "abc\t7\n"
    "about\t2\n"
    "bat\t1\n"
    "better\t8\n"
    "apple\t4\n"
    "ab\t9\n";

// The index of kEnglishFixture, written out: ascending bytes, so "a" before "aa"
// before "ab" ... and "banana" before "band" before "bat".
const char* const kFixtureSortedIndex =
    "a/5,aa/3,ab/9,abc/7,able/7,about/2,above/9,apple/4,"
    "banana/6,band/6,bat/1,better/8,cat/4,zebra/5";

void ExpectLoad(const std::string& text, LexiconStatus wanted_status,
                long long wanted_line, long long wanted_entries,
                const std::string& label) {
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(text, &info);
  Check(StatusName(info.status), StatusName(wanted_status),
        label + " [status]");
  CheckNum(static_cast<long long>(info.line), wanted_line, label + " [line]");
  CheckNum(static_cast<long long>(info.entry_count), wanted_entries,
           label + " [entries]");
  // A rejected file must never hand back a usable object, and a successful load
  // must never hand back null: "failed" and "loaded but empty" stay distinct.
  CheckTrue((wanted_status == LexiconStatus::kOk) == (lexicon != nullptr),
           label + " [handle matches status]",
           lexicon ? "non-null" : "null");
}

// --- 1. a well-formed lexicon loads, and the index is in word order ----------
void TestNormalLoad() {
  std::cout << "[en-core] case 1: normal load" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 1: load", StatusName(info.status), "ok");
    return;
  }
  Check(StatusName(info.status), "ok", "case 1: status");
  CheckNum(static_cast<long long>(lexicon->size()), 14, "case 1: entry count");
  // The ascending-word layout is a contract, not an implementation detail: the
  // prefix range search is only correct under it.
  Check(JoinEntries(lexicon->entries()), kFixtureSortedIndex,
        "case 1: index order and payload");
}

// --- 2. the matched SET is right, including the range edges -----------------
void TestQuerySets() {
  std::cout << "[en-core] case 2: query sets" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 2: load", StatusName(info.status), "ok");
    return;
  }
  // "a" -> a aa ab abc able about above apple (8 rows in range). Since P2.2 the
  // core, not the Translator, drops the row whose text repeats the input, so
  // match_count is 7 and the raw range size is match_count + deduplicated = 8.
  CompletionResult ra = epocanva::en::CompletePrefix(*lexicon, "a", 8);
  CheckNum(static_cast<long long>(ra.match_count) +
               static_cast<long long>(ra.deduplicated),
           8, "case 2: prefix a range size");
  CheckNum(static_cast<long long>(ra.match_count), 7,
           "case 2: prefix a count after dropping the exact match");
  Check(Join(ra.completions), "ab/9,above/9,abc/7,able/7,apple/4,aa/3,about/2",
        "case 2: prefix a full result");

  // "ab" -> ab abc able about above (5 rows; "about" starts with a-b-o so it is
  // in range, which is exactly the case a hand-derived expectation must not miss).
  CompletionResult rab = epocanva::en::CompletePrefix(*lexicon, "ab", 8);
  CheckNum(static_cast<long long>(rab.match_count) +
               static_cast<long long>(rab.deduplicated),
           5, "case 2: prefix ab range size");
  Check(Join(rab.completions), "above/9,abc/7,able/7,about/2",
        "case 2: prefix ab full result");

  // "aa" must pick up only "aa": not the shorter "a", and not "ab"/"abc"/
  // "able"/"about"/"above"/"apple", which diverge at byte 2. That single row
  // repeats the input, so the range is proven to have held exactly one entry and
  // it was the dictionary's own "aa" -- an empty list on its own would not say
  // which of the two happened.
  CompletionResult raa = epocanva::en::CompletePrefix(*lexicon, "aa", 8);
  CheckNum(static_cast<long long>(raa.deduplicated), 1,
           "case 2: aa matched only its own row");
  CheckNum(static_cast<long long>(raa.match_count), 0,
           "case 2: aa leaves no completion");
  // A whole word is its own range endpoint, same reasoning.
  CompletionResult rabc = epocanva::en::CompletePrefix(*lexicon, "abc", 8);
  CheckNum(static_cast<long long>(rabc.deduplicated), 1,
           "case 2: abc matched only its own row");
  Check(Join(rabc.completions), "", "case 2: exact word is not repeated");
  // Dictionary head and tail.
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "ze", 8), {"zebra"},
                "case 2: dictionary tail");
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "ban", 8),
                {"banana", "band"}, "case 2: ban then band ordering");
  // A prefix that lands between two words matches nothing rather than returning
  // the neighbour the binary search stopped at.
  CompletionResult rgap = epocanva::en::CompletePrefix(*lexicon, "aq", 8);
  CheckNum(static_cast<long long>(rgap.match_count), 0, "case 2: gap count");
  Check(Join(rgap.completions), "", "case 2: gap yields nothing");
  CompletionResult rzz = epocanva::en::CompletePrefix(*lexicon, "zz", 8);
  CheckNum(static_cast<long long>(rzz.match_count), 0,
           "case 2: prefix past the last letter yields nothing");
}

// --- 3 + 4. weight descending, then ascending bytes -------------------------
void TestOrdering() {
  std::cout << "[en-core] case 3/4: weight then ascii order" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 3: load", StatusName(info.status), "ok");
    return;
  }
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "b", 8),
                {"better", "banana", "band", "bat"},
                "case 3: weight descending");
  // banana/band both carry weight 6, so the tie-break alone decides them.
  CompletionResult rb = epocanva::en::CompletePrefix(*lexicon, "b", 8);
  if (rb.completions.size() == 4) {
    Check(std::to_string(rb.completions[1].weight) + "/" +
              std::to_string(rb.completions[2].weight),
          "6/6", "case 4: the middle pair really is tied");
    Check(rb.completions[1].canonical + "<" + rb.completions[2].canonical,
          "banana<band", "case 4: tie broken by ascending canonical bytes");
  }
  // Weight 9 tie for "a": "ab" is a strict prefix of "above" so it sorts first.
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "a", 2), {"ab", "above"},
                "case 4: weight-9 tie ab before above");
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "c", 8), {"cat"},
                "case 3: single match");

  // Weight 0 and uint32 max are legal values: neither may disappear or be
  // treated as absent.
  LexiconLoadInfo edge_info;
  std::unique_ptr<EnglishLexicon> edges = EnglishLexicon::FromText(
      "za\t0\nzb\t1\nzc\t4294967295\n", &edge_info);
  if (!edges) {
    ReportFailure("case 3: weight edge load", StatusName(edge_info.status),
                  "ok");
    return;
  }
  CheckWordList(epocanva::en::CompletePrefix(*edges, "z", 8),
                {"zc", "zb", "za"},
                "case 3: uint32 max first, weight 0 last, none dropped");

  LexiconLoadInfo junk_info;
  const bool junk_rejected =
      EnglishLexicon::FromText("zz\ta\n", &junk_info) == nullptr;
  CheckTrue(junk_rejected, "case 3: non-digit weight rejected",
            StatusName(junk_info.status));
}

// --- 5 + 6 + 7. no match, empty input, unsupported and over-long input ------
void TestRejectedInputs() {
  std::cout << "[en-core] case 5/6/7: no match and rejected prefixes"
            << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 5: load", StatusName(info.status), "ok");
    return;
  }
  CompletionResult rq = epocanva::en::CompletePrefix(*lexicon, "q", 8);
  Check(StatusName(rq.case_style), "lowercase",
        "case 5: valid lowercase prefix with no matches");
  CheckTrue(rq.lookup_performed, "case 5: the index really was consulted",
            rq.lookup_performed ? "yes" : "no");
  Check(Join(rq.completions), "", "case 5: no matches returns nothing");

  CompletionResult rempty = epocanva::en::CompletePrefix(*lexicon, "", 8);
  Check(StatusName(rempty.case_style), "empty", "case 6: empty input");
  CheckTrue(!rempty.lookup_performed, "case 6: empty input is not looked up",
            rempty.lookup_performed ? "yes" : "no");
  Check(Join(rempty.completions), "", "case 6: empty input returns nothing");

  // Each uncompletable shape keeps its own answer. Nothing is repaired on the way:
  // no lowercasing, no trimming, no truncation, no letter extraction, no deletion
  // of the byte that caused the refusal.
  // P2.3 CHANGE, deliberate: the two word delimiters are no longer in this list.
  // "hel-lo" and "n't" became legal SHAPES when the entry grammar grew to
  // [a-z]+ ( '|' | '-' )? [a-z]+, so they are asserted below as queries that
  // reach the index and come back with nothing, which is a different fact from
  // being refused. Everything here still stops before the index.
  const std::string accented = std::string("h") + "\xC3" + "\xA9" + "l";
  struct Refusal {
    std::string input;
    std::string outcome;
    std::string decision;
  };
  const Refusal refused[] = {
      {"hel lo", "unsupported-byte", "unsupported-byte"},  // a space is not stripped
      {"hel.", "protected-text", "protected-text"},        // '.' is a technical mark
      {"0abc", "protected-text", "protected-text"},        // a digit is too
      {"a_b", "protected-text", "protected-text"},         // underscore
      {"he=l", "protected-text", "protected-text"},
      {"a@b", "protected-text", "protected-text"},
      {"x/y", "protected-text", "protected-text"},
      {"C:\\tmp", "protected-text", "protected-text"},     // backslash
      {"q?r", "protected-text", "protected-text"},
      {"s&t", "protected-text", "protected-text"},
      {"u#v", "protected-text", "protected-text"},
      {"w%z", "protected-text", "protected-text"},
      {"1+2", "protected-text", "protected-text"},
      {"k:l", "protected-text", "protected-text"},
      {accented, "non-ascii", "non-ascii"},                // 4 bytes here
      {"hel-lo-x", "illegal-word-form", "illegal-word-form"},   // two hyphens
      {"'hello", "illegal-word-form", "illegal-word-form"},     // leading delimiter
      // NOT here: "hello'" is letters then ONE trailing delimiter, which is the
      // accepted partial state -- asserted as a query in case 24 below.
      {"a'b-c", "illegal-word-form", "illegal-word-form"},      // both kinds at once
  };
  for (const Refusal& c : refused) {
    CompletionResult r = epocanva::en::CompletePrefix(*lexicon, c.input, 8);
    Check(StatusName(r.outcome), c.outcome, "case 7: refused [" + c.input + "]");
    Check(StatusName(r.token.decision), c.decision,
          "case 7: refused [" + c.input + "] token decision");
    Check(Join(r.completions), "", "case 7: refused returns nothing");
    // A refusal must not be recorded as a word-list problem either.
    CheckTrue(!r.lookup_performed,
              "case 7: refused [" + c.input + "] did not reach the index",
              r.lookup_performed ? "yes" : "no");
  }
  // A mixed-case shape is a policy refusal, and it is a DIFFERENT bucket from
  // "not letters": both produce only the original text, but only one of them means
  // "the input contains something that is not a letter at all".
  for (const std::string& mixed : {std::string("HeL"), std::string("hEL"),
                                   std::string("iPh"), std::string("hELLO")}) {
    CompletionResult r = epocanva::en::CompletePrefix(*lexicon, mixed, 8);
    Check(StatusName(r.case_style), "mixed-case",
          "case 7: mixed [" + mixed + "]");
    Check(Join(r.completions), "", "case 7: mixed returns nothing");
    CheckTrue(!r.lookup_performed, "case 7: mixed is not looked up",
              r.lookup_performed ? "yes" : "no");
  }

  const std::string at_limit(kMaxPrefixBytes, 'a');
  const std::string over_limit = at_limit + "b";
  Check(StatusName(epocanva::en::CompletePrefix(*lexicon, at_limit, 8).case_style),
        "lowercase", "case 7: 64-byte prefix accepted");
  CompletionResult rlong = epocanva::en::CompletePrefix(*lexicon, over_limit, 8);
  Check(StatusName(rlong.case_style), "input-too-long",
        "case 7: 65-byte prefix rejected rather than truncated");
  Check(Join(rlong.completions), "",
        "case 7: over-long prefix matched no truncated form of itself");
}

// --- 8. k == 0 and the k cap -----------------------------------------------
void TestKLimits() {
  std::cout << "[en-core] case 8: k = 0 and the k cap" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 8: load", StatusName(info.status), "ok");
    return;
  }
  CompletionResult r0 = epocanva::en::CompletePrefix(*lexicon, "a", 0);
  Check(StatusName(r0.case_style), "lowercase",
        "case 8: k=0 still classifies the prefix");
  Check(Join(r0.completions), "", "case 8: k=0 returns nothing");
  // match_count survives, so "no matches" and "matches suppressed by k" are
  // distinguishable by a caller. 7 here because the core already dropped the row
  // that repeats the input ("a"), which is not a k-related suppression.
  CheckNum(static_cast<long long>(r0.match_count), 7,
           "case 8: k=0 still reports the available matches");
  CheckNum(static_cast<long long>(
               epocanva::en::CompletePrefix(*lexicon, "a", 1000).completions.size()),
           7, "case 8: asking beyond the range returns the range");

  // The cap needs more matches than kMaxCompletions, so build 70 rows that all
  // start with 'z' and carry distinct weights 1..70. Expectations below are
  // hand-derived: the 64 heaviest of 1..70 are 70 down to 7, leaving 1..6 out.
  std::string big;
  for (int i = 0; i < 70; ++i) {
    std::string word = "z";
    word += static_cast<char>('a' + (i / 26) % 26);
    word += static_cast<char>('a' + i % 26);
    word += static_cast<char>('a' + (i * 7) % 26);
    big += word + "\t" + std::to_string(i + 1) + "\n";
  }
  LexiconLoadInfo big_info;
  std::unique_ptr<EnglishLexicon> big_lexicon =
      EnglishLexicon::FromText(big, &big_info);
  if (!big_lexicon) {
    ReportFailure("case 8: 70-row load", StatusName(big_info.status), "ok");
    return;
  }
  CompletionResult rcap = epocanva::en::CompletePrefix(*big_lexicon, "z", 1000);
  CheckNum(static_cast<long long>(rcap.match_count), 70, "case 8: 70 matches");
  CheckNum(static_cast<long long>(rcap.completions.size()),
           static_cast<long long>(kMaxCompletions), "case 8: result capped");
  if (!rcap.completions.empty()) {
    CheckNum(static_cast<long long>(rcap.completions.front().weight), 70,
             "case 8: heaviest kept");
    CheckNum(static_cast<long long>(rcap.completions.back().weight), 7,
             "case 8: cut after the 64th heaviest");
  }
  bool monotone = true;
  std::size_t smallest = 0;
  for (std::size_t i = 0; i < rcap.completions.size(); ++i) {
    if (i > 0 && rcap.completions[i - 1].weight < rcap.completions[i].weight) {
      monotone = false;
    }
    if (i == 0 || rcap.completions[i].weight < smallest) {
      smallest = rcap.completions[i].weight;
    }
  }
  CheckTrue(monotone, "case 8: weights non-increasing", monotone ? "yes" : "no");
  CheckTrue(smallest >= 7, "case 8: nothing lighter than the cut point kept",
            std::to_string(smallest));
}

// --- 9. duplicate and malformed data, each condition named separately ------
void TestMalformedData() {
  std::cout << "[en-core] case 9: malformed lexicon data" << std::endl;
  ExpectLoad("a\t5\n", LexiconStatus::kOk, 0, 1, "single row");
  ExpectLoad(kEnglishFixture, LexiconStatus::kOk, 0, 14, "fixture");
  ExpectLoad("a\t5", LexiconStatus::kOk, 0, 1, "no final newline");
  ExpectLoad("a\t5\r\nb\t7\r\n", LexiconStatus::kOk, 0, 2, "CRLF");
  ExpectLoad("a\t5\r\nb\t7\n", LexiconStatus::kOk, 0, 2, "mixed LF and CRLF");
  ExpectLoad("a\t0\n", LexiconStatus::kOk, 0, 1, "bare zero weight legal");
  ExpectLoad("a\t4294967295\n", LexiconStatus::kOk, 0, 1, "uint32 max");
  ExpectLoad("a\t5\nab\t5\n", LexiconStatus::kOk, 0, 2, "distinct words");
  ExpectLoad("", LexiconStatus::kEmptyFile, 0, 0, "zero bytes");
  ExpectLoad("\n", LexiconStatus::kEmptyFile, 0, 0, "newline only");
  ExpectLoad("a\t5\n\n", LexiconStatus::kBlankLine, 2, 0,
             "two final newlines");
  ExpectLoad("\na\t5\n", LexiconStatus::kBlankLine, 1, 0, "leading blank line");
  ExpectLoad("a\t5\nb\n", LexiconStatus::kMissingField, 2, 0, "no tab");
  ExpectLoad("a\t5\n\t7\n", LexiconStatus::kIllegalWord, 2, 0, "empty word");
  ExpectLoad("a\t5\t6\n", LexiconStatus::kTooManyFields, 1, 0, "two tabs");
  ExpectLoad("Ab\t5\n", LexiconStatus::kIllegalWord, 1, 0, "uppercase byte");
  ExpectLoad("a b\t5\n", LexiconStatus::kIllegalWord, 1, 0, "space in word");
  // P2.3: a hyphen or apostrophe BETWEEN letters is now a legal entry, so the row
  // that P2.2 rejected is asserted as accepted here -- and the ways a delimiter
  // can be misplaced are asserted as their own status, distinct from the ways a
  // BYTE can be wrong.
  ExpectLoad("a-b\t5\n", LexiconStatus::kOk, 0, 1, "dash between letters");
  ExpectLoad("a'b\t5\n", LexiconStatus::kOk, 0, 1, "apostrophe between letters");
  ExpectLoad("don't\t5\n", LexiconStatus::kOk, 0, 1, "the brief's own example");
  ExpectLoad("'ab\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "leading apostrophe");
  ExpectLoad("ab'\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "trailing apostrophe");
  ExpectLoad("-ab\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "leading hyphen");
  ExpectLoad("ab-\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "trailing hyphen");
  ExpectLoad("a--b\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "doubled hyphen");
  ExpectLoad("a'b'c\t5\n", LexiconStatus::kIllegalWordForm, 1, 0, "two apostrophes");
  ExpectLoad("rock'n'roll\t5\n", LexiconStatus::kIllegalWordForm, 1, 0,
             "rock'n'roll is a multi-delimiter token");
  ExpectLoad("a'b-c\t5\n", LexiconStatus::kIllegalWordForm, 1, 0,
             "apostrophe and hyphen in one word");
  ExpectLoad("a-b-c\t5\n", LexiconStatus::kIllegalWordForm, 1, 0,
             "three parts is not a word form");
  ExpectLoad("a1b\t5\n", LexiconStatus::kIllegalWord, 1, 0, "digit in word");
  ExpectLoad("A-b\t5\n", LexiconStatus::kIllegalWord, 1, 0,
             "uppercase next to a delimiter is still an uppercase byte");
  ExpectLoad("a-b\t5\n", LexiconStatus::kOk, 0, 1, "dash between letters again");
  ExpectLoad(std::string("\xC3\xA9") + "\t5\n", LexiconStatus::kIllegalWord, 1,
             0, "non-ASCII word");
  ExpectLoad("a\t\n", LexiconStatus::kIllegalWeight, 1, 0, "empty weight");
  ExpectLoad("a\tx\n", LexiconStatus::kIllegalWeight, 1, 0, "non-digit weight");
  ExpectLoad("a\t+5\n", LexiconStatus::kIllegalWeight, 1, 0, "signed weight");
  ExpectLoad("a\t-5\n", LexiconStatus::kIllegalWeight, 1, 0, "negative weight");
  ExpectLoad("a\t 5\n", LexiconStatus::kIllegalWeight, 1, 0,
             "leading space in weight");
  ExpectLoad("a\t5 \n", LexiconStatus::kIllegalWeight, 1, 0,
             "trailing space in weight");
  ExpectLoad("a\t05\n", LexiconStatus::kIllegalWeight, 1, 0,
             "leading zero in weight");
  ExpectLoad("a\t4294967296\n", LexiconStatus::kWeightOverflow, 1, 0,
             "uint32 max + 1");
  ExpectLoad("a\t99999999999999999999999\n", LexiconStatus::kWeightOverflow, 1,
             0, "absurd weight");
  ExpectLoad("a\t5\na\t7\n", LexiconStatus::kDuplicateWord, 2, 0,
             "duplicate reported at its own line");
  ExpectLoad("ab\t5\nab\t5\n", LexiconStatus::kDuplicateWord, 2, 0,
             "identical duplicate row");
  ExpectLoad((std::string(kMaxWordBytes, 'm') + "\t3\n").c_str(),
             LexiconStatus::kOk, 0, 1, "word at the 64-byte limit");
  ExpectLoad((std::string(kMaxWordBytes, 'm') + "n\t3\n").c_str(),
             LexiconStatus::kWordTooLong, 1, 0,
             "65-byte word rejected, not truncated");
}

// --- P2.2: ASCII case classification and display mapping -------------------------
//
// Expectations are hand-derived from kEnglishFixture (the 14 rows above). For the
// prefix "ab" the range is ab(9) abc(7) able(7) about(2) above(9); ordered by
// weight descending then canonical ascending that is
//     ab, above, abc, able, about
// because ab and above both carry 9 and "ab" < "above". Dropping the row whose
// DISPLAY text repeats the input is what leaves the four-item lists below.
void TestCasePreservation() {
  std::cout << "[en-core] case 14-19: ASCII case mapping" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 14: load", StatusName(info.status), "ok");
    return;
  }

  // 1. classification priority, including the single-letter ambiguity.
  const std::pair<const char*, const char*> styles[] = {
      {"", "empty"},
      {"a", "lowercase"},
      {"A", "initial-capital"},      // a single capital is NOT "all uppercase"
      {"ab", "lowercase"},
      {"AB", "uppercase"},
      {"Ab", "initial-capital"},
      {"aB", "mixed-case"},
      {"HeL", "mixed-case"},
      {"hEL", "mixed-case"},
      {"iPh", "mixed-case"},
      {"hello", "lowercase"},
      {"HELLO", "uppercase"},
      {"Hello", "initial-capital"},
      {"hELLO", "mixed-case"},
      {"Hel1o", "not-alphabetic"},   // a digit outranks any casing judgement
      {"ab ", "not-alphabetic"},
      // P2.3: the two ASCII word delimiters are CASE-TRANSPARENT, so a-b and a'b
      // are ordinary lowercase words. A byte that is neither a letter nor one of
      // them still outranks every casing judgement.
      {"a-b", "lowercase"},
      {"a_b", "not-alphabetic"},
      {"a'b", "lowercase"},
      {"don't", "lowercase"},
      {"Don't", "initial-capital"},
      {"DON'T", "uppercase"},
      {"don'T", "mixed-case"},      // a capital after a delimiter is not a shape
      {"co-operate", "lowercase"},
      {"Co-operate", "initial-capital"},   // only the WHOLE word's first letter
      {"CO-OPERATE", "uppercase"},
      {"Co-Operate", "mixed-case"},        // never auto-capitalised after '-'
      {"don'", "lowercase"},        // trailing delimiter: a queryable prefix shape
      {"Don'", "initial-capital"},
      {"'", "not-alphabetic"},      // no letter at all: not a casing of anything
      {"-", "not-alphabetic"},
  };
  for (const std::pair<const char*, const char*>& c : styles) {
    Check(std::string(epocanva::en::CaseStyleName(
              epocanva::en::ClassifyAsciiCase(c.first))),
          c.second, std::string("case 14: classify [") + c.first + "]");
  }
  // 4. the length limit, in both a lowercase and an uppercase shape, and evaluated
  //    before content so an over-long input is never judged from a truncation.
  Check(std::string(epocanva::en::CaseStyleName(epocanva::en::ClassifyAsciiCase(
            std::string(kMaxCaseInputBytes, 'z')))),
        "lowercase", "case 14: 64-byte lowercase at the limit");
  Check(std::string(epocanva::en::CaseStyleName(epocanva::en::ClassifyAsciiCase(
            std::string(kMaxCaseInputBytes, 'Z')))),
        "uppercase", "case 14: 64-byte uppercase at the limit");
  Check(std::string(epocanva::en::CaseStyleName(epocanva::en::ClassifyAsciiCase(
            std::string(kMaxCaseInputBytes + 1, 'z')))),
        "input-too-long", "case 14: 65-byte lowercase refused");
  Check(std::string(epocanva::en::CaseStyleName(epocanva::en::ClassifyAsciiCase(
            std::string(kMaxCaseInputBytes + 1, 'Z')))),
        "input-too-long", "case 14: 65-byte uppercase refused");
  // 6. a non-ASCII byte is reported, never folded or dropped.
  const std::string non_ascii = std::string("a") + "\xC3" + "\xA9" + "b";
  Check(std::string(epocanva::en::CaseStyleName(
            epocanva::en::ClassifyAsciiCase(non_ascii))),
        "not-alphabetic", "case 14: non-ASCII byte is not folded");

  // 1, 8, 10. the three completable shapes, de-duplicated on DISPLAY text.
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "ab", 8),
                {"above", "abc", "able", "about"},
                "case 15: lowercase ab keeps the canonical spelling");
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "Ab", 8),
                {"Above", "Abc", "Able", "About"},
                "case 15: initial capital maps the first byte only");
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "AB", 8),
                {"ABOVE", "ABC", "ABLE", "ABOUT"},
                "case 15: uppercase maps every byte");
  CompletionResult r_ab = epocanva::en::CompletePrefix(*lexicon, "ab", 8);
  CompletionResult r_Ab = epocanva::en::CompletePrefix(*lexicon, "Ab", 8);
  CompletionResult r_AB = epocanva::en::CompletePrefix(*lexicon, "AB", 8);
  CheckNum(static_cast<long long>(r_ab.deduplicated), 1, "case 16: ab dropped 1");
  CheckNum(static_cast<long long>(r_Ab.deduplicated), 1, "case 16: Ab dropped 1");
  CheckNum(static_cast<long long>(r_AB.deduplicated), 1, "case 16: AB dropped 1");
  CheckNum(static_cast<long long>(r_ab.match_count), 4,
           "case 16: match_count is after the drop, not before");
  CheckTrue(r_ab.lookup_performed && r_Ab.lookup_performed && r_AB.lookup_performed,
            "case 15: all three shapes reached the index", "no");
  Check(r_AB.completions[0].canonical, "above",
        "case 15: canonical stays the dictionary row while display is mapped");

  // 2. a single letter completes as its own shape.
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "A", 8),
                {"Ab", "Above", "Abc", "Able", "Apple", "Aa", "About"},
                "case 17: single 'A' completes initial-capital");
  CheckWordList(epocanva::en::CompletePrefix(*lexicon, "a", 8),
                {"ab", "above", "abc", "able", "apple", "aa", "about"},
                "case 17: single 'a' completes lowercase");

  // 3. mixed case completes nothing, with its own reason.
  for (const std::string& mixed : {std::string("aB"), std::string("ABc")}) {
    CompletionResult r = epocanva::en::CompletePrefix(*lexicon, mixed, 8);
    Check(StatusName(r.case_style), "mixed-case",
          "case 18: mixed [" + mixed + "] not completed");
    Check(Join(r.completions), "", "case 18: mixed yields nothing");
    CheckTrue(!r.lookup_performed, "case 18: mixed did not reach the index",
              r.lookup_performed ? "yes" : "no");
  }

  // 9. case never re-ranks: the canonical order is identical across shapes.
  std::string lower_order;
  std::string upper_order;
  for (const Completion& c : r_ab.completions) {
    lower_order += c.canonical + ",";
  }
  for (const Completion& c : r_AB.completions) {
    upper_order += c.canonical + ",";
  }
  Check(upper_order, lower_order,
        "case 19: canonical ranking unaffected by case");
}

// 7. the raw input is never rewritten by a query. Verified by comparing the
// caller's buffer byte-for-byte before and after, for every shape.
void TestRawInputImmutable() {
  std::cout << "[en-core] case 20: raw input is never modified" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    ReportFailure("case 20: load", StatusName(info.status), "ok");
    return;
  }
  const char* inputs[] = {"ab",    "Ab",     "AB",     "aB",    "A",
                          "hello", "HELLO",  "HeLLo",  "he1lo", "a\xC3\xA9" "b"};
  for (const char* raw : inputs) {
    const std::string original(raw);
    std::string buffer(original);
    const CompletionResult r = epocanva::en::CompletePrefix(*lexicon, buffer, 8);
    CheckNum(static_cast<long long>(buffer.size()),
             static_cast<long long>(original.size()),
             std::string("case 20: length preserved for [") + raw + "]");
    Check(buffer, original,
          std::string("case 20: bytes preserved for [") + raw + "]");
    // A completable shape must produce a display text different from the caller's
    // own bytes: that is what shows the mapping ran on a copy.
    if (r.lookup_performed && !r.completions.empty()) {
      CheckTrue(r.completions[0].display != buffer,
                "case 20: display is derived, not written into the input [" +
                    std::string(raw) + "]",
                r.completions[0].display);
    }
  }
}

// --- 29 / 30 / 31. the P2.5 space policy, testable without an engine -----------
//
// These three groups are the reason the policy lives in
// engine/languages/en/english_space_policy.h rather than inside the Processor: the
// rules "which modifier makes a space somebody else's key", "which composition
// state is case A versus case B" and "which case commits text at all" are decided
// here, and the engine adapter that acts on them cannot be unit-tested in this
// project at all (it needs a live rime::Context).

const char* SpaceEventName(epocanva::en::SpaceKeyEvent kind) {
  switch (kind) {
    case epocanva::en::SpaceKeyEvent::kNotSpace:
      return "not-space";
    case epocanva::en::SpaceKeyEvent::kKeyRelease:
      return "key-release";
    case epocanva::en::SpaceKeyEvent::kBlockedByModifier:
      return "blocked-by-modifier";
    case epocanva::en::SpaceKeyEvent::kPlainSpace:
      return "plain-space";
  }
  return "out-of-range";  // unreachable for the four declared values; not a fifth
}

const char* SpaceCaseName(epocanva::en::SpaceCase selected_case) {
  switch (selected_case) {
    case epocanva::en::SpaceCase::kNotApplicable:
      return "not-applicable";
    case epocanva::en::SpaceCase::kCommitTokenThenSpace:
      return "commit-token-then-space";
    case epocanva::en::SpaceCase::kCommitSelectionThenSpace:
      return "commit-selection-then-space";
    case epocanva::en::SpaceCase::kSpaceOnly:
      return "space-only";
  }
  return "out-of-range";
}

void TestSpaceKeyClassification() {
  std::cout << "[en-core] case 29: which space events the policy acts on"
            << std::endl;
  using epocanva::en::ClassifySpaceKey;
  using epocanva::en::SpaceKeyEvent;
  struct Row {
    int keysym;
    unsigned mask;
    const char* wanted;
    const char* why;
  };
  // Modifier bits written as the numbers third_party/librime/src/rime/key_table.h
  // documents, so a row reads as "shift+space" rather than as "3".
  const unsigned shift = epocanva::en::kShiftMaskBit;
  const unsigned lock = epocanva::en::kLockMaskBit;
  const unsigned ctrl = epocanva::en::kControlMaskBit;
  const unsigned alt = epocanva::en::kAltMaskBit;
  const unsigned super = epocanva::en::kSuperMaskBit;
  const unsigned release = epocanva::en::kReleaseMaskBit;
  const unsigned forward = 1u << 25;  // kForwardMask: an engine-internal marker
  const int space = epocanva::en::kSpaceKeysym;
  const Row rows[] = {
      {space, 0u, "plain-space", "bare space"},
      {space, shift, "plain-space", "shift+space is still a space"},
      {space, lock, "plain-space", "caps-lock state must not change the commit"},
      {space, shift | lock, "plain-space", "shift+lock"},
      {space, forward, "plain-space", "an internal marker is not a user modifier"},
      {space, ctrl, "blocked-by-modifier", "Ctrl+Space belongs to the host"},
      {space, alt, "blocked-by-modifier", "Alt+Space belongs to the host"},
      {space, super, "blocked-by-modifier", "Super+Space is a system binding"},
      {space, ctrl | shift, "blocked-by-modifier", "Ctrl+Shift+Space"},
      {space, ctrl | alt | super, "blocked-by-modifier", "all three at once"},
      {space, release, "key-release", "a release is not a press"},
      {space, release | shift, "key-release", "release wins over shift"},
      {space, release | ctrl, "key-release",
       "release is the more specific reason to refuse"},
      {'h', 0u, "not-space", "a letter is the speller's business"},
      {'h', ctrl, "not-space", "Ctrl+H is not a space whatever the keysym"},
      {0xff0d, 0u, "not-space", "Return"},
      {0xff1b, 0u, "not-space", "Escape"},
      {0xff08, 0u, "not-space", "BackSpace"},
      {0x00a0, 0u, "not-space",
       "U+00A0 NO-BREAK SPACE is not XK_space and is not handled here"},
      {0x0000, 0u, "not-space", "no keysym"},
      {0x2000, 0u, "not-space",
       "0x2000 is a keysym with the same low byte; it is not the space bar"},
  };
  int rows_checked = 0;
  for (const Row& row : rows) {
    ++rows_checked;
    Check(SpaceEventName(ClassifySpaceKey(row.keysym, row.mask)), row.wanted,
          std::string("case 29: ") + row.why);
  }
  CheckNum(static_cast<long long>(rows_checked), 21,
           "case 29: every row in the table was checked");
}

void TestSpaceCaseTable() {
  std::cout << "[en-core] case 30: state to case, and case to actions"
            << std::endl;
  using epocanva::en::CaseCommitsComposition;
  using epocanva::en::CaseEmitsSpace;
  using epocanva::en::DecideSpaceCase;
  using epocanva::en::SpaceCase;
  // All four state combinations, including the one that cannot occur. Answering it
  // is deliberate: a caller that misreads the engine must get "space only" rather
  // than an uninitialised decision, and this row is what pins that.
  Check(SpaceCaseName(DecideSpaceCase(false, false)), "space-only",
        "case 30: nothing composing");
  Check(SpaceCaseName(DecideSpaceCase(false, true)), "space-only",
        "case 30: impossible pair answered without inventing a commit");
  Check(SpaceCaseName(DecideSpaceCase(true, false)), "commit-token-then-space",
        "case 30: composing, nothing explicitly selected (rule A)");
  Check(SpaceCaseName(DecideSpaceCase(true, true)),
        "commit-selection-then-space",
        "case 30: composing, a selection waits to be committed (rule B)");

  struct Action {
    SpaceCase selected_case;
    const char* name;
    bool commits;
    bool emits_space;
  };
  const Action actions[] = {
      {SpaceCase::kNotApplicable, "not-applicable", false, false},
      {SpaceCase::kCommitTokenThenSpace, "commit-token-then-space", true, true},
      {SpaceCase::kCommitSelectionThenSpace, "commit-selection-then-space", true,
       true},
      {SpaceCase::kSpaceOnly, "space-only", false, true},
  };
  int emitted_total = 0;
  int commit_total = 0;
  for (const Action& a : actions) {
    CheckTrue(CaseCommitsComposition(a.selected_case) == a.commits,
              std::string("case 30: commit flag for ") + a.name,
              CaseCommitsComposition(a.selected_case) ? "true" : "false");
    CheckTrue(CaseEmitsSpace(a.selected_case) == a.emits_space,
              std::string("case 30: space flag for ") + a.name,
              CaseEmitsSpace(a.selected_case) ? "true" : "false");
    if (a.emits_space) {
      ++emitted_total;
    }
    if (a.commits) {
      ++commit_total;
    }
  }
  // The arithmetic the engine-side counters are expected to reproduce: three of
  // the four cases emit exactly one space, and the two that commit text are a
  // strict subset of those.
  CheckNum(emitted_total, 3, "case 30: cases that emit a space");
  CheckNum(commit_total, 2, "case 30: cases that commit text");
  CheckTrue(commit_total <= emitted_total,
            "case 30: committing cases are a subset of emitting cases",
            std::to_string(commit_total) + "/" + std::to_string(emitted_total));
}

void TestSpacePolicyConstants() {
  std::cout << "[en-core] case 31: the policy's constants still match the pinned "
               "source"
            << std::endl;
  // Pinned against third_party/librime/src/rime/key_table.h:14-38 at
  // 33e78140250125871856cdc5b42ddc6a5fcd3cd4. These are transcribed numbers, not
  // included values, so a future upstream change has to fail HERE rather than
  // quietly changing which keys the processor accepts.
  CheckNum(static_cast<long long>(epocanva::en::kShiftMaskBit), 1, "shift bit");
  CheckNum(static_cast<long long>(epocanva::en::kLockMaskBit), 2, "lock bit");
  CheckNum(static_cast<long long>(epocanva::en::kControlMaskBit), 4, "control bit");
  CheckNum(static_cast<long long>(epocanva::en::kAltMaskBit), 8, "alt bit");
  CheckNum(static_cast<long long>(epocanva::en::kSuperMaskBit), 67108864,
           "super bit (1 << 26)");
  CheckNum(static_cast<long long>(epocanva::en::kReleaseMaskBit), 1073741824,
           "release bit (1 << 30)");
  CheckNum(epocanva::en::kSpaceKeysym, 32, "XK_space");
  CheckNum(epocanva::en::kSpaceCharacter, 32,
           "the emitted U+0020 has the same code point as the keysym");
  const unsigned blocking = epocanva::en::kSpaceBlockingMask;
  CheckNum(static_cast<long long>(blocking), 67108876LL,
           "case 31: blocking mask is exactly ctrl|alt|super (4+8+1<<26)");
  CheckTrue((blocking & epocanva::en::kShiftMaskBit) == 0u,
            "case 31: shift is not a blocking modifier",
            std::to_string(blocking));
  CheckTrue((blocking & epocanva::en::kLockMaskBit) == 0u,
            "case 31: lock is not a blocking modifier", std::to_string(blocking));
  CheckTrue((blocking & epocanva::en::kReleaseMaskBit) == 0u,
            "case 31: release is refused by its own rule, not by this mask",
            std::to_string(blocking));
}


// --- P2.6 spelling suggestions (cases 32 to 38) --------------------------------
//
// The fixture below is CONTROLLED TEST DATA, not English. The rows exist so each of
// the four allowed edits has an isolated example, so a weight tie and a budget cut
// are observable, and so every suppression gate has an input that reaches it and no
// other. Weights are chosen to make ranking assertable; they are not frequencies and
// this list claims nothing about language quality.
//
// EXPECTATIONS ARE LITERALS, derived by build/p26-verify/derive_expectations.py --
// an independently written pair-distance walker over these rows, not a call into
// spelling_suggester.cc. Case 37 then re-derives every answer at run time with the
// reference below and requires set AND order equality, so a candidate that is
// missing, invented, mis-ranked or duplicated cannot survive.
const char* const kSpellingFixture =
    "hello\t9\n"
    "help\t8\n"
    "hollo\t7\n"
    "hallo\t6\n"
    "hillo\t6\n"
    "hella\t6\n"
    "world\t7\n"
    "works\t6\n"
    "word\t4\n"
    "test\t5\n"
    "tests\t3\n"
    "hell\t4\n"
    "helium\t3\n"
    "don't\t5\n"
    "co-operate\t2\n"
    "abcdefghijklmnopqrstuvwxyzabcdef\t1\n";

// 32 letters, the exact upper edge of the window, and a row in the fixture.
const char* const kLongWord = "abcdefghijklmnopqrstuvwxyzabcdef";

std::shared_ptr<const EnglishLexicon> SpellingLexicon(LexiconLoadInfo* info) {
  return std::shared_ptr<const EnglishLexicon>(
      EnglishLexicon::FromText(kSpellingFixture, info).release());
}

std::string LowerCopy(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return value;
}

const char* EditShort(SpellingEdit edit) {
  switch (edit) {
    case SpellingEdit::kDeletion:
      return "dele";
    case SpellingEdit::kInsertion:
      return "inse";
    case SpellingEdit::kSubstitution:
      return "subs";
    case SpellingEdit::kTransposition:
      return "tran";
    case SpellingEdit::kNone:
      break;
  }
  return "none";
}

// One candidate rendered the way every expectation here is written:
// "canonical(weight,edit)", single-space joined.
std::string ShowSuggestions(const std::vector<SpellingSuggestion>& list) {
  std::string out;
  for (const SpellingSuggestion& suggestion : list) {
    if (!out.empty()) {
      out += " ";
    }
    out += suggestion.canonical + "(" + std::to_string(suggestion.weight) + "," +
           EditShort(suggestion.edit) + ")";
  }
  return out;
}

// --- the independent reference (case 37) ---------------------------------------
//
// Answers "is `word` reachable from `key` by exactly one allowed edit" by walking the
// two words. spelling_suggester.cc goes the other way: it generates variants and asks
// the index. Two different algorithms agreeing on a set is evidence; one calling the
// other would not be.
//
// This is NOT a general Damerau-Levenshtein and must not be described as one: it
// recognises the four operations of this round only, so a deletion plus an insertion
// (distance 2 in that metric) is simply not a case it considers.
bool ReferenceOneEditApart(const std::string& key, const std::string& word,
                           const char** operation) {
  if (key == word) {
    return false;
  }
  const long diff = static_cast<long>(word.size()) - static_cast<long>(key.size());
  if (diff == 0) {
    std::size_t first = 0;
    std::size_t mismatches = 0;
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
      *operation = "subs";
      return true;
    }
    if (mismatches == 2 && first + 1 < key.size() &&
        key[first] == word[first + 1] && key[first + 1] == word[first]) {
      *operation = "tran";
      return true;
    }
    return false;
  }
  if (diff == -1 || diff == 1) {
    const std::string& longer = (diff == -1) ? key : word;
    const std::string& shorter = (diff == -1) ? word : key;
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
    if (matched == shorter.size() && skipped == 1) {
      *operation = (diff == -1) ? "dele" : "inse";
      return true;
    }
  }
  return false;
}

// True for the three case shapes this round accepts: all lowercase, all uppercase,
// or an initial capital with every later letter lowercase. Written here rather than
// borrowed from english_case.h so the reference really is a second opinion.
bool ReferenceIsAcceptedShape(const std::string& text) {
  bool any_letter = false;
  bool any_upper = false;
  bool any_lower = false;
  bool seen_letter = false;
  bool upper_after_first = false;
  for (const char c : text) {
    const bool upper = (c >= 'A' && c <= 'Z');
    const bool lower = (c >= 'a' && c <= 'z');
    if (!upper && !lower) {
      return false;  // non-letter bytes are another gate's business, not this one
    }
    any_letter = true;
    any_upper = any_upper || upper;
    any_lower = any_lower || lower;
    if (upper && seen_letter) {
      upper_after_first = true;
    }
    seen_letter = true;
  }
  if (!any_letter) {
    return false;
  }
  if (any_upper && any_lower) {
    return !upper_after_first;
  }
  return true;
}

// The reference's whole answer, as "outcome|candidate list", derived by scanning the
// fixture rows. Delimited rows are never suggestion targets in this round.
std::string ReferenceAnswer(const EnglishLexicon& lexicon,
                            const std::string& raw_input, std::size_t k) {
  const std::string key = LowerCopy(raw_input);
  if (key.size() > kMaxSuggestionWordBytes) {
    return "too-long|";
  }
  if (key.size() < kMinSuggestionWordBytes) {
    return "too-short|";
  }
  for (const char c : key) {
    if (c < 'a' || c > 'z') {
      return "shape-gate|";  // delimiter, digit, mark or non-ASCII byte
    }
  }
  if (!ReferenceIsAcceptedShape(raw_input)) {
    return "mixed-case|";
  }
  for (const LexiconEntry& entry : lexicon.entries()) {
    if (entry.word == key) {
      return "word-in-lexicon|";
    }
  }
  // Gate 9 recomputed from the rows: a longer row that starts with this key would
  // have been offered as an ordinary completion.
  for (const LexiconEntry& entry : lexicon.entries()) {
    if (entry.word.size() > key.size() &&
        entry.word.compare(0, key.size(), key) == 0) {
      return "completion-available|";
    }
  }
  std::vector<std::pair<const LexiconEntry*, const char*>> hits;
  for (const LexiconEntry& entry : lexicon.entries()) {
    if (entry.word.find('\'') != std::string::npos ||
        entry.word.find('-') != std::string::npos) {
      continue;
    }
    const char* operation = "none";
    if (ReferenceOneEditApart(key, entry.word, &operation)) {
      hits.emplace_back(&entry, operation);
    }
  }
  if (hits.empty()) {
    return "no-candidate|";
  }
  std::sort(hits.begin(), hits.end(),
            [](const std::pair<const LexiconEntry*, const char*>& a,
               const std::pair<const LexiconEntry*, const char*>& b) {
              return epocanva::en::CompletionBefore(*a.first, *b.first);
            });
  std::string list;
  for (std::size_t i = 0; i < hits.size() && i < k; ++i) {
    if (!list.empty()) {
      list += " ";
    }
    list += hits[i].first->word + "(" + std::to_string(hits[i].first->weight) +
           "," + hits[i].second + ")";
  }
  return "suggested|" + list;
}

// The implementation's answer in the same shape. The completion count is obtained the
// way the engine obtains it -- from CompletePrefix on the same input -- because gate
// 9 consumes that number; the reference derives it independently from the rows.
std::string ImplementationAnswer(const EnglishLexicon& lexicon,
                                 const std::string& raw_input, std::size_t k) {
  const CompletionResult completion =
      epocanva::en::CompletePrefix(lexicon, raw_input, kMaxCompletions);
  const SpellingResult spelling = epocanva::en::SuggestSpelling(
      &lexicon, raw_input, completion.completions.size(), k, /*enabled=*/true);
  std::string outcome = epocanva::en::SpellingOutcomeName(spelling.outcome);
  // The reference folds the byte-class refusals into one name, and treats "" as the
  // too-short case it also is. Everything else must match word for word.
  if (outcome == "protected-input" || outcome == "non-ascii-input" ||
      outcome == "unsupported-byte" || outcome == "delimited-form") {
    outcome = "shape-gate";
  }
  if (outcome == "empty-input") {
    outcome = "too-short";
  }
  return outcome + "|" + ShowSuggestions(spelling.suggestions);
}

void TestSpellingMembershipAndFind() {
  std::cout << "[en-core] case 32: exact membership over the loaded index"
            << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 32: load", StatusName(info.status), "ok");
    return;
  }
  CheckNum(static_cast<long long>(lexicon->size()), 16, "case 32: rows");
  const struct {
    const char* word;
    bool present;
    unsigned long long weight;
  } probes[] = {
      {"hello", true, 9},     {"help", true, 8},       {"hell", true, 4},
      {"word", true, 4},      {"world", true, 7},      {"test", true, 5},
      {"tests", true, 3},     {"helium", true, 3},     {"don't", true, 5},
      {"co-operate", true, 2}, {kLongWord, true, 1},   {"helo", false, 0},
      {"hxllo", false, 0},    {"h", false, 0},         {"helps", false, 0},
      {"HELLO", false, 0},    {"", false, 0},
      {"abcdefghijklmnopqrstuvwxyzabcdefg", false, 0},
  };
  int probes_checked = 0;
  for (const auto& probe : probes) {
    ++probes_checked;
    const LexiconEntry* found = lexicon->Find(probe.word);
    CheckTrue((found != nullptr) == probe.present,
              std::string("case 32: Find(") + probe.word + ") presence",
              found ? "found" : "absent");
    if (found == nullptr) {
      continue;
    }
    CheckNum(static_cast<long long>(found->weight),
             static_cast<long long>(probe.weight),
             std::string("case 32: Find(") + probe.word + ") weight");
    CheckTrue(found->word == probe.word,
              std::string("case 32: Find(") + probe.word + ") returned itself",
              found->word);
    // The pointer must address the caller-visible vector: the suggester's identity
    // arithmetic for duplicate hits depends on that, not on a copy.
    CheckTrue(found >= lexicon->entries().data() &&
                  found < lexicon->entries().data() + lexicon->size(),
              std::string("case 32: Find(") + probe.word + ") hit is in entries()",
              "");
  }
  CheckNum(probes_checked, 18, "case 32: probe rows checked");
}

void TestSpellingFourEdits() {
  std::cout << "[en-core] case 33: one example per allowed edit" << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 33: load", StatusName(info.status), "ok");
    return;
  }
  const struct {
    const char* input;
    const char* wanted;
    const char* why;
  } rows[] = {
      {"hcello", "hello(9,dele)", "delete one letter: h-c-ello"},
      {"hellop", "hello(9,dele)", "delete the trailing p"},
      {"wrld", "world(7,inse)", "insert the missing o"},
      {"helo", "hello(9,inse) help(8,subs) hell(4,subs)",
       "one insertion and the two substitutions that also reach a row"},
      {"hxllo", "hello(9,subs) hollo(7,subs) hallo(6,subs)",
       "substitution only, with the weight-6 tie broken by ASCII order"},
      {"wirld", "world(7,subs)", "substitution only"},
      {"ehllo", "hello(9,tran)", "transposition of the first pair"},
      {"wrod", "word(4,tran)", "transposition inside the word"},
      {"wordd", "world(7,subs) word(4,dele)",
       "two operations; ranked by weight, not by which operation"},
      {"testz", "test(5,dele) tests(3,subs)",
       "both a deletion and a substitution reach a row"},
  };
  for (const auto& row : rows) {
    const SpellingResult result = epocanva::en::SuggestSpelling(
        &*lexicon, row.input, /*completion_candidates=*/0, /*k=*/3,
        /*enabled=*/true);
    Check(epocanva::en::SpellingOutcomeName(result.outcome), "suggested",
          std::string("case 33: outcome for ") + row.input + " (" + row.why + ")");
    Check(ShowSuggestions(result.suggestions), row.wanted,
          std::string("case 33: candidates for ") + row.input);
  }
}

void TestSpellingRankingBudgetAndBounds() {
  std::cout << "[en-core] case 34: ordering, k, and the two length bounds"
            << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 34: load", StatusName(info.status), "ok");
    return;
  }
  // "hxllo" reaches four rows: hello(9), hollo(7) and the weight-6 pair hallo/hillo.
  // k decides how many come back; the ORDER must not depend on k.
  const struct {
    std::size_t k;
    std::size_t wanted_count;
    const char* wanted;
  } budget[] = {
      {0, 0, ""},
      {1, 1, "hello(9,subs)"},
      {2, 2, "hello(9,subs) hollo(7,subs)"},
      {3, 3, "hello(9,subs) hollo(7,subs) hallo(6,subs)"},
      {7, 4, "hello(9,subs) hollo(7,subs) hallo(6,subs) hillo(6,subs)"},
      {1000, 4, "hello(9,subs) hollo(7,subs) hallo(6,subs) hillo(6,subs)"},
  };
  for (const auto& row : budget) {
    const SpellingResult result =
        epocanva::en::SuggestSpelling(&*lexicon, "hxllo", 0, row.k, true);
    Check(ShowSuggestions(result.suggestions), row.wanted,
          "case 34: k=" + std::to_string(row.k) + " budget");
    CheckNum(static_cast<long long>(result.suggestions.size()),
             static_cast<long long>(row.wanted_count),
             "case 34: k=" + std::to_string(row.k) + " list size");
    // k == 0 still reports that the search ran and hit: an empty list from a
    // successful search is a different fact from "nothing within one edit".
    Check(epocanva::en::SpellingOutcomeName(result.outcome), "suggested",
          "case 34: k=" + std::to_string(row.k) + " outcome");
    CheckNum(static_cast<long long>(result.words_in_lexicon), 4,
             "case 34: k=" + std::to_string(row.k) + " still saw four hits");
  }
  const std::string three = "hel";
  const std::string four = "helo";
  const std::string at32(kLongWord);
  CheckNum(static_cast<long long>(kMinSuggestionWordBytes), 4,
           "case 34: min edge is 4");
  CheckNum(static_cast<long long>(kMaxSuggestionWordBytes), 32,
           "case 34: max edge is 32");
  CheckNum(static_cast<long long>(at32.size()), 32,
           "case 34: the fixture's 32-letter row really is 32");
  Check(epocanva::en::SpellingOutcomeName(
            epocanva::en::SuggestSpelling(&*lexicon, three, 0, 3, true).outcome),
        "too-short", "case 34: 3 letters is below the window");
  Check(epocanva::en::SpellingOutcomeName(
            epocanva::en::SuggestSpelling(&*lexicon, four, 0, 3, true).outcome),
        "suggested", "case 34: exactly 4 letters is inside it");
  const std::string sub32 = at32.substr(0, 31) + "q";
  Check(epocanva::en::SpellingOutcomeName(
            epocanva::en::SuggestSpelling(&*lexicon, sub32, 0, 3, true).outcome),
        "suggested", "case 34: exactly 32 letters still searches");
  Check(ShowSuggestions(epocanva::en::SuggestSpelling(&*lexicon, sub32, 0, 3, true)
                            .suggestions),
        "abcdefghijklmnopqrstuvwxyzabcdef(1,subs)",
        "case 34: the 32-letter row is one substitution away");
  const std::string over33 = at32 + "z";
  const SpellingResult too_long =
      epocanva::en::SuggestSpelling(&*lexicon, over33, 0, 3, true);
  Check(epocanva::en::SpellingOutcomeName(too_long.outcome), "too-long",
        "case 34: 33 letters is refused outright");
  // A refusal is never a truncation in disguise: the length is reported as typed.
  CheckNum(static_cast<long long>(too_long.length_bytes), 33,
           "case 34: the over-long input keeps its real length");
  CheckNum(static_cast<long long>(too_long.variants_generated), 0,
           "case 34: and no variant was generated for it");
  // Work per keystroke at the widest edge, as arithmetic rather than as prose:
  // 32 deletions + 33*26 insertions + 32*25 substitutions + 31 transpositions.
  const std::string wide32 = at32.substr(0, 30) + "qz";
  const SpellingResult big =
      epocanva::en::SuggestSpelling(&*lexicon, wide32, 0, 3, true);
  CheckNum(static_cast<long long>(wide32.size()), 32,
           "case 34: the wide probe is 32 letters");
  CheckNum(static_cast<long long>(big.variants_generated), 1721,
           "case 34: variant arithmetic at the 32-letter edge");
  CheckNum(static_cast<long long>(big.lexicon_lookups),
           static_cast<long long>(big.variants_generated),
           "case 34: exactly one lookup per generated variant");
  Check(epocanva::en::SpellingOutcomeName(big.outcome), "no-candidate",
        "case 34: and that 32-letter probe reaches no row");
}

void TestSpellingDuplicatesAndSelfEdits() {
  std::cout << "[en-core] case 35: a candidate produced twice appears once"
            << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 35: load", StatusName(info.status), "ok");
    return;
  }
  // "helo" reaches "hello" by inserting 'l' at position 2 AND at position 3.
  // "helllo" reaches it by deleting any of three 'l's.
  const SpellingResult helo =
      epocanva::en::SuggestSpelling(&*lexicon, "helo", 0, 3, true);
  CheckNum(static_cast<long long>(helo.words_in_lexicon), 4,
           "case 35: helo hit the index four times (hello twice)");
  CheckNum(static_cast<long long>(helo.duplicates_dropped), 1,
           "case 35: helo drops one duplicate");
  Check(ShowSuggestions(helo.suggestions), "hello(9,inse) help(8,subs) hell(4,subs)",
        "case 35: helo lists hello once");
  const SpellingResult helllo =
      epocanva::en::SuggestSpelling(&*lexicon, "helllo", 0, 3, true);
  CheckNum(static_cast<long long>(helllo.words_in_lexicon), 3,
           "case 35: helllo hits hello three times by deletion");
  CheckNum(static_cast<long long>(helllo.duplicates_dropped), 2,
           "case 35: helllo keeps one of them");
  Check(ShowSuggestions(helllo.suggestions), "hello(9,dele)",
        "case 35: helllo yields hello once");
  // 6 deletions + 7*26 insertions + 6*25 substitutions + 3 transpositions: the two
  // equal-adjacent pairs are skipped, because swapping them returns the input.
  CheckNum(static_cast<long long>(helllo.variants_generated), 341,
           "case 35: helllo variant count");
  const char* const probes[] = {"helo", "helllo", "hxllo", "testz", "wordd", "wrld"};
  for (const char* input : probes) {
    const SpellingResult result =
        epocanva::en::SuggestSpelling(&*lexicon, input, 0, 8, true);
    for (const SpellingSuggestion& suggestion : result.suggestions) {
      CheckTrue(suggestion.canonical != LowerCopy(input),
                std::string("case 35: no suggestion equals the input for ") + input,
                suggestion.canonical);
      CheckTrue(suggestion.display != input,
                std::string("case 35: display is not the typed text for ") + input,
                suggestion.display);
      // Plain letter words only: the two delimited rows must never come back.
      CheckTrue(suggestion.canonical.find('\'') == std::string::npos &&
                    suggestion.canonical.find('-') == std::string::npos,
                "case 35: no delimited row is ever suggested",
                suggestion.canonical);
    }
  }
}

void TestSpellingGateTable() {
  std::cout << "[en-core] case 36: every empty answer has its own name" << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 36: load", StatusName(info.status), "ok");
    return;
  }
  const struct {
    const char* input;
    std::size_t completions;
    bool enabled;
    const char* wanted;
    const char* why;
  } rows[] = {
      {"helo", 0, false, "disabled", "the switch is off; nothing else is examined"},
      {"helo", 0, true, "suggested", "control: same input, switch on"},
      {"", 0, true, "empty-input", "nothing typed"},
      {"hel", 0, true, "too-short", "three letters"},
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, true, "too-long",
       "33 letters, and the input is not cut down first"},
      {"h3llo", 0, true, "protected-input", "a digit"},
      {"h.elo", 0, true, "protected-input", "a full stop"},
      {"user@example.com", 0, true, "protected-input", "an e-mail address"},
      {"C:\\tmp\\file.txt", 0, true, "protected-input", "a Windows path"},
      {"https://example.com/a?x=1", 0, true, "protected-input", "a URL"},
      {"hgllo", 1, true, "completion-available",
       "the caller reports a completion, so no correction is offered"},
      {"heli", 1, true, "completion-available",
       "helium completes; help and hell are one edit away and still suppressed"},
      {"help", 0, true, "word-in-lexicon", "already correct, and nothing completes it"},
      {"hello", 0, true, "word-in-lexicon", "already correct"},
      {"test", 0, true, "word-in-lexicon", "already correct"},
      {"tests", 0, true, "word-in-lexicon", "already correct, and the longer form"},
      {"HELLO", 0, true, "word-in-lexicon",
       "uppercase of a row is still already correct"},
      {"don't", 0, true, "delimited-form", "apostrophe: outside this round"},
      {"co-op", 0, true, "delimited-form", "hyphen: outside this round"},
      {"co-operate", 0, true, "delimited-form",
       "a correct delimited row is refused on FORM, before membership is considered"},
      {"hEllo", 0, true, "mixed-case", "letters, unrecognised casing"},
      {"HEllo", 0, true, "mixed-case", "a second capital after the first letter"},
      {"zxqj", 0, true, "no-candidate", "searched; nothing within one edit"},
      {"hxlo", 0, true, "no-candidate",
       "hello is two edits away and must not be reached"},
  };
  int checked = 0;
  for (const auto& row : rows) {
    ++checked;
    const SpellingResult result = epocanva::en::SuggestSpelling(
        &*lexicon, row.input, row.completions, 3, row.enabled);
    Check(epocanva::en::SpellingOutcomeName(result.outcome), row.wanted,
          std::string("case 36: ") + row.input + " (" + row.why + ")");
    const bool ran = std::string(row.wanted) == "suggested" ||
                     std::string(row.wanted) == "no-candidate";
    CheckNum(result.search_ran ? 1 : 0, ran ? 1 : 0,
             std::string("case 36: search_ran for ") + row.input);
    if (!ran) {
      CheckNum(static_cast<long long>(result.suggestions.size()), 0,
               std::string("case 36: nothing offered when ") + row.wanted);
      CheckNum(static_cast<long long>(result.lexicon_lookups), 0,
               std::string("case 36: no lookups when ") + row.wanted);
      CheckNum(static_cast<long long>(result.variants_generated), 0,
               std::string("case 36: no variants when ") + row.wanted);
    }
  }
  CheckNum(checked, 24, "case 36: gate rows checked");
  // The two answers a caller must never be able to confuse with "nothing found":
  // a disabled feature and a missing word list, both before any input rule.
  const SpellingResult no_index =
      epocanva::en::SuggestSpelling(nullptr, "helo", 0, 3, true);
  Check(epocanva::en::SpellingOutcomeName(no_index.outcome), "lexicon-unavailable",
        "case 36: a missing word list is not 'nothing found'");
  CheckNum(static_cast<long long>(no_index.lexicon_lookups), 0,
           "case 36: and no lookup happened");
  const SpellingResult off_and_broken =
      epocanva::en::SuggestSpelling(nullptr, "helo", 0, 3, false);
  Check(epocanva::en::SpellingOutcomeName(off_and_broken.outcome), "disabled",
        "case 36: the switch is evaluated before the index");
}

void TestSpellingAgainstIndependentReference() {
  std::cout << "[en-core] case 37: an independent reference agrees on set and order"
            << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 37: load", StatusName(info.status), "ok");
    return;
  }
  const char* const inputs[] = {
      "helo", "hxllo", "hcello", "ehllo", "helpo", "helllo", "wrod", "wirld",
      "wrld", "wordd", "hxlo", "heli", "help", "hello", "test", "tests", "zxqj",
      "hellop", "HELO", "Helo", "HELLO", "hEllo", "HEllo", "hlo", "hel", "h3llo",
      "h.elo", "don't", "co-operate", "co-op", "user@example.com", kLongWord,
      "abcdefghijklmnopqrstuvwxyzabcdeq", "abcdefghijklmnopqrstuvwxyzabcdefz",
      "testz", "hollo", "hallo", "hillo", "hella", "works", "world", "helium",
      "hell", "aehllo", "helol", "worlld", "tets", "wold", "wordl", "helo o",
  };
  std::size_t agreed = 0;
  std::size_t total = 0;
  for (const char* input : inputs) {
    ++total;
    const std::string wanted = ReferenceAnswer(*lexicon, input, 3);
    const std::string got = ImplementationAnswer(*lexicon, input, 3);
    if (wanted == got) {
      ++agreed;
      continue;
    }
    ReportFailure(std::string("case 37: reference disagrees for ") + input, got,
                  wanted);
  }
  CheckNum(static_cast<long long>(total), 50, "case 37: inputs compared");
  CheckNum(static_cast<long long>(agreed), static_cast<long long>(total),
           "case 37: every input agrees with the independent reference");
}

void TestSpellingRawInputImmutableAndCase() {
  std::cout << "[en-core] case 38: the caller's bytes are never rewritten"
            << std::endl;
  LexiconLoadInfo info;
  const std::shared_ptr<const EnglishLexicon> lexicon = SpellingLexicon(&info);
  if (!lexicon) {
    ReportFailure("case 38: load", StatusName(info.status), "ok");
    return;
  }
  const char* const inputs[] = {"helo", "HELO", "Helo", "hxllo", "don't", "h3llo",
                               "hEllo", "wordd"};
  for (const char* input : inputs) {
    std::string buffer(input);
    const SpellingResult result =
        epocanva::en::SuggestSpelling(&*lexicon, buffer, 0, 3, true);
    Check(buffer, std::string(input),
          "case 38: bytes preserved for [" + std::string(input) + "]");
    CheckNum(static_cast<long long>(result.length_bytes),
             static_cast<long long>(std::string(input).size()),
             "case 38: reported length for [" + std::string(input) + "]");
    for (const SpellingSuggestion& suggestion : result.suggestions) {
      CheckTrue(suggestion.display != suggestion.canonical ||
                    LowerCopy(std::string(input)) == input,
                "case 38: display follows the input's shape",
                suggestion.canonical + " / " + suggestion.display);
      CheckTrue(suggestion.canonical == LowerCopy(suggestion.canonical),
                "case 38: canonical stays the stored lowercase form",
                suggestion.canonical);
    }
  }
  const struct {
    const char* input;
    const char* wanted_display;
  } shapes[] = {
      {"helo", "hello help hell"},
      {"HELO", "HELLO HELP HELL"},
      {"Helo", "Hello Help Hell"},
  };
  for (const auto& shape : shapes) {
    const SpellingResult result =
        epocanva::en::SuggestSpelling(&*lexicon, shape.input, 0, 3, true);
    std::string display;
    for (const SpellingSuggestion& suggestion : result.suggestions) {
      if (!display.empty()) {
        display += " ";
      }
      display += suggestion.display;
    }
    Check(display, shape.wanted_display,
          std::string("case 38: display rendering for ") + shape.input);
  }
}

// --- 10 + 11. repeatability, and independence from row order ---------------
void TestDeterminism() {
  std::cout << "[en-core] case 10/11: repeat and row-order independence"
            << std::endl;
  LexiconLoadInfo info_a, info_b;
  std::unique_ptr<EnglishLexicon> lex_a =
      EnglishLexicon::FromText(kEnglishFixture, &info_a);
  std::unique_ptr<EnglishLexicon> lex_b =
      EnglishLexicon::FromText(kShuffledFixture, &info_b);
  if (!lex_a || !lex_b) {
    ReportFailure("case 11: loads",
                  StatusName(info_a.status) + "/" + StatusName(info_b.status),
                  "ok/ok");
    return;
  }
  Check(JoinEntries(lex_a->entries()), JoinEntries(lex_b->entries()),
        "case 11: index identical after row shuffle");
  const std::string prefixes[] = {"a", "ab", "b", "ban", "z", "q", ""};
  for (const std::string& p : prefixes) {
    CompletionResult ra = epocanva::en::CompletePrefix(*lex_a, p, 8);
    CompletionResult rb = epocanva::en::CompletePrefix(*lex_b, p, 8);
    Check(Join(ra.completions), Join(rb.completions),
          "case 11: query [" + p + "] unaffected by row order");
    CheckNum(static_cast<long long>(rb.match_count),
             static_cast<long long>(ra.match_count),
             "case 11: query [" + p + "] match count");
  }
  // Same query on the same index, five times, byte-identical.
  CompletionResult first = epocanva::en::CompletePrefix(*lex_a, "a", 8);
  for (int i = 0; i < 5; ++i) {
    CompletionResult again = epocanva::en::CompletePrefix(*lex_a, "a", 8);
    Check(Join(again.completions), Join(first.completions),
          "case 10: repeat run " + std::to_string(i));
  }
}


// --- P2.3: token structure, delimited entries and the protection boundary -------
//
// The word list these groups query, written as literals. The index order and every
// expected list below was derived by hand from these 10 rows using one fact: in
// std::string's ascending BYTE order, U+0027 (0x27) and U+002D (0x2D) both sit
// BELOW every lowercase letter (0x61). So "co-operate" sorts before "coopt" and
// "don't" before "done", and a prefix bound that assumed letters-only continuations
// is exactly the thing at risk.
//
//   index  word         weight
//   0      a            1
//   1      co-operate   6
//   2      coopt        7
//   3      do           3
//   4      does         5
//   5      doesn't      4
//   6      don't        8
//   7      done         6
//   8      re-enter     3
//   9      reed         9
const char* const kTokenFixture =
    "do\t3\n"
    "does\t5\n"
    "done\t6\n"
    "don't\t8\n"
    "doesn't\t4\n"
    "co-operate\t6\n"
    "coopt\t7\n"
    "re-enter\t3\n"
    "reed\t9\n"
    "a\t1\n";

const char* const kTokenFixtureIndex =
    "a/1,co-operate/6,coopt/7,do/3,does/5,doesn't/4,don't/8,done/6,"
    "re-enter/3,reed/9";

std::unique_ptr<EnglishLexicon> LoadTokenFixture(const char* label,
                                                 const char* text) {
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon = EnglishLexicon::FromText(text, &info);
  Check(StatusName(info.status), "ok", std::string(label) + " loads");
  CheckNum(static_cast<long long>(lexicon ? static_cast<long long>(lexicon->size())
                                          : 0),
           10, std::string(label) + " entry count");
  return lexicon;
}

// Group 21: the complete-entry grammar, over the brief's own list (section 7).
void TestDictionaryEntryForms() {
  std::cout << "[en-core] case 21: dictionary entry forms" << std::endl;
  struct Row {
    const char* word;
    bool wanted;
  };
  const Row rows[] = {
      {"hello", true},
      {"don't", true},
      {"co-operate", true},
      {"a", true},
      {"a'b", true},
      {"a-b", true},
      {"'hello", false},   // leading apostrophe
      {"hello'", false},   // trailing apostrophe: a prefix state, never an entry
      {"-word", false},    // leading hyphen
      {"word-", false},    // trailing hyphen
      {"rock'n'roll", false},  // three parts
      {"co--operate", false},  // doubled delimiter
      {"don't-x", false},      // both kinds in one word
      {"a-b-c", false},        // three parts with hyphens
      {"h3llo", false},        // digit
      {"Hello", false},        // uppercase: the list stores canonical lowercase
      {"h\xE9" "llo", false},  // non-ASCII byte
      {"", false},             // empty
  };
  for (const Row& r : rows) {
    CheckTrue(epocanva::en::IsDictionaryEntryForm(r.word) == r.wanted,
              std::string("case 21: entry form [") + r.word + "]",
              r.wanted ? "must be accepted" : "must be rejected");
  }
  // The limit applies to the WHOLE entry, delimiter included: 62 letters + ' + 'a'
  // is 64 code units and legal; one letter more is 65 and is not.
  const std::string at_limit = std::string(62, 'a') + "'a";
  CheckNum(static_cast<long long>(at_limit.size()),
           static_cast<long long>(kMaxWordBytes),
           "case 21: at-limit entry is exactly 64 code units");
  CheckTrue(epocanva::en::IsDictionaryEntryForm(at_limit),
            "case 21: 64-byte delimited entry accepted", "rejected");
  const std::string over = std::string(63, 'a') + "'a";
  CheckTrue(!epocanva::en::IsDictionaryEntryForm(over),
            "case 21: 65-byte delimited entry refused", "accepted");
  // And the loader reports the over-long row as its own status, not as a form
  // error -- the two have different fixes.
  ExpectLoad(at_limit + "\t5\n", LexiconStatus::kOk, 0, 1, "64-byte delimited entry");
  ExpectLoad(over + "\t5\n", LexiconStatus::kWordTooLong, 1, 0,
             "65-byte delimited entry is too long, not malformed");
}

// Group 22: the structural decision table. For every input the answer carries the
// decision, the delimiter form, which byte class stopped it and where, and the TWO
// SEPARATE questions the brief asks: is this a complete entry, and is this a
// queryable prefix. For "don'" only the second is true.
void TestTokenStructureTable() {
  std::cout << "[en-core] case 22: token structure table" << std::endl;
  struct Row {
    const char* input;
    const char* decision;
    const char* delimiter;
    const char* stop_class;
    long long stop_offset;       // -1 means npos
    long long delimiter_offset;  // -1 means npos
    bool full_entry;
    bool query_prefix;
  };
  const std::string accented = std::string("h") + "\xC3" + "\xA9" + "l";
  const Row rows[] = {
      {"hel", "query", "no-delimiter", "none", -1, -1, true, true},
      {"don'", "query", "trailing-delimiter", "none", -1, 3, false, true},
      {"co-", "query", "trailing-delimiter", "none", -1, 2, false, true},
      {"a'", "query", "trailing-delimiter", "none", -1, 1, false, true},
      {"word-", "query", "trailing-delimiter", "none", -1, 4, false, true},
      {"hello'", "query", "trailing-delimiter", "none", -1, 5, false, true},
      {"don't", "query", "internal-delimiter", "none", -1, 3, true, true},
      {"co-operate", "query", "internal-delimiter", "none", -1, 2, true, true},
      {"Don'", "query", "trailing-delimiter", "none", -1, 3, false, true},
      {"DON'T", "query", "internal-delimiter", "none", -1, 3, true, true},
      {"'hello", "illegal-word-form", "leading-delimiter", "misplaced-delimiter", 0, 0,
       false, false},
      {"-word", "illegal-word-form", "leading-delimiter", "misplaced-delimiter", 0, 0,
       false, false},
      {"rock'n'roll", "illegal-word-form", "more-than-one-delimiter", "extra-delimiter",
       6, 4, false, false},
      {"co--operate", "illegal-word-form", "more-than-one-delimiter", "extra-delimiter",
       3, 2, false, false},
      {"a'b-c", "illegal-word-form", "both-delimiter-kinds", "mixed-delimiter-kinds", 3,
       1, false, false},
      {"a-b-c", "illegal-word-form", "more-than-one-delimiter", "extra-delimiter", 3, 1,
       false, false},
      {"he2", "protected-text", "no-delimiter", "digit", 2, -1, false, false},
      {"v1.2", "protected-text", "no-delimiter", "digit", 1, -1, false, false},
      {"user@example.com", "protected-text", "no-delimiter", "punctuation-mark", 4,
       -1, false, false},
      {"foo_bar", "protected-text", "no-delimiter", "punctuation-mark", 3, -1,
       false, false},
      {"https://example.com/a?x=1&y=2#z", "protected-text", "no-delimiter",
       "punctuation-mark", 5, -1, false, false},
      {"C:\\tmp\\file.txt", "protected-text", "no-delimiter", "punctuation-mark", 1,
       -1, false, false},
      {"a:b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a/b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a\\b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a?b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a&b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a=b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a#b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a%b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a+b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"a.b", "protected-text", "no-delimiter", "punctuation-mark", 1, -1, false,
       false},
      {"hel lo", "unsupported-byte", "no-delimiter", "other-ascii-byte", 3, -1,
       false, false},
      {"a;b", "unsupported-byte", "no-delimiter", "other-ascii-byte", 1, -1, false,
       false},
      {"a\"b", "unsupported-byte", "no-delimiter", "other-ascii-byte", 1, -1, false,
       false},
      {accented.c_str(), "non-ascii", "no-delimiter", "non-ascii-byte", 1, -1,
       false, false},
      {"", "empty", "no-delimiter", "no-bytes-typed", -1, -1, false, false},
  };
  for (const Row& r : rows) {
    const std::string text = r.input;
    const TokenAnalysis a = epocanva::en::AnalyzeEnglishToken(text);
    const std::string tag = "case 22: [" + text + "]";
    Check(StatusName(a.decision), r.decision, tag + " decision");
    Check(StatusName(a.delimiter), r.delimiter, tag + " delimiter form");
    Check(StatusName(a.stop_class), r.stop_class, tag + " stop class");
    CheckNum(static_cast<long long>(a.stop_offset == std::string_view::npos
                                        ? -1
                                        : a.stop_offset),
             r.stop_offset, tag + " stop offset");
    CheckNum(static_cast<long long>(a.delimiter_offset == std::string_view::npos
                                        ? -1
                                        : a.delimiter_offset),
             r.delimiter_offset, tag + " delimiter offset");
    CheckNum(static_cast<long long>(a.length_bytes),
             static_cast<long long>(text.size()), tag + " length in code units");
    CheckTrue(a.is_full_entry_form == r.full_entry, tag + " complete-entry answer",
              r.full_entry ? "not complete" : "claimed complete");
    CheckTrue(a.is_query_prefix_form == r.query_prefix, tag + " query-prefix answer",
              r.query_prefix ? "not queryable" : "claimed queryable");
  }

  // The length rule is evaluated BEFORE content, at the exact boundary, and an
  // over-long input is refused whole rather than truncated.
  {
    const std::string sixty_four(64, 'a');
    const std::string sixty_five(65, 'a');
    const TokenAnalysis at = epocanva::en::AnalyzeEnglishToken(sixty_four);
    Check(StatusName(at.decision), "query", "case 22: 64-byte input queryable");
    CheckNum(static_cast<long long>(at.length_bytes), 64,
             "case 22: 64-byte length reported whole");
    const TokenAnalysis over = epocanva::en::AnalyzeEnglishToken(sixty_five);
    Check(StatusName(over.decision), "input-too-long",
          "case 22: 65-byte input refused");
    Check(StatusName(over.stop_class), "length",
          "case 22: the refusal names the length, not a byte");
    CheckNum(static_cast<long long>(over.length_bytes), 65,
             "case 22: 65-byte length reported whole, not cut to 64");
    // A digit 70 bytes in is still a LENGTH refusal: an over-long input must never
    // be classified from a 64-byte prefix of itself.
    const TokenAnalysis late =
        epocanva::en::AnalyzeEnglishToken(std::string(70, 'a') + "2");
    Check(StatusName(late.decision), "input-too-long",
          "case 22: over-long input with a late digit is still too long");
  }

  // Delimiters are transparent to the length rule but not to the form rule.
  {
    const std::string delimited = std::string(30, 'a') + "'" + std::string(32, 'b');
    const TokenAnalysis a = epocanva::en::AnalyzeEnglishToken(delimited);
    CheckNum(static_cast<long long>(a.length_bytes), 63,
             "case 22: delimited at-limit sample length");
    Check(StatusName(a.decision), "query", "case 22: 63-byte delimited queryable");
    Check(StatusName(a.delimiter), "internal-delimiter",
          "case 22: 63-byte delimiter form");
    CheckNum(static_cast<long long>(a.delimiter_offset), 30,
             "case 22: 63-byte delimiter offset");
  }
}

// Group 23: the index range, asserted as INDICES against the hand-written order
// above, in both directions. P2.1 bounded the range with key + '{', which was
// correct only while every continuation byte sat below '{'.
void TestPrefixRanges() {
  std::cout << "[en-core] case 23: prefix range boundaries" << std::endl;
  std::unique_ptr<EnglishLexicon> lexicon =
      LoadTokenFixture("case 23", kTokenFixture);
  if (!lexicon) {
    return;
  }
  const std::vector<LexiconEntry>& entries = lexicon->entries();
  Check(JoinEntries(entries), kTokenFixtureIndex,
        "case 23: index order puts both delimiters below every letter");
  struct Row {
    const char* key;
    long long first;
    long long last;
  };
  const Row rows[] = {
      {"do", 3, 8},      // do, does, doesn't, don't, done
      {"don", 6, 8},     // don't, done
      {"don'", 6, 7},    // don't ONLY -- the bound must not leak "done"
      {"does", 4, 6},    // does, doesn't
      {"doesn'", 5, 6},  // doesn't only
      {"co", 1, 3},      // co-operate, coopt
      {"co-", 1, 2},     // co-operate ONLY -- must not leak "coopt"
      {"re", 8, 10},     // re-enter, reed
      {"re-", 8, 9},     // re-enter only
      {"a", 0, 1},       // a
      {"reed", 9, 10},   // reed itself
      {"x", 10, 10},     // nothing at all
  };
  for (const Row& r : rows) {
    const std::string key = r.key;
    const std::size_t first = epocanva::en::PrefixRangeStart(entries, key);
    const std::size_t last = epocanva::en::PrefixRangeEnd(entries, key);
    CheckNum(static_cast<long long>(first), r.first,
             "case 23: [" + key + "] range start");
    CheckNum(static_cast<long long>(last), r.last,
             "case 23: [" + key + "] range end (exclusive)");
    // Both halves of the proof, checked against the data rather than assumed:
    // everything inside the range starts with the key, and neither neighbour does.
    for (std::size_t i = first; i < last; ++i) {
      CheckTrue(entries[i].word.compare(0, key.size(), key) == 0,
                "case 23: [" + key + "] entry " + std::to_string(i) +
                    " inside the range really has the prefix",
                entries[i].word);
    }
    CheckTrue(first == 0 ||
                  entries[first - 1].word.compare(0, key.size(), key) != 0,
              "case 23: [" + key + "] nothing before the range matches",
              first == 0 ? "begin" : entries[first - 1].word);
    CheckTrue(last == entries.size() ||
                  entries[last].word.compare(0, key.size(), key) != 0,
              "case 23: [" + key + "] nothing after the range matches",
              last == entries.size() ? "end" : entries[last].word);
  }
}

// Group 24/25: completions over delimited rows -- sets, order, dedup, budget,
// protection and the case mapping the brief names.
void TestDelimitedCompletions() {
  std::cout << "[en-core] case 24: delimited completions" << std::endl;
  std::unique_ptr<EnglishLexicon> lexicon =
      LoadTokenFixture("case 24", kTokenFixture);
  if (!lexicon) {
    return;
  }
  struct Row {
    const char* input;
    const char* outcome;
    std::vector<std::string> wanted;
    long long match;
    long long dedup;
    long long range_size;
  };
  const Row rows[] = {
      // 1. regression: plain words keep behaving as in P2.1/P2.2.
      {"do", "completed", {"don't", "done", "does", "doesn't"}, 4, 1, 5},
      {"don", "completed", {"don't", "done"}, 2, 0, 2},
      {"co", "completed", {"coopt", "co-operate"}, 2, 0, 2},
      {"re", "completed", {"reed", "re-enter"}, 2, 0, 2},
      // 2. the two brief-named partials: a trailing delimiter is queryable.
      {"don'", "completed", {"don't"}, 1, 0, 1},
      {"co-", "completed", {"co-operate"}, 1, 0, 1},
      {"doesn'", "completed", {"doesn't"}, 1, 0, 1},
      {"re-", "completed", {"re-enter"}, 1, 0, 1},
      // 3. case mapping across a delimiter.
      {"Don'", "completed", {"Don't"}, 1, 0, 1},
      {"DON'", "completed", {"DON'T"}, 1, 0, 1},
      {"Co-", "completed", {"Co-operate"}, 1, 0, 1},
      {"CO-", "completed", {"CO-OPERATE"}, 1, 0, 1},
      {"Re-", "completed", {"Re-enter"}, 1, 0, 1},
      // 4. a complete word typed out: candidate 0 already IS that text, so the row
      //    is dropped and the honest answer is "no match", not an error.
      {"don't", "no-match", {}, 0, 1, 1},
      {"co-operate", "no-match", {}, 0, 1, 1},
      {"a", "no-match", {}, 0, 1, 1},
      // 5. mixed casing keeps the original only, delimiter or not.
      {"DoN't", "mixed-case", {}, 0, 0, 0},
      {"Co-Operate", "mixed-case", {}, 0, 0, 0},
      {"don'T", "mixed-case", {}, 0, 0, 0},
      // 6. a legal shape the list does not contain is a word-list answer, and it
      //    DID reach the index -- which is how it differs from a refusal.
      {"won'", "no-match", {}, 0, 0, 0},
      {"e-mail", "no-match", {}, 0, 0, 0},
  };
  for (const Row& r : rows) {
    const CompletionResult got = epocanva::en::CompletePrefix(*lexicon, r.input, 8);
    const std::string tag = std::string("case 24: [") + r.input + "]";
    Check(StatusName(got.outcome), r.outcome, tag + " outcome");
    CheckWordList(got, r.wanted, tag + " completions");
    CheckNum(static_cast<long long>(got.match_count), r.match, tag + " match count");
    CheckNum(static_cast<long long>(got.deduplicated), r.dedup, tag + " deduplicated");
    // The invariant P2.3 leans on: matched range = survivors + dropped repeats.
    CheckNum(static_cast<long long>(got.match_count + got.deduplicated), r.range_size,
             tag + " range size = match + dedup");
    // Anything that reached the index says so; a refusal never does.
    const std::string oc(r.outcome);
    CheckTrue(got.lookup_performed == (oc == "completed" || oc == "no-match"),
              tag + " lookup_performed says exactly the two index answers",
              got.lookup_performed ? "yes" : "no");
  }

  // 7. protection: refused inputs never reach the index, and the reason is a byte
  //    class -- never a word-list problem.
  const char* protected_inputs[] = {
      "user@example.com",       "https://example.com/a?x=1&y=2#z",
      "C:\\tmp\\file.txt",      "foo_bar",
      "v1.2",                   "he2",
      "don't2",                 "co-operate.2",
      "a@b",                    "a.b",
      "a:b",                    "a/b",
      "a\\b",                   "a?b",
      "a&b",                    "a=b",
      "a#b",                    "a%b",
      "a+b",
  };
  for (const char* in : protected_inputs) {
    const CompletionResult got = epocanva::en::CompletePrefix(*lexicon, in, 8);
    const std::string tag = std::string("case 24: protected [") + in + "]";
    Check(StatusName(got.outcome), "protected-text", tag + " outcome");
    CheckTrue(!got.lookup_performed, tag + " not looked up",
              got.lookup_performed ? "yes" : "no");
    CheckWordList(got, {}, tag + " offers no completion");
    // A digit and a mark are both "protected", but the stop class says which, so a
    // future change to either set is visible instead of silent.
    const TokenAnalysis a = epocanva::en::AnalyzeEnglishToken(in);
    CheckTrue(a.stop_class == StopClass::kDigit ||
                  a.stop_class == StopClass::kPunctuationMark,
              tag + " stop class is a byte class",
              StatusName(a.stop_class));
  }
  const char* malformed[] = {"'hello",   "-word",     "rock'n'roll",
                             "co--operate", "don't-x", "a-b-c",
                             "a'b-c"};
  for (const char* in : malformed) {
    const CompletionResult got = epocanva::en::CompletePrefix(*lexicon, in, 8);
    const std::string tag = std::string("case 24: malformed [") + in + "]";
    Check(StatusName(got.outcome), "illegal-word-form", tag + " outcome");
    CheckTrue(!got.lookup_performed, tag + " not looked up",
              got.lookup_performed ? "yes" : "no");
    CheckWordList(got, {}, tag + " offers no completion");
  }

  // 8. the budget is unchanged by P2.3: k == 0 is a refusal to SHOW, not a
  //    word-list miss, and a large k is clamped by the matches, never padded.
  {
    const CompletionResult k0 = epocanva::en::CompletePrefix(*lexicon, "do", 0);
    Check(StatusName(k0.outcome), "completed", "case 24: k=0 still found rows");
    CheckWordList(k0, {}, "case 24: k=0 returns no text");
    CheckNum(static_cast<long long>(k0.match_count), 4, "case 24: k=0 match count");
    const CompletionResult all =
        epocanva::en::CompletePrefix(*lexicon, "d", kMaxCompletions + 100);
    CheckWordList(all, {"don't", "done", "does", "doesn't", "do"},
                  "case 24: every row under 'd' in weight order");
    CheckNum(static_cast<long long>(all.completions.size()), 5,
             "case 24: an over-large k is bounded by the matches");
    const CompletionResult one = epocanva::en::CompletePrefix(*lexicon, "do", 1);
    CheckWordList(one, {"don't"}, "case 24: k=1 keeps the heaviest row");
  }

  // 9. the canonical / display split survives a delimiter.
  {
    const CompletionResult got = epocanva::en::CompletePrefix(*lexicon, "Don'", 8);
    CheckNum(static_cast<long long>(got.completions.size()), 1,
             "case 24: Don' yields exactly one completion");
    if (!got.completions.empty()) {
      Check(got.completions[0].canonical, "don't",
            "case 24: canonical stays the dictionary spelling");
      Check(got.completions[0].display, "Don't",
            "case 24: display is the text a selection commits");
      CheckNum(static_cast<long long>(got.completions[0].weight), 8,
               "case 24: weight carried through the mapping");
    }
  }
}

// Group 26: the case layer applied to a canonical word that holds a delimiter.
// The rule is the brief's own: initial_capital capitalises the FIRST letter of the
// whole word and nothing else, so "co-operate" never becomes "Co-Operate", and no
// mapping moves a delimiter.
void TestApplyCaseStyleWithDelimiters() {
  std::cout << "[en-core] case 26: case mapping across a delimiter" << std::endl;
  struct Row {
    const char* canonical;
    CaseStyle style;
    const char* wanted;
  };
  const Row rows[] = {
      {"don't", CaseStyle::kLowercase, "don't"},
      {"don't", CaseStyle::kInitialCapital, "Don't"},
      {"don't", CaseStyle::kUppercase, "DON'T"},
      {"co-operate", CaseStyle::kLowercase, "co-operate"},
      {"co-operate", CaseStyle::kInitialCapital, "Co-operate"},
      {"co-operate", CaseStyle::kUppercase, "CO-OPERATE"},
      {"re-enter", CaseStyle::kInitialCapital, "Re-enter"},
      {"re-enter", CaseStyle::kUppercase, "RE-ENTER"},
      {"hello", CaseStyle::kInitialCapital, "Hello"},
      // A style this layer refuses must NOT invent a casing: the canonical text
      // comes back unchanged, so a wiring bug shows up as a visible duplicate of
      // the dictionary spelling rather than as a third casing nobody typed.
      {"don't", CaseStyle::kMixed, "don't"},
      {"don't", CaseStyle::kNotAlphabetic, "don't"},
      {"don't", CaseStyle::kEmpty, "don't"},
      {"don't", CaseStyle::kTooLong, "don't"},
  };
  for (const Row& r : rows) {
    Check(epocanva::en::ApplyCaseStyle(r.canonical, r.style), r.wanted,
          std::string("case 26: [") + r.canonical + "] as " +
              StatusName(r.style));
  }
  // Positions are counted in code units, not in letters: after either mapping the
  // apostrophe is still at index 3 and the length is still 5.
  const std::string upper =
      epocanva::en::ApplyCaseStyle("don't", CaseStyle::kUppercase);
  CheckNum(static_cast<long long>(upper.find('\'')), 3,
           "case 26: apostrophe stays at code-unit 3 under uppercasing");
  CheckNum(static_cast<long long>(upper.size()), 5,
           "case 26: length preserved by the mapping");
  const std::string initial =
      epocanva::en::ApplyCaseStyle("co-operate", CaseStyle::kInitialCapital);
  CheckNum(static_cast<long long>(initial.find('-')), 2,
           "case 26: hyphen stays at code-unit 2 under initial capitalising");
  CheckTrue(initial[3] == 'o', "case 26: the letter after '-' is NOT capitalised",
            std::string(1, initial[3]));
}

// Group 27: he -> he2 -> he, i.e. an ordinary word, then protected text, then the
// ordinary word again. The answers must come back identical, which is what lets a
// user delete the offending byte and keep going.
void TestProtectionTransition() {
  std::cout << "[en-core] case 27: protected then restored" << std::endl;
  std::unique_ptr<EnglishLexicon> lexicon =
      LoadTokenFixture("case 27", kTokenFixture);
  if (!lexicon) {
    return;
  }
  const CompletionResult before = epocanva::en::CompletePrefix(*lexicon, "do", 8);
  const CompletionResult during =
      epocanva::en::CompletePrefix(*lexicon, "do2", 8);
  const CompletionResult after = epocanva::en::CompletePrefix(*lexicon, "do", 8);
  Check(StatusName(before.outcome), "completed", "case 27: before is completed");
  Check(StatusName(during.outcome), "protected-text",
        "case 27: the middle step is protected");
  CheckTrue(!during.lookup_performed, "case 27: protected middle not looked up",
            during.lookup_performed ? "yes" : "no");
  CheckWordList(after, {"don't", "done", "does", "doesn't"},
                "case 27: restoring the letters restores the same completions");
  CheckNum(static_cast<long long>(after.match_count),
           static_cast<long long>(before.match_count),
           "case 27: restoring the letters restores the match count");
  // The delimiter case too: don' -> don't2 -> don'.
  const CompletionResult p1 = epocanva::en::CompletePrefix(*lexicon, "don'", 8);
  const CompletionResult p2 = epocanva::en::CompletePrefix(*lexicon, "don't2", 8);
  const CompletionResult p3 = epocanva::en::CompletePrefix(*lexicon, "don'", 8);
  Check(StatusName(p2.outcome), "protected-text",
        "case 27: digit after a delimited prefix is protected");
  CheckWordList(p3, {"don't"}, "case 27: delimited prefix restored identically");
  // A protected token that becomes a MALFORMED one and then a good one again:
  // co- -> co-'s (two delimiters) -> co-.
  const CompletionResult m1 = epocanva::en::CompletePrefix(*lexicon, "co-", 8);
  const CompletionResult m2 =
      epocanva::en::CompletePrefix(*lexicon, "co-'s", 8);
  const CompletionResult m3 = epocanva::en::CompletePrefix(*lexicon, "co-", 8);
  Check(StatusName(m2.outcome), "illegal-word-form",
        "case 27: a second delimiter is an illegal form");
  CheckWordList(m3, {"co-operate"}, "case 27: co- restored after the illegal step");
  // Repeats are byte-identical, including across a delimiter.
  const CompletionResult again = epocanva::en::CompletePrefix(*lexicon, "don'", 8);
  CheckWordList(again, {"don't"}, "case 27: repeated query is byte-identical");
  CheckNum(static_cast<long long>(m1.match_count),
           static_cast<long long>(m3.match_count),
           "case 27: match count restored after an illegal step");
}

// Group 28: the raw input is never modified, shortened, truncated or re-cased --
// for the refused shapes too, which is where a "tidy up before querying" bug
// would show.
void TestRawInputImmutableWithTokens() {
  std::cout << "[en-core] case 28: raw input untouched across tokens" << std::endl;
  std::unique_ptr<EnglishLexicon> lexicon =
      LoadTokenFixture("case 28", kTokenFixture);
  if (!lexicon) {
    return;
  }
  const char* inputs[] = {
      "don'",          "Don'",         "DON'",         "don't",       "doesn't",
      "co-",           "Co-",          "co-operate",   "re-enter",    "user@example.com",
      "https://example.com/a?x=1&y=2#z",
                      "C:\\tmp\\file.txt",
                      "foo_bar",       "v1.2",         "he2",         "hel.",
      "rock'n'roll",   "co--operate",  "don't-x",      "'hello",      "a'b-c",
      "a b",           "a;b",          "don't2",
  };
  for (const char* in : inputs) {
    const std::string expected(in);
    // A buffer holding the text, a NUL and a canary: the function is only allowed
    // to read the first expected.size() code units.
    std::string buffer = expected;
    buffer.push_back('\0');
    buffer.append("canary");
    const std::string_view view(buffer.data(), expected.size());
    const CompletionResult got = epocanva::en::CompletePrefix(*lexicon, view, 7);
    const TokenAnalysis a = epocanva::en::AnalyzeEnglishToken(view);
    const std::string tag = "case 28: [" + expected + "]";
    CheckNum(static_cast<long long>(buffer.size()),
             static_cast<long long>(expected.size()) + 7, tag + " buffer size kept");
    Check(std::string(buffer.data(), expected.size()), expected,
          tag + " raw bytes unchanged");
    CheckTrue(buffer[expected.size()] == '\0' &&
                  buffer.substr(expected.size() + 1) == "canary",
              tag + " nothing written past the viewed range", "overwritten");
    CheckNum(static_cast<long long>(a.length_bytes),
             static_cast<long long>(expected.size()),
             tag + " length measured in code units, not truncated to 64");
    // No completion may repeat the input: candidate 0 already is it, which is the
    // whole reason the display-level dedup exists.
    for (const Completion& c : got.completions) {
      CheckTrue(c.display != expected,
                tag + " no completion repeats the raw input", c.display);
    }
  }
}

// --- file loading: absent, unreadable and malformed stay distinguishable ----
void TestFileLoads(const std::filesystem::path& scratch) {
  std::cout << "[en-core] case 12: file loading" << std::endl;
  std::cout << "[en-core] case 12: scratch at        " << scratch.string()
            << std::endl;
  std::error_code ec;
  std::filesystem::create_directories(scratch, ec);
  CheckTrue(!ec, "case 12: scratch directory created", ec ? ec.message() : "ok");

  const std::filesystem::path good = scratch / "lexicon.tsv";
  {
    std::ofstream out(good, std::ios::binary);
    out << kEnglishFixture;
  }
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromFile(good, &info);
  Check(StatusName(info.status), "ok", "case 12: good file");
  CheckNum(static_cast<long long>(lexicon ? lexicon->size() : 0), 14,
           "case 12: good file entries");

  LexiconLoadInfo absent_info;
  std::unique_ptr<EnglishLexicon> absent = EnglishLexicon::FromFile(
      scratch / "absent.tsv", &absent_info);
  Check(StatusName(absent_info.status), "file-not-found",
        "case 12: absent file named as absent");
  CheckTrue(absent == nullptr, "case 12: absent file yields no lexicon",
            absent ? "loaded" : "null");

  // A directory on that path is "unreadable", never "an empty lexicon".
  LexiconLoadInfo dir_info;
  std::unique_ptr<EnglishLexicon> from_dir =
      EnglishLexicon::FromFile(scratch, &dir_info);
  Check(StatusName(dir_info.status), "file-unreadable",
        "case 12: directory not mistaken for an empty lexicon");
  CheckTrue(from_dir == nullptr, "case 12: directory yields no lexicon",
            from_dir ? "loaded" : "null");

  const std::filesystem::path bad = scratch / "bad.tsv";
  {
    std::ofstream out(bad, std::ios::binary);
    out << "hello\t4\nhellp\n";
  }
  LexiconLoadInfo bad_info;
  std::unique_ptr<EnglishLexicon> bad_lexicon =
      EnglishLexicon::FromFile(bad, &bad_info);
  Check(StatusName(bad_info.status), "missing-field", "case 12: bad file status");
  CheckNum(static_cast<long long>(bad_info.line), 2,
           "case 12: bad file line reported");
  CheckNum(static_cast<long long>(bad_info.entry_count), 0,
           "case 12: bad file reports zero entries");
  CheckTrue(bad_lexicon == nullptr, "case 12: bad file yields no lexicon",
            bad_lexicon ? "loaded" : "null");
}

// A test that cannot fail is not a test, so the harness checks itself.
void TestHarnessCanFail() {
  std::cout << "[en-core] case 13: harness self-test" << std::endl;
  const int failed_before = g_failed;
  const int checked_before = g_checked;
  Check("deliberately wrong", "deliberately right",
        "deliberate harness self-test failure (expected)");
  // Captured before any further Check call, because Check itself increments the
  // counter it is being used to verify.
  const int failed_delta = g_failed - failed_before;
  const int checked_delta = g_checked - checked_before;
  // Undo the deliberate failure so the exit code reflects the real cases only.
  --g_failed;
  CheckNum(failed_delta, 1, "harness records a failure");
  CheckNum(checked_delta, 1, "harness counts a check");
}

// --forcefail: run ONE assertion that is false on purpose and require the
// harness to report it. Exit 1 when the harness fails it (the expected result of
// this mode is therefore a non-zero exit); exit 3 if the harness swallowed it,
// which would mean every other case in this file is unproven.
int RunForceFailMode() {
  std::cout << "[en-core] mode forcefail: one intentionally false assertion"
            << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    std::cout << "[en-core] FORCEFAIL SETUP FAIL   fixture did not load"
              << std::endl;
    return 2;
  }
  // The wrong expectation: prefix "a" really returns 8 rows, claimed 9.
  CompletionResult r = epocanva::en::CompletePrefix(*lexicon, "a", 8);
  const int before = g_failed;
  CheckNum(static_cast<long long>(r.completions.size()), 9,
           "intentionally wrong expectation (must be reported)");
  if (g_failed == before) {
    std::cout << "[en-core] FORCEFAIL RESULT           HARNESS BROKEN -- a false "
                 "assertion was not recorded"
              << std::endl;
    return 3;
  }
  std::cout << "[en-core] FORCEFAIL RESULT           FAIL as designed ("
            << g_failed << " recorded)" << std::endl;
  return 1;
}

// --bench: a small timing sample. Explicitly NOT a performance claim: 14 rows
// fit in cache, so this measures the shape of the code on toy data, not a word
// list of production size.
void RunBenchMode() {
  std::cout << "[en-core] mode bench: small-sample timing only" << std::endl;
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromText(kEnglishFixture, &info);
  if (!lexicon) {
    std::cout << "[en-core] BENCH SETUP FAIL         fixture did not load"
              << std::endl;
    return;
  }
  const std::size_t iterations = 200000;
  std::size_t sink = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    sink += epocanva::en::CompletePrefix(*lexicon, "a", 8).completions.size();
  }
  const auto stop = std::chrono::steady_clock::now();
  const double microseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() /
      1000.0;
  std::cout << "[en-core] bench iterations         " << iterations << std::endl;
  std::cout << "[en-core] bench total us           " << microseconds
            << std::endl;
  std::cout << "[en-core] bench us per query       "
            << (microseconds / static_cast<double>(iterations)) << std::endl;
  std::cout << "[en-core] bench sink               " << sink << std::endl;
  std::cout << "[en-core] bench caveat             14-row fixture; not a "
               "production-scale or comparative measurement"
            << std::endl;
}


// --pilot-bench: the bounded P2.6 performance trial on the P2.4 pilot word list.
//
// WHAT THIS MEASURES. The cost of the one-edit suggestion pass over an index that is
// already resident -- 42,356 rows, the largest data set this project has -- per input
// class: a correct word, a prefix that completes, and searched inputs at 4, 5, 8, 10,
// 25 and 32 letters, plus the three suppression classes that must cost nothing. This is
// the ONLY half of the P2.6 cost story that exists. The planned engine half (process_key,
// segmentation, the candidate menu) was registered as --mode pilot-spelling in the
// pre-incident project before any implementation of it existed, and it has never run in
// any green scan; the recovery project neither implements nor registers it. So the two
// are not "reported separately" -- one of them was never measured. See the coverage
// matrix in docs/reports/integration-harness-recovery-status.md.
//
// WHAT IT DOES NOT MEASURE. Not a cold file read, not an engine-level per-key cost under
// a real menu, not a user's machine, not a distribution over real typing, and not a claim
// about any future trie or database. The inputs and their expected properties come from
// build/p26-verify/pick_bench_inputs2.py, which reads the TSV and brute-forces the
// one-edit relation itself -- the code under test is not consulted for an expectation.
struct PilotBenchRow {
  const char* input;
  const char* category;
  // The completion count handed to gate 9. Every row that must reach the edit loop
  // passes 0; the 'completes' row passes 1 because the data scan shows a pilot row
  // really does start with it, so what is timed there is the suppression path rather
  // than an accident of the input.
  std::size_t completion_candidates;
  SpellingOutcome want_outcome;
  // Suggestions this input must yield at k = 3, counted by the scan.
  std::size_t want_suggestions;
};

// The number of variant strings the four operations produce, computed WITHOUT producing
// them. deletions + insertions + substitutions + transpositions, with the two excluded
// cases (a letter substituted by itself, two equal adjacent letters swapped) subtracted
// at generation -- which is why a run of equal letters makes a smaller number and why
// this closed form reproduces it.
std::size_t ExpectedPilotVariants(const std::string& key) {
  std::size_t equal_adjacent = 0;
  for (std::size_t i = 0; i + 1 < key.size(); ++i) {
    if (key[i] == key[i + 1]) {
      ++equal_adjacent;
    }
  }
  const std::size_t n = key.size();
  return n + (n + 1) * 26 + n * 25 + (n >= 1 ? n - 1 - equal_adjacent : 0);
}

double PercentileUs(const std::vector<double>& sorted, double fraction) {
  if (sorted.empty()) {
    return 0.0;
  }
  std::size_t index = static_cast<std::size_t>(fraction * (sorted.size() - 1));
  return sorted[index];
}

int RunPilotBenchMode(const std::filesystem::path& lexicon_path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(lexicon_path, ec) || ec) {
    std::cout << "[en-core] FAIL pilot-bench         word list not found: "
              << lexicon_path.u8string() << std::endl;
    return 2;
  }
  std::cout << "[en-core] mode pilot-bench: P2.6 one-edit cost on pilot data"
            << std::endl;
  LexiconLoadInfo info;
  const auto load_start = std::chrono::steady_clock::now();
  std::unique_ptr<EnglishLexicon> lexicon =
      EnglishLexicon::FromFile(lexicon_path, &info);
  const auto load_stop = std::chrono::steady_clock::now();
  if (!lexicon) {
    std::cout << "[en-core] FAIL pilot-bench         load status "
              << StatusName(info.status) << " at line " << info.line << std::endl;
    return 2;
  }
  CheckNum(static_cast<long long>(info.entry_count), 42356,
          "pilot bench: the generated list is the 42,356-row set");
  const double load_us = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             load_stop - load_start).count() / 1000.0;
  std::cout << "[en-core] pilot-bench index        " << info.entry_count
            << " rows, read and built in " << load_us << " us" << std::endl;

  constexpr std::size_t kRepetitions = 200;
  constexpr std::size_t kBenchK = 3;
  // The budget is a checked bound, not a report: the trial fails if any single call
  // exceeds it. Set from the measured worst case with headroom, so a regression that
  // turns a keystroke into a whole-list scan is caught rather than noticed later.
  constexpr double kBudgetUs = 25000.0;
  // Outcome and count expectations are hand-written from the scan; 'kNoCandidate' rows
  // are the junk inputs with no neighbour at all, which still cost a full search.
  const PilotBenchRow rows[] = {
      {"hello", "correct-word", 1, SpellingOutcome::kWordInLexicon, 0},
      {"electroencephalographic", "correct-word-23", 1,
       SpellingOutcome::kWordInLexicon, 0},
      {"appl", "completion-suppressed", 1,
       SpellingOutcome::kCompletionAvailable, 0},
      {"zqqx", "searched-4", 0, SpellingOutcome::kNoCandidate, 0},
      {"zello", "searched-5", 0, SpellingOutcome::kSuggested, 3},
      {"zqvkxbjw", "searched-8", 0, SpellingOutcome::kNoCandidate, 0},
      {"comzuter", "searched-8-hit", 0, SpellingOutcome::kSuggested, 2},
      {"expezience", "searched-10-hit", 0, SpellingOutcome::kSuggested, 2},
      {"zqelectroencephalographic", "searched-25", 0,
       SpellingOutcome::kNoCandidate, 0},
      {"zqelectroencephalographicaaaaaaa", "searched-32", 0,
       SpellingOutcome::kNoCandidate, 0},
      {"user@example.com", "protected", 0, SpellingOutcome::kProtectedInput, 0},
      {"hEllo", "mixed-case", 0, SpellingOutcome::kMixedCase, 0},
      {"don't", "delimited", 1, SpellingOutcome::kDelimitedForm, 0},
  };

  std::vector<std::string> lines;
  const std::size_t kBenchRows = sizeof(rows) / sizeof(rows[0]);
  double worst_us = 0.0;
  std::string worst_input;
  unsigned long long total_variants = 0;
  unsigned long long total_lookups = 0;
  for (const PilotBenchRow& row : rows) {
    const std::string input(row.input);
    std::vector<double> samples;
    samples.reserve(kRepetitions);
    SpellingResult last;
    for (std::size_t i = 0; i < kRepetitions; ++i) {
      const auto start = std::chrono::steady_clock::now();
      // completion_candidates is passed in, not queried: the bench measures the
      // suggestion pass, and the completion gate is fed the value the scan established.
      last = epocanva::en::SuggestSpelling(lexicon.get(), input,
                                           row.completion_candidates, kBenchK,
                                           /*enabled=*/true);
      const auto stop = std::chrono::steady_clock::now();
      samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            stop - start).count() / 1000.0);
    }
    std::sort(samples.begin(), samples.end());
    const char* outcome = epocanva::en::SpellingOutcomeName(last.outcome);
    Check(outcome, epocanva::en::SpellingOutcomeName(row.want_outcome),
          "pilot bench: outcome for " + input);
    CheckNum(static_cast<long long>(last.suggestions.size()),
             static_cast<long long>(row.want_suggestions),
             "pilot bench: suggestion count for " + input);
    // A refusal must never reach the edit loop; a search must look up exactly the
    // variants it generated. Both halves of that are arithmetic, not timing.
    const long long expected_variants =
        (row.want_outcome == SpellingOutcome::kSuggested ||
         row.want_outcome == SpellingOutcome::kNoCandidate)
            ? static_cast<long long>(ExpectedPilotVariants(input))
            : 0;
    CheckNum(static_cast<long long>(last.variants_generated), expected_variants,
             "pilot bench: variants generated for " + input);
    CheckNum(static_cast<long long>(last.lexicon_lookups),
             static_cast<long long>(last.variants_generated),
             "pilot bench: one index lookup per variant for " + input);
    const double median = PercentileUs(samples, 0.5);
    const double p95 = PercentileUs(samples, 0.95);
    const double maximum = samples.back();
    total_variants += last.variants_generated;
    total_lookups += last.lexicon_lookups;
    if (maximum > worst_us) {
      worst_us = maximum;
      worst_input = input;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%-26s %-18s us min=%7.1f med=%7.1f p95=%7.1f max=%7.1f "
                  "variants=%5llu lookups=%5llu sugg=%zu",
                  row.category, input.c_str(), samples.front(), median, p95, maximum,
                  static_cast<unsigned long long>(last.variants_generated),
                  static_cast<unsigned long long>(last.lexicon_lookups),
                  last.suggestions.size());
    lines.push_back(std::string(buf));
  }
  for (const std::string& line : lines) {
    std::cout << "[en-core] pilot-bench sample       " << line << std::endl;
  }
  std::cout << "[en-core] pilot-bench method       steady_clock, "
            << kRepetitions << " repetitions per input, k=" << kBenchK
            << ", single thread, index resident, no engine" << std::endl;
  std::cout << "[en-core] pilot-bench totals       inputs=" << kBenchRows
            << " variants=" << total_variants << " lookups=" << total_lookups
            << std::endl;
  std::cout << "[en-core] pilot-bench worst        " << worst_us << " us for '"
            << worst_input << "'" << std::endl;
  CheckTrue(worst_us < kBudgetUs, "pilot bench: worst single call under budget",
            std::to_string(worst_us) + " us vs " + std::to_string(kBudgetUs) + " us");
  CheckNum(static_cast<long long>(total_lookups),
           static_cast<long long>(total_variants),
           "pilot bench: lookups equal variants over the whole trial");

  std::cout << "[en-core] assertions           " << g_checked << " checked, "
            << g_failed << " failed" << std::endl;
  if (g_failed != 0) {
    std::cout << "RESULT FAIL" << std::endl;
    return 1;
  }
  std::cout << "RESULT PASS" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // The default scratch location is the OS temporary directory, not a name
  // resolved against the current directory: a developer who runs this executable
  // from the repository root would otherwise create files in the source tree,
  // which AGENTS.md section 10 forbids. CTest additionally pins the working
  // directory to the build tree (tests/english_core/CMakeLists.txt).
  std::error_code temp_ec;
  const std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path(temp_ec);
  if (temp_ec) {
    std::cout << "[en-core] FAIL scratch             no usable temporary "
                 "directory: "
              << temp_ec.message() << std::endl;
    return 2;
  }
  std::filesystem::path scratch = temp_dir / "epocanva-en-core-scratch";
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--forcefail") {
      return RunForceFailMode();
    }
    if (arg == "--bench") {
      RunBenchMode();
      return 0;
    }
    if (arg == "--pilot-bench" && i + 1 < argc) {
      return RunPilotBenchMode(std::filesystem::path(argv[++i]));
    }
    if (arg == "--unit") {
      continue;  // the default mode, named so a CTest case reads meaningfully
    }
    if (arg == "--scratch" && i + 1 < argc) {
      scratch = std::filesystem::path(argv[++i]);
      continue;
    }
    std::cout << "[en-core] FAIL arguments           unrecognised argument: "
              << arg << std::endl;
    std::cout << "usage: epocanva_english_core_tests [--unit] [--scratch <dir>] "
                 "[--forcefail] [--bench] [--pilot-bench <word-list.tsv>]"
              << std::endl;
    return 2;
  }
  std::cout << "[en-core] scope: lexicon parsing, prefix completion and the space "
               "policy -- all of it decided without an engine, without a Rime "
               "header, without a language model, a dictionary or any stored input"
            << std::endl;
  TestNormalLoad();
  TestQuerySets();
  TestOrdering();
  TestRejectedInputs();
  TestKLimits();
  TestMalformedData();
  TestCasePreservation();
  TestRawInputImmutable();
  TestDictionaryEntryForms();
  TestTokenStructureTable();
  TestPrefixRanges();
  TestDelimitedCompletions();
  TestApplyCaseStyleWithDelimiters();
  TestProtectionTransition();
  TestSpaceKeyClassification();
  TestSpaceCaseTable();
  TestSpacePolicyConstants();
  TestSpellingMembershipAndFind();
  TestSpellingFourEdits();
  TestSpellingRankingBudgetAndBounds();
  TestSpellingDuplicatesAndSelfEdits();
  TestSpellingGateTable();
  TestSpellingAgainstIndependentReference();
  TestSpellingRawInputImmutableAndCase();
  TestRawInputImmutableWithTokens();
  TestDeterminism();
  TestFileLoads(scratch);
  TestHarnessCanFail();
  std::cout << "[en-core] assertions           " << g_checked << " checked, "
            << g_failed << " failed" << std::endl;
  // The success marker is emitted exactly as tests/infrastructure/assert_probe.cmake
  // searches for it: contiguous "RESULT PASS", no column padding, because a
  // padded label is not the substring the wrapper looks for.
  if (g_failed != 0) {
    std::cout << "RESULT FAIL" << std::endl;
    return 1;
  }
  std::cout << "RESULT PASS" << std::endl;
  return 0;
}
