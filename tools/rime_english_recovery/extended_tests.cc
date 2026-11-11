#include "extended_tests.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "english_extended_config.h"

namespace epocanva {
namespace recovery {
namespace {

namespace config = epocanva::english::extended;
constexpr char kSchema[] = "epocanva_extended_english";
constexpr char kSchemaFile[] = "epocanva_extended_english.schema.yaml";
constexpr char kLexiconFile[] = "lexicon/epocanva_extended_lexicon.tsv";
constexpr char kFrequencyFile[] = "frequency/frequency.tsv";
constexpr char kProvenance[] =
    "EpoCanva hand-authored integration fixture v1; synthetic counts; not a corpus";
constexpr char kHeader[] =
    "epocanva.en.frequency/v1\ttest_fixture\n"
    "provenance\tEpoCanva hand-authored integration fixture v1; synthetic counts; not a corpus\n";
constexpr char kHelloRow[] = "13361513971546434870\t50\n";
constexpr char kLegacyMenu[] = "hel|hello|helipad|heliport|helix|hell|helm|helmet";
constexpr char kFrequencyMenu[] = "hel|helium|hello|help|helipad|heliport|helix|hell";
constexpr int kSpace = 0x20;  // X11 keysym; emitted byte is U+0020

enum Feature : unsigned {
  kMorphology = 1,
  kAbbreviation = 2,
  kSpelling = 4,
  kSegmentation = 8,
  kAll = 15,
};
struct SchemaCase {
  const char* id;
  unsigned features;
  const char* frequency = nullptr;  // fixture-relative file name, not input
  int completions = 7;
  bool old_spelling = false;
  bool omit_switches = false;
  bool allow_unattested = false;
  const char* source = config::kTestFixtureSource;
  const char* provenance = kProvenance;
  const char* expected_failure = nullptr;
};
const SchemaCase kSchemas[] = {
    {kSchema, 0},
    {"epocanva_extended_defaults", 0, nullptr, 7, false, true},
    {"epocanva_extended_zero_off", 0, nullptr, 0},
    {"epocanva_extended_morphology", kMorphology, nullptr, 0},
    {"epocanva_extended_generated", kMorphology, nullptr, 0, false, false, true},
    {"epocanva_extended_abbreviation", kAbbreviation},
    {"epocanva_extended_spelling", kSpelling},
    {"epocanva_extended_old_spelling", 0, nullptr, 7, true},
    {"epocanva_extended_both_spelling", kSpelling, nullptr, 7, true},
    {"epocanva_extended_segmentation", kSegmentation},
    {"epocanva_extended_frequency", 0, "frequency.tsv"},
    {"epocanva_extended_frequency_segmentation", kSegmentation, "frequency.tsv"},
    {"epocanva_extended_frequency_crlf", kSegmentation, "crlf.tsv"},
    {"epocanva_extended_all", kAll, "frequency.tsv", 7, true},
    {"epocanva_extended_all_zero", kAll, "frequency.tsv", 0},
    {"epocanva_extended_bad_unknown", kAll, "unknown.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "unknown-frequency-lexeme"},
    {"epocanva_extended_bad_duplicate", kAll, "duplicate.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "duplicate-frequency-record"},
    {"epocanva_extended_bad_sum", kAll, "sum.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "frequency-count-overflow"},
    {"epocanva_extended_bad_count", kAll, "count.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "frequency-value-overflow"},
    {"epocanva_extended_bad_id", kAll, "id.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "frequency-value-overflow"},
    {"epocanva_extended_bad_zero", kAll, "zero.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "invalid-frequency-record"},
    {"epocanva_extended_bad_fields", kAll, "fields.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "invalid-frequency-record"},
    {"epocanva_extended_bad_decimal", kAll, "decimal.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "invalid-frequency-record"},
    {"epocanva_extended_bad_empty", kAll, "empty.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "empty-frequency-data"},
    {"epocanva_extended_bad_missing", kAll, "missing.tsv", 7, true, false, false,
     config::kTestFixtureSource, kProvenance, "frequency-file-not-found"},
    {"epocanva_extended_bad_source", kAll, "frequency.tsv", 7, true, false, false,
     "attestation_weight", kProvenance, "invalid-frequency-source"},
    {"epocanva_extended_bad_relabel", kAll, "frequency.tsv", 7, true, false, false,
     config::kProductionCorpusSource, kProvenance, "frequency-source-or-format-mismatch"},
    {"epocanva_extended_bad_provenance", kAll, "frequency.tsv", 7, true, false, false,
     config::kTestFixtureSource, "different test declaration", "frequency-provenance-mismatch"},
    {"epocanva_extended_bad_no_provenance", kAll, "frequency.tsv", 7, true, false, false,
     config::kTestFixtureSource, nullptr, "invalid-frequency-config"},
};

// Assertions intentionally receive no observed text. Existing Checker::CheckStr,
// CheckMenu, TypeText and DescribeState print raw values, so none is used here.
void Check(Harness& h, bool condition, const std::string& label) {
  h.checks.Check(condition, label, "assertion-mismatch");
}

bool ReadBytes(const std::filesystem::path& path, std::string* text) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

ExitCode WriteFixture(const std::filesystem::path& path, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec || std::filesystem::exists(path, ec) || ec) {
    Fail(kRunDirectoryFailed, "extended.fixture.write", "destination-not-new");
    return kRunDirectoryFailed;
  }
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  std::string actual;
  if (!out || !ReadBytes(path, &actual) || actual != text) {
    Fail(kRunDirectoryFailed, "extended.fixture.write", "byte-verification-failed");
    return kRunDirectoryFailed;
  }
  return kOk;
}

bool ReplaceExactlyOnce(std::string* text, const std::string& from,
                        const std::string& to) {
  const std::size_t found_utf8_byte = text->find(from);
  if (found_utf8_byte == std::string::npos ||
      text->find(from, found_utf8_byte + from.size()) != std::string::npos) {
    return false;
  }
  text->replace(found_utf8_byte, from.size(), to);
  return true;
}

bool DeriveSchema(const std::string& original, const SchemaCase& spec,
                  std::string* result) {
  *result = original;
  if (!ReplaceExactlyOnce(result, "  schema_id: " + std::string(kSchema) + "\n",
                          "  schema_id: " + std::string(spec.id) + "\n") ||
      !ReplaceExactlyOnce(result, "  max_completions: 7\n",
                          "  max_completions: " + std::to_string(spec.completions) + "\n")) {
    return false;
  }
  const std::pair<const char*, unsigned> switches[] = {
      {config::kEnableMorphology, kMorphology},
      {config::kEnableAbbreviation, kAbbreviation},
      {config::kEnableTwoEditSpelling, kSpelling},
      {config::kEnableSegmentation, kSegmentation},
  };
  for (const auto& item : switches) {
    const std::string prefix = "  " + std::string(item.first) + ": ";
    const std::string replacement = spec.omit_switches ? std::string()
        : prefix + ((spec.features & item.second) != 0 ? "true\n" : "false\n");
    if (!ReplaceExactlyOnce(result, prefix + "false\n", replacement)) {
      return false;
    }
  }
  if (spec.old_spelling &&
      !ReplaceExactlyOnce(result, "  enable_suggestions: false\n",
                          "  enable_suggestions: true\n")) {
    return false;
  }
  if (spec.allow_unattested) {
    *result += "  require_attested_morphology: false\n";
  }
  if (spec.frequency) {
    *result += "  frequency_path: \"frequency/" + std::string(spec.frequency) + "\"\n";
    *result += "  frequency_source: \"" + std::string(spec.source) + "\"\n";
    if (spec.provenance) {
      *result += "  frequency_provenance: \"" + std::string(spec.provenance) + "\"\n";
    }
  }
  return true;
}

ExitCode CheckConfig(Harness& h, const SchemaCase& spec) {
  if (!RIME_API_AVAILABLE(h.api, schema_open) ||
      !RIME_API_AVAILABLE(h.api, config_get_bool) ||
      !RIME_API_AVAILABLE(h.api, config_get_int) ||
      !RIME_API_AVAILABLE(h.api, config_get_string) ||
      !RIME_API_AVAILABLE(h.api, config_close)) {
    return kApiUnavailable;
  }
  RimeConfig cfg{};
  if (h.api->schema_open(spec.id, &cfg) != True) {
    return kDeployFailed;
  }
  ++h.balance.config_get;
  const std::pair<const char*, unsigned> switches[] = {
      {config::kEnableMorphology, kMorphology},
      {config::kEnableAbbreviation, kAbbreviation},
      {config::kEnableTwoEditSpelling, kSpelling},
      {config::kEnableSegmentation, kSegmentation},
  };
  const std::string label = std::string(spec.id) + ".config";
  for (const auto& item : switches) {
    Bool value = False;
    const std::string key = "english/" + std::string(item.first);
    const bool found = h.api->config_get_bool(&cfg, key.c_str(), &value) == True;
    Check(h, found == !spec.omit_switches, label + ".switch-presence." + item.first);
    if (found) {
      Check(h, (value == True) == ((spec.features & item.second) != 0),
            label + ".switch-value." + item.first);
    }
  }
  Bool old_spelling = False;
  Check(h, h.api->config_get_bool(&cfg, "english/enable_suggestions", &old_spelling) == True &&
      (old_spelling == True) == spec.old_spelling, label + ".legacy-spelling");
  Bool attested = True;
  const bool attested_present = h.api->config_get_bool(
      &cfg, "english/require_attested_morphology", &attested) == True;
  Check(h, attested_present == spec.allow_unattested &&
      (!attested_present || attested == False), label + ".attestation-default");
  int value = -1;
  Check(h, h.api->config_get_int(&cfg, "english/max_completions", &value) == True &&
      value == spec.completions, label + ".completion-budget");
  value = -1;
  Check(h, h.api->config_get_int(&cfg, "menu/page_size", &value) == True && value == 8,
        label + ".page-size");
  const auto string_equals = [&](const char* key, const char* expected, const char* name) {
    char buffer[512]{};  // UTF-8 code units, with one trailing NUL byte
    const bool found = h.api->config_get_string(&cfg, key, buffer, sizeof(buffer)) == True;
    Check(h, found == (expected != nullptr), label + name + ".presence");
    if (found && expected) {
      Check(h, buffer[sizeof(buffer) - 1] == '\0' && std::string(buffer) == expected,
            label + name + ".value");
    }
  };
  string_equals("english/lexicon_path", kLexiconFile, ".lexicon");
  string_equals("english/frequency_source", spec.frequency ? spec.source : nullptr,
                ".frequency-source");
  string_equals("english/frequency_provenance", spec.frequency ? spec.provenance : nullptr,
                ".frequency-provenance");
  const Bool closed = h.api->config_close(&cfg);
  if (closed != True) {
    return kApiResultUnbalanced;
  }
  ++h.balance.config_free;
  return kOk;
}

ExitCode Prepare(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "extended.surface", "english-module-unavailable");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  for (const auto& spec : kSchemas) {
    if (std::string(spec.id) != kSchema) {
      const auto source = h.shared_dir / (std::string(spec.id) + ".schema.yaml");
      if (h.api->deploy_schema(source.string().c_str()) != True) {
        return kDeployFailed;
      }
      std::error_code ec;
      Check(h, std::filesystem::is_regular_file(
          std::filesystem::path(h.staging_dir) / source.filename(), ec) && !ec,
          std::string(spec.id) + ".compiled-schema");
    }
    const ExitCode rc = CheckConfig(h, spec);
    if (rc != kOk) {
      return rc;
    }
  }
  return kOk;
}

class Session {
 public:
  explicit Session(Harness& h) : h_(h), id_(h.api->create_session()) {}
  ~Session() { if (id_ != kNoSession) h_.api->destroy_session(id_); }
  RimeSessionId id() const { return id_; }
  ExitCode Select(const char* schema) {
    if (id_ == kNoSession) return kCreateSessionFailed;
    if (!RIME_API_AVAILABLE(h_.api, select_schema)) return kApiUnavailable;
    h_.api->select_schema(id_, schema);
    StateSnapshot state;
    ExitCode failure = kOk;
    if (!ReadState(h_.api, id_, &h_.balance, &state, &failure)) return failure;
    Check(h_, state.schema_id == schema, "extended.schema-selected");
    return state.schema_id == schema ? kOk : kSchemaSelectionFailed;
  }
  ExitCode Close() {
    const Bool destroyed = h_.api->destroy_session(id_);
    const bool absent = !h_.api->find_session(id_);
    id_ = kNoSession;
    Check(h_, destroyed == True && absent, "extended.session-released");
    return destroyed == True && absent ? kOk : kDestroySessionFailed;
  }
 private:
  Harness& h_;
  RimeSessionId id_;
};

ExitCode CheckProperty(Harness& h, RimeSessionId sid, const char* key,
                       const char* expected, const std::string& label) {
  if (!RIME_API_AVAILABLE(h.api, get_property)) return kApiUnavailable;
  char value[128]{};  // UTF-8 code units, fixed labels only
  const bool found = h.api->get_property(sid, key, value, sizeof(value)) == True;
  Check(h, found && value[sizeof(value) - 1] == '\0' && std::string(value) == expected,
        label);
  return kOk;
}

// The whole menu, not just the first eight-row page. A ninth item makes this
// fail even if get_context's paged view still reports eight. Iterator ownership
// follows rime_api_impl.h:373-419; End frees both copied strings on every path.
ExitCode CheckWholeMenu(Harness& h, RimeSessionId sid, const StateSnapshot& state,
                        const std::string& label) {
  if (!RIME_API_AVAILABLE(h.api, candidate_list_begin) ||
      !RIME_API_AVAILABLE(h.api, candidate_list_next) ||
      !RIME_API_AVAILABLE(h.api, candidate_list_end)) return kApiUnavailable;
  RimeCandidateListIterator it{};
  const Bool begun = h.api->candidate_list_begin(sid, &it);
  Check(h, begun == True, label + ".iterator-begin");
  if (begun != True) return kOk;
  std::vector<std::string> texts;
  std::vector<std::string> comments;
  Bool next = False;
  // Read at most nine candidates: observing a ninth already falsifies the cap.
  while (texts.size() <= 8 && (next = h.api->candidate_list_next(&it)) == True) {
    texts.emplace_back(it.candidate.text ? it.candidate.text : "");
    comments.emplace_back(it.candidate.comment ? it.candidate.comment : "");
  }
  h.api->candidate_list_end(&it);
  Check(h, next == False && texts.size() <= 8, label + ".whole-menu-budget");
  Check(h, texts == state.candidates && comments == state.comments,
        label + ".whole-menu-matches-page");
  Check(h, std::set<std::string>(texts.begin(), texts.end()).size() == texts.size(),
        label + ".deduplicated");
  return kOk;
}

ExitCode Type(Harness& h, RimeSessionId sid, const std::string& text,
              StateSnapshot* state, const std::string& label) {
  KeyEvent event;
  ExitCode rc = SendKey(h.api, sid, kXkEscape, 0, &h.balance, &event);
  if (rc != kOk) return rc;
  Check(h, !event.state.commit_present, label + ".escape-no-commit");
  std::string prefix;
  for (const unsigned char byte : text) {
    rc = SendKey(h.api, sid, static_cast<int>(byte), 0, &h.balance, &event);
    if (rc != kOk) return rc;
    prefix.push_back(static_cast<char>(byte));
    Check(h, event.handled, label + ".key-handled");
    Check(h, event.state.raw_input == prefix, label + ".raw-unchanged");
    Check(h, event.state.has_preedit && event.state.preedit == prefix &&
        event.state.preedit_length == static_cast<long>(prefix.size()),
        label + ".preedit-utf8-bytes");
    Check(h, !event.state.candidates.empty() && event.state.candidates[0] == prefix,
          label + ".raw-candidate-zero");
    Check(h, !event.state.commit_present, label + ".typing-no-commit");
    Check(h, event.state.page_size == 8 && event.state.num_candidates >= 1 &&
        event.state.num_candidates <= 8, label + ".page-budget");
    rc = CheckWholeMenu(h, sid, event.state, label);
    if (rc != kOk) return rc;
  }
  *state = event.state;
  return kOk;
}

std::size_t CandidateIndex(const StateSnapshot& state, const std::string& text) {
  const auto found = std::find(state.candidates.begin(), state.candidates.end(), text);
  return static_cast<std::size_t>(found - state.candidates.begin());  // menu ordinal
}
bool Contains(const StateSnapshot& state, const std::string& text,
              const char* comment = nullptr) {
  const auto ordinal = CandidateIndex(state, text);
  return ordinal < state.candidates.size() &&
      (!comment || (ordinal < state.comments.size() && state.comments[ordinal] == comment));
}
void RawOnly(Harness& h, const StateSnapshot& state, const std::string& raw,
             const std::string& label) {
  Check(h, state.candidates == std::vector<std::string>{raw}, label + ".raw-only");
}
ExitCode Probe(Harness& h, Session& session, const char* schema, const std::string& raw,
               StateSnapshot* state, const std::string& label) {
  const ExitCode selected = session.Select(schema);
  return selected == kOk ? Type(h, session.id(), raw, state, label) : selected;
}

ExitCode VerifyFinalCounters(Harness& h) {
  EpoCanvaEnglishCounters counters;
  if (!ReadCounters(h.surface, &counters)) return kCounterReadFailed;
  Check(h, counters.lexicon_load_attempts == 1 && counters.lexicon_load_successes == 1 &&
      counters.lexicon_load_failures == 0 && counters.loaded_entry_count == 30,
      "extended.one-immutable-lexicon");
  Check(h, counters.queries_covering_part_of_input == 0 &&
      counters.queries_covering_whole_input == counters.queries,
      "extended.whole-input-only");
  Check(h, counters.original_candidates == counters.queries,
        "extended.one-raw-per-query");
  Check(h, counters.translator_live == 0, "extended.no-live-translator");
  return kOk;
}

}  // namespace

ExitCode StageExtendedFixture(const Options& opt,
                              const std::filesystem::path& shared_dir,
                              std::filesystem::path* schema_source) {
  std::string original, defaults, lexicon, frequencies;
  if (!ReadBytes(opt.fixture_dir / kSchemaFile, &original) ||
      !ReadBytes(opt.fixture_dir / "default.yaml", &defaults) ||
      !ReadBytes(opt.fixture_dir / "lexicon.tsv", &lexicon) ||
      !ReadBytes(opt.fixture_dir / "frequency.tsv", &frequencies)) {
    Fail(kFixtureMissing, "extended.fixture.read", "required-test-input-unavailable");
    return kFixtureMissing;
  }
  ExitCode rc = WriteFixture(shared_dir / "default.yaml", defaults);
  if (rc != kOk) return rc;
  rc = WriteFixture(shared_dir / kLexiconFile, lexicon);
  if (rc != kOk) return rc;
  rc = WriteFixture(shared_dir / kFrequencyFile, frequencies);
  if (rc != kOk) return rc;
  std::string crlf;
  for (const char byte : frequencies) {
    if (byte == '\n') crlf += '\r';
    crlf += byte;
  }
  // Malformed inputs are derived only inside this run root. The source fixture
  // is never edited. Stable decimal IDs are independently calculated FNV-1a-v1
  // fixture constants; unknown ID 1 is not derived from a word under test.
  const std::pair<const char*, std::string> files[] = {
      {"crlf.tsv", crlf},
      {"unknown.tsv", std::string(kHeader) + kHelloRow + "1\t5\n"},
      {"duplicate.tsv", std::string(kHeader) + kHelloRow + kHelloRow},
      {"sum.tsv", std::string(kHeader) +
          "13361513971546434870\t18446744073709551615\n2985470801476758889\t1\n"},
      {"count.tsv", std::string(kHeader) + "13361513971546434870\t18446744073709551616\n"},
      {"id.tsv", std::string(kHeader) + "18446744073709551616\t1\n"},
      {"zero.tsv", std::string(kHeader) + "13361513971546434870\t0\n"},
      {"fields.tsv", std::string(kHeader) + "13361513971546434870\t1\textra\n"},
      {"decimal.tsv", std::string(kHeader) + "013361513971546434870\t1\n"},
      {"empty.tsv", kHeader},
  };
  for (const auto& file : files) {
    rc = WriteFixture(shared_dir / "frequency" / file.first, file.second);
    if (rc != kOk) return rc;
  }
  for (const auto& spec : kSchemas) {
    std::string text;
    if (!DeriveSchema(original, spec, &text)) {
      Fail(kFixtureMissing, "extended.fixture.derive", "schema-anchor-not-unique");
      return kFixtureMissing;
    }
    rc = WriteFixture(shared_dir / (std::string(spec.id) + ".schema.yaml"), text);
    if (rc != kOk) return rc;
  }
  *schema_source = shared_dir / kSchemaFile;
  return kOk;
}

ExitCode RunExtendedDefaultsMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  const char* schemas[] = {kSchema, "epocanva_extended_defaults"};
  for (const char* schema : schemas) {
    StateSnapshot state;
    rc = Probe(h, session, schema, "hel", &state, "extended.defaults.prefix");
    if (rc != kOk) return rc;
    Check(h, state.candidates == SplitPipeList(kLegacyMenu), "extended.defaults.legacy-order");
    rc = CheckProperty(h, session.id(), config::kSupportStatusProperty, "disabled",
                       "extended.defaults.status-disabled");
    if (rc != kOk) return rc;
    for (const char* raw : {"brb", "wroldd", "helloworld"}) {
      rc = Type(h, session.id(), raw, &state, "extended.defaults.no-extensions");
      if (rc != kOk) return rc;
      RawOnly(h, state, raw, "extended.defaults.no-extensions");
    }
  }
  EpoCanvaEnglishCounters counters;
  if (!ReadCounters(h.surface, &counters)) return kCounterReadFailed;
  Check(h, SpellingTotal(counters) == 0, "extended.defaults.legacy-spelling-counters-zero");
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedMorphologyMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  StateSnapshot off, on;
  struct Example { const char* raw; const char* target; const char* label; };
  const Example examples[] = {
      {"walk", "walking", "progressive"}, {"walk", "walked", "past"},
      {"walk", "walks", "third-person"}, {"cat", "cats", "plural"},
      {"child", "children", "irregular"}, {"cat", "cat's", "singular-possessive"},
      {"cats", "cats'", "plural-possessive"}, {"Walk", "Walking", "initial-capital"},
      {"WALK", "WALKING", "uppercase"},
  };
  for (const auto& example : examples) {
    const std::string label = "extended.morphology." + std::string(example.label);
    rc = Probe(h, session, "epocanva_extended_zero_off", example.raw, &off, label);
    if (rc != kOk) return rc;
    RawOnly(h, off, example.raw, label + ".off");
    rc = Probe(h, session, "epocanva_extended_morphology", example.raw, &on, label);
    if (rc != kOk) return rc;
    Check(h, Contains(on, example.target, config::kMorphologyComment), label + ".on");
    Check(h, !Contains(on, "catses'"), label + ".unattested-possessive-base-refused");
  }
  rc = Type(h, session.id(), "hello", &on, "extended.morphology.attested-default");
  if (rc != kOk) return rc;
  Check(h, !Contains(on, "helloing") && !Contains(on, "helloed"),
        "extended.morphology.unattested-forms-refused");
  rc = Probe(h, session, "epocanva_extended_generated", "hello", &on,
             "extended.morphology.fixture-only-generated-scope");
  if (rc != kOk) return rc;
  Check(h, Contains(on, "helloing", config::kMorphologyComment),
        "extended.morphology.explicit-generated-opt-out");
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedAbbreviationMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  struct Example { const char* raw; const char* target; const char* comment; const char* label; };
  const Example examples[] = {
      {"brb", "be right back", config::kAbbreviationComment, "phrase"},
      {"BRB", "BE RIGHT BACK", config::kAbbreviationComment, "uppercase"},
      {"Brb", "Be right back", config::kAbbreviationComment, "initial-capital"},
      {"dont", "don't", config::kContractionComment, "contraction"},
  };
  for (const auto& example : examples) {
    StateSnapshot state;
    const std::string label = "extended.abbreviation." + std::string(example.label);
    rc = Probe(h, session, kSchema, example.raw, &state, label + ".off");
    if (rc != kOk) return rc;
    RawOnly(h, state, example.raw, label + ".off");
    rc = Probe(h, session, "epocanva_extended_abbreviation", example.raw, &state, label + ".on");
    if (rc != kOk) return rc;
    Check(h, Contains(state, example.target, example.comment), label + ".candidate");
  }
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedSpellingMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  StateSnapshot state;
  for (const char* off : {kSchema, "epocanva_extended_old_spelling"}) {
    rc = Probe(h, session, off, "wroldd", &state, "extended.spelling.two-edits-off");
    if (rc != kOk) return rc;
    RawOnly(h, state, "wroldd", "extended.spelling.two-edits-off");
  }
  for (const char* on : {"epocanva_extended_spelling", "epocanva_extended_both_spelling"}) {
    rc = Probe(h, session, on, "wroldd", &state, "extended.spelling.two-edits-on");
    if (rc != kOk) return rc;
    Check(h, Contains(state, "world", config::kSpellingComment),
          "extended.spelling.transposition-plus-deletion");
  }
  rc = Type(h, session.id(), "wrld", &state, "extended.spelling.legacy-dedup");
  if (rc != kOk) return rc;
  Check(h, std::count(state.candidates.begin(), state.candidates.end(), "world") == 1 &&
      Contains(state, "world", EPOCANVA_ENGLISH_SPELLING_COMMENT),
      "extended.spelling.legacy-row-retains-label");
  rc = Probe(h, session, "epocanva_extended_spelling", "WROLDD", &state,
             "extended.spelling.uppercase");
  if (rc != kOk) return rc;
  Check(h, Contains(state, "WORLD", config::kSpellingComment), "extended.spelling.case-preserved");
  rc = Type(h, session.id(), "walking", &state, "extended.spelling.recognized-form");
  if (rc != kOk) return rc;
  RawOnly(h, state, "walking", "extended.spelling.recognized-form");
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedFrequencyMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  StateSnapshot state;
  rc = Probe(h, session, kSchema, "hel", &state, "extended.frequency.legacy");
  if (rc != kOk) return rc;
  Check(h, state.candidates == SplitPipeList(kLegacyMenu) && !Contains(state, "helium"),
        "extended.frequency.promoted-row-outside-old-budget");
  rc = Probe(h, session, "epocanva_extended_frequency", "hel", &state,
             "extended.frequency.ranked");
  if (rc != kOk) return rc;
  Check(h, state.candidates == SplitPipeList(kFrequencyMenu),
        "extended.frequency.ranks-before-truncation");
  rc = CheckProperty(h, session.id(), config::kFrequencySourceProperty,
                     config::kTestFixtureSource, "extended.frequency.test-provenance-only");
  if (rc != kOk) return rc;
  rc = Type(h, session.id(), "helloworld", &state, "extended.segmentation.explicit-off");
  if (rc != kOk) return rc;
  RawOnly(h, state, "helloworld", "extended.segmentation.explicit-off");
  rc = Probe(h, session, "epocanva_extended_segmentation", "helloworld", &state,
             "extended.segmentation.missing-frequency");
  if (rc != kOk) return rc;
  RawOnly(h, state, "helloworld", "extended.segmentation.missing-frequency");
  rc = CheckProperty(h, session.id(), config::kSegmentationStatusProperty,
                     "frequency-unavailable", "extended.segmentation.unavailable-explicit");
  if (rc != kOk) return rc;
  for (const char* schema : {"epocanva_extended_frequency_segmentation",
                              "epocanva_extended_frequency_crlf"}) {
    rc = Probe(h, session, schema, "helloworld", &state, "extended.segmentation.on");
    if (rc != kOk) return rc;
    Check(h, state.candidates == SplitPipeList("helloworld|hello world") &&
        Contains(state, "hello world", config::kSegmentationComment),
        "extended.segmentation.complete-path");
    rc = Type(h, session.id(), "nowhere", &state, "extended.segmentation.ambiguity");
    if (rc != kOk) return rc;
    Check(h, state.candidates == SplitPipeList("nowhere|now here|no where"),
          "extended.segmentation.independent-frequency-order");
    rc = Type(h, session.id(), "HELLOWORLD", &state, "extended.segmentation.uppercase");
    if (rc != kOk) return rc;
    Check(h, Contains(state, "HELLO WORLD", config::kSegmentationComment),
          "extended.segmentation.case-preserved");
  }
  rc = Probe(h, session, "epocanva_extended_frequency", "hel", &state,
             "extended.frequency.immutable-load");
  if (rc != kOk) return rc;
  const auto original = h.shared_dir / kFrequencyFile;
  const auto moved = h.shared_dir / "frequency" / "fixture-moved.tsv";
  std::error_code ec;
  std::filesystem::rename(original, moved, ec);
  Check(h, !ec, "extended.frequency.remove-source-after-construction");
  if (ec) return kRunDirectoryFailed;
  rc = Type(h, session.id(), "hel", &state, "extended.frequency.no-query-read");
  std::filesystem::rename(moved, original, ec);
  Check(h, !ec, "extended.frequency.restore-test-source");
  if (ec) return kRunDirectoryFailed;
  if (rc != kOk) return rc;
  Check(h, state.candidates == SplitPipeList(kFrequencyMenu),
        "extended.frequency.index-survives-source-removal");
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedProtectionMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  rc = session.Select("epocanva_extended_all");
  if (rc != kOk) return rc;
  const char* protected_inputs[] = {
      "brb@example.com", "https://hel.example/", "C:\\hello", "hel_var",
      "wroldd123", "cat++", "BrB", "wAlk", "HelloWorld", "walk--ing", "'walk",
  };
  for (const char* raw : protected_inputs) {
    StateSnapshot state;
    rc = Type(h, session.id(), raw, &state, "extended.protection.raw");
    if (rc != kOk) return rc;
    RawOnly(h, state, raw, "extended.protection.refused");
  }
  StateSnapshot state;
  const std::string overlong(65, 'a');  // UTF-8 code units (ASCII bytes)
  rc = Type(h, session.id(), overlong, &state, "extended.protection.too-long");
  if (rc != kOk) return rc;
  RawOnly(h, state, overlong, "extended.protection.too-long");
  rc = Type(h, session.id(), "brb1", &state, "extended.protection.backspace");
  if (rc != kOk) return rc;
  RawOnly(h, state, "brb1", "extended.protection.before-backspace");
  KeyEvent event;
  rc = SendKey(h.api, session.id(), kXkBackSpace, 0, &h.balance, &event);
  if (rc != kOk) return rc;
  Check(h, event.state.raw_input == "brb" &&
      Contains(event.state, "be right back", config::kAbbreviationComment),
      "extended.protection.recomputed-after-backspace");
  rc = Type(h, session.id(), "hel", &state, "extended.protection.saturated-menu");
  if (rc != kOk) return rc;
  Check(h, state.candidates == SplitPipeList(kFrequencyMenu),
        "extended.protection.extensions-share-eight-total");
  rc = Type(h, session.id(), "walk", &state, "extended.protection.completion-dedup");
  if (rc != kOk) return rc;
  Check(h, std::count(state.candidates.begin(), state.candidates.end(), "walking") == 1,
        "extended.protection.morphology-completion-unique");
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedCommitMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  enum class Accept { kNone, kHighlight, kSelect };
  struct Example {
    const char* schema;
    const char* raw;
    const char* target;
    const char* comment;
    Accept accept;
    bool space;
    const char* expected;
    const char* label;
  };
  const Example examples[] = {
      {"epocanva_extended_all_zero", "brb", "be right back", config::kAbbreviationComment,
       Accept::kNone, false, "brb", "default-return"},
      {"epocanva_extended_all_zero", "brb", "be right back", config::kAbbreviationComment,
       Accept::kNone, true, "brb ", "default-space"},
      {"epocanva_extended_all_zero", "brb", "be right back", config::kAbbreviationComment,
       Accept::kHighlight, true, "brb ", "highlight-space"},
      {"epocanva_extended_all_zero", "brb", "be right back", config::kAbbreviationComment,
       Accept::kSelect, false, "be right back", "abbreviation-return"},
      {"epocanva_extended_all_zero", "BRB", "BE RIGHT BACK", config::kAbbreviationComment,
       Accept::kSelect, true, "BE RIGHT BACK ", "abbreviation-space"},
      {"epocanva_extended_morphology", "walk", "walking", config::kMorphologyComment,
       Accept::kSelect, false, "walking", "morphology-return"},
      {"epocanva_extended_morphology", "cat", "cat's", config::kMorphologyComment,
       Accept::kSelect, true, "cat's ", "possessive-space"},
      {"epocanva_extended_all_zero", "dont", "don't", config::kContractionComment,
       Accept::kSelect, false, "don't", "contraction-return"},
      {"epocanva_extended_spelling", "wroldd", "world", config::kSpellingComment,
       Accept::kSelect, false, "world", "spelling-return"},
      {"epocanva_extended_frequency_segmentation", "helloworld", "hello world",
       config::kSegmentationComment, Accept::kSelect, true, "hello world ", "segmentation-space"},
  };
  for (const auto& example : examples) {
    const std::string label = "extended.commit." + std::string(example.label);
    StateSnapshot before;
    rc = Probe(h, session, example.schema, example.raw, &before, label);
    if (rc != kOk) return rc;
    Check(h, Contains(before, example.target, example.comment), label + ".offered");
    const auto ordinal = CandidateIndex(before, example.target);
    if (example.accept != Accept::kNone && ordinal < before.candidates.size()) {
      if (!RIME_API_AVAILABLE(h.api, select_candidate) ||
          !RIME_API_AVAILABLE(h.api, highlight_candidate)) return kApiUnavailable;
      const Bool accepted = example.accept == Accept::kSelect
          ? h.api->select_candidate(session.id(), ordinal)
          : h.api->highlight_candidate(session.id(), ordinal);
      Check(h, accepted == True, label + ".acceptance-action");
      StateSnapshot selected;
      ExitCode failure = kOk;
      if (!ReadState(h.api, session.id(), &h.balance, &selected, &failure)) return failure;
      Check(h, !selected.commit_present && selected.status_composing &&
          selected.raw_input == example.raw, label + ".selection-not-commit");
    }
    KeyEvent event;
    rc = SendKey(h.api, session.id(), example.space ? kSpace : kXkReturn,
                 0, &h.balance, &event);
    if (rc != kOk) return rc;
    Check(h, event.handled && event.state.commit_present &&
        event.state.commit_text == example.expected, label + ".exact-commit-utf8-bytes");
    Check(h, !event.state.status_composing && !event.state.has_preedit &&
        event.state.raw_input.empty() && event.state.candidates.empty(), label + ".cleared");
    bool repeated = false;
    std::string repeated_text;
    rc = ReadCommitOnce(h.api, session.id(), &h.balance, &repeated, &repeated_text);
    if (rc != kOk) return rc;
    Check(h, !repeated, label + ".commit-consumed-once");
  }
  // A second session cannot inherit the first one's feature switches or output.
  Session other(h);
  StateSnapshot state;
  rc = Probe(h, other, kSchema, "brb", &state, "extended.commit.session-isolation");
  if (rc != kOk) return rc;
  RawOnly(h, state, "brb", "extended.commit.second-session-off");
  rc = other.Close();
  if (rc != kOk) return rc;
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedMalformedFrequencyMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  for (const auto& spec : kSchemas) {
    if (!spec.expected_failure) continue;
    const std::string label = std::string(spec.id) + ".refusal";
    StateSnapshot state;
    rc = Probe(h, session, spec.id, "hel", &state, label);
    if (rc != kOk) return rc;
    Check(h, state.candidates == SplitPipeList(kLegacyMenu), label + ".legacy-preserved");
    rc = CheckProperty(h, session.id(), config::kSupportStatusProperty,
                       spec.expected_failure, label + ".exact-status");
    if (rc != kOk) return rc;
    rc = CheckProperty(h, session.id(), config::kFrequencySourceProperty,
                       "unavailable", label + ".no-partial-frequency");
    if (rc != kOk) return rc;
    rc = CheckProperty(h, session.id(), config::kSegmentationStatusProperty,
                       "support-unavailable", label + ".segmentation-unavailable");
    if (rc != kOk) return rc;
    for (const char* raw : {"brb", "helloworld", "wroldd"}) {
      rc = Type(h, session.id(), raw, &state, label + ".extensions-unavailable");
      if (rc != kOk) return rc;
      RawOnly(h, state, raw, label + ".extensions-unavailable");
    }
    // Legacy one-edit spelling is not disabled by an extension data error.
    rc = Type(h, session.id(), "wrld", &state, label + ".legacy-spelling");
    if (rc != kOk) return rc;
    Check(h, Contains(state, "world", EPOCANVA_ENGLISH_SPELLING_COMMENT),
          label + ".legacy-spelling-preserved");
  }
  rc = session.Close();
  return rc == kOk ? VerifyFinalCounters(h) : rc;
}

ExitCode RunExtendedSelfCheckFailMode(Harness& h) {
  ExitCode rc = Prepare(h);
  if (rc != kOk) return rc;
  Session session(h);
  StateSnapshot state;
  rc = Probe(h, session, "epocanva_extended_abbreviation", "brb", &state,
             "extended.negative.live-menu");
  if (rc != kOk) return rc;
  Check(h, Contains(state, "be right back", config::kAbbreviationComment),
        "extended.negative.positive-prerequisite");
  if (h.checks.failed() != 0) {
    // A stale DLL must not pass this control merely by failing setup. Only the
    // deliberately false claim below is allowed to produce expected exit 14.
    return kNegativeExpectationNotMet;
  }
  Check(h, !state.candidates.empty() && state.candidates[0] == "be right back",
        "extended.negative.intentional-raw-zero-replacement");
  return session.Close();
}

}  // namespace recovery
}  // namespace epocanva
