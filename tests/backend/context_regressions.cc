// Adversarial regressions for the public backend profile/context API.
// Only project-authored fixtures are written, beneath an explicit build scratch
// directory. No host input, Rime deployment, system changes or network calls.
#include "epocanva_backend.h"
#include "engine/backend/context_text.h"
#include "engine/backend_session/session.h"
#include "engine/prediction/model.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace pred = epocanva::prediction;
namespace flow = epocanva::backend_session;
namespace learn = epocanva::learning;
int checks = 0;
int failures = 0;

void Check(bool value, const char* label) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
void Need(bool value, const char* label) {
  Check(value, label);
  if (!value) throw std::runtime_error("fixture or prerequisite failed");
}
void Near(double actual, double expected, const char* label) {
  Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-10, label);
}
template<class T> T Init(std::uint32_t version = EPOCANVA_BACKEND_ABI_VERSION) {
  T value{}; value.struct_size_bytes = sizeof(T); value.abi_version = version; return value;
}
epocanva_utf8_view View(std::string_view text) {
  return {text.data(), static_cast<std::uint64_t>(text.size())};
}
pred::LexemeId Id(std::string_view language, std::string_view word) {
  std::string normalized;
  if (!pred::NormalizeLexemeUtf8(word, &normalized)) throw std::runtime_error("invalid fixture word");
  return pred::StableLexemeId(language, normalized);
}
std::string Sha256(std::string_view bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Need(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
       "native fixture SHA256 provider opens");
  std::array<unsigned char, 32> digest{};
  const auto status = BCryptHash(algorithm, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
  (void)BCryptCloseAlgorithmProvider(algorithm, 0);
  Need(status >= 0, "native fixture SHA256 succeeds");
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (const auto byte : digest) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
  return hex;
}
void WriteFixture(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  Need(static_cast<bool>(out), "project-authored build fixture written completely");
}

pred::ModelRecords Records() {
  pred::ModelRecords records;
  records.metadata.source = pred::SourceKind::kTestFixture;
  records.metadata.source_fingerprint.assign(64, 'a');
  using Word = std::tuple<const char*, const char*, std::uint64_t>;
  const Word words[] = {
      {"en", "hello", 100}, {"en", "good", 100}, {"en", "world", 80},
      {"en", "morning", 60}, {"en", "friend", 50}, {"en", "don't", 10},
      {"fr", "avant", 10}, {"fr", u8"École", 100}, {"fr", u8"école", 10},
      {"fr", "bonjour", 30}, {"ja", u8"か", 10}, {"ja", u8"カ", 10},
      {"zh", u8"你好", 50}, {"zh", u8"世界", 40}, {"ko", u8"안녕", 10},
      {"ko", u8"세계", 10}};
  for (const auto& word : words) {
    const auto id = Id(std::get<0>(word), std::get<1>(word));
    std::string normalized;
    Need(pred::NormalizeLexemeUtf8(std::get<1>(word), &normalized), "fixture word normalized");
    records.lexicon.push_back({id, std::get<0>(word), normalized, pred::TokenKind::kWord});
    records.unigrams.push_back({id, std::get<2>(word)});
  }
  // More than a menu's 64 slots exercise provider union before the final cap.
  for (unsigned ordinal = 0; ordinal < 70; ++ordinal) {
    std::string word = "prefix";
    word += static_cast<char>('a' + ordinal / 26);
    word += static_cast<char>('a' + ordinal % 26);
    const auto id = Id("en", word);
    records.lexicon.push_back({id, "en", word, pred::TokenKind::kWord});
    records.unigrams.push_back({id, ordinal + 1});
  }
  records.unigrams.push_back({0, 10});
  records.bigrams = {{Id("en", "hello"), Id("en", "good"), 40},
      {Id("en", "good"), Id("en", "world"), 60},
      {Id("en", "good"), Id("en", "morning"), 30},
      {Id("fr", "avant"), Id("fr", u8"école"), 10},
      {Id("zh", u8"你好"), Id("zh", u8"世界"), 30}};
  records.trigrams = {{Id("en", "hello"), Id("en", "good"), Id("en", "morning"), 30}};
  std::sort(records.lexicon.begin(), records.lexicon.end(),
      [](const auto& a, const auto& b) { return a.id < b.id; });
  std::sort(records.unigrams.begin(), records.unigrams.end(),
      [](const auto& a, const auto& b) { return a.id < b.id; });
  std::sort(records.bigrams.begin(), records.bigrams.end(), [](const auto& a, const auto& b) {
    return std::tie(a.previous_id, a.id) < std::tie(b.previous_id, b.id);
  });
  records.metadata.lexeme_count = records.lexicon.size();
  for (const auto& row : records.unigrams) records.metadata.unigram_total += row.count;
  for (const auto& row : records.bigrams) records.metadata.bigram_total += row.count;
  for (const auto& row : records.trigrams) records.metadata.trigram_total += row.count;
  return records;
}
std::string Serialize(const pred::ModelRecords& records) {
  std::string payload;
  for (const auto& row : records.lexicon)
    payload += "L\t" + std::to_string(row.id) + "\t" + row.language_id + "\tword\t" + row.normalized_utf8 + "\n";
  for (const auto& row : records.unigrams)
    payload += "U\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  for (const auto& row : records.bigrams)
    payload += "B\t" + std::to_string(row.previous_id) + "\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  for (const auto& row : records.trigrams)
    payload += "T\t" + std::to_string(row.previous2_id) + "\t" + std::to_string(row.previous1_id) +
        "\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  return "EPOCANVA_PREDICTION_TSV\t1\nid_scheme\tfnv1a64-language-normalized-v1\n"
      "normalization\tascii-lower-utf8-v1\nsource\ttest-fixture\nsource_sha256\t" +
      records.metadata.source_fingerprint + "\npayload_sha256\t" + Sha256(payload) +
      "\nlexemes\t" + std::to_string(records.metadata.lexeme_count) +
      "\nunigram_total\t" + std::to_string(records.metadata.unigram_total) +
      "\nbigram_total\t" + std::to_string(records.metadata.bigram_total) +
      "\ntrigram_total\t" + std::to_string(records.metadata.trigram_total) + "\ndata\n" + payload;
}
struct Fixture {
  std::shared_ptr<const pred::Model> model;
  std::string path_utf8, sha256;
  std::string aff_path_utf8, dic_path_utf8, aff_sha256, dic_sha256;
  Fixture(const std::filesystem::path& scratch, bool hunspell) {
    const auto records = Records();
    pred::BuildInfo info;
    model = pred::Model::FromRecords(records, &info);
    Need(model && info.status == pred::Status::kOk, "independent fixture records admitted");
    std::filesystem::create_directories(scratch);
    const auto bytes = Serialize(records);
    path_utf8 = (scratch / "context-regressions-model.tsv").u8string();
    sha256 = Sha256(bytes);
    WriteFixture(std::filesystem::u8path(path_utf8), bytes);
    if (hunspell) {
      const std::string aff = "SET UTF-8\nTRY abcdefghijklmnopqrstuvwxyz\n";
      const std::string dic = "1\nbonjour\n";
      aff_path_utf8 = (scratch / "context-regressions-fr.aff").u8string();
      dic_path_utf8 = (scratch / "context-regressions-fr.dic").u8string();
      aff_sha256 = Sha256(aff); dic_sha256 = Sha256(dic);
      WriteFixture(std::filesystem::u8path(aff_path_utf8), aff);
      WriteFixture(std::filesystem::u8path(dic_path_utf8), dic);
    }
  }
};

epocanva_backend_profile Profile(std::uint32_t id, std::uint32_t language,
                                  std::uint32_t input, std::uint32_t output,
                                  std::string_view variant) {
  auto profile = Init<epocanva_backend_profile>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  profile.profile_id = id; profile.language = language; profile.input_mode = input;
  profile.output_mode = output; profile.variant_id = View(variant);
  profile.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
  return profile;
}
epocanva_backend_session_options SessionOptions(std::uint32_t language, std::uint32_t mode) {
  auto options = Init<epocanva_backend_session_options>();
  options.context = {101, 1, EPOCANVA_PRIVACY_NORMAL, 0};
  options.language = language; options.input_mode = mode;
  return options;  // learning off unless a test explicitly enables it
}
epocanva_backend_request Request(const epocanva_backend_session_options& options) {
  auto request = Init<epocanva_backend_request>();
  request.context = options.context; request.learning_enabled = options.learning_enabled;
  return request;
}
struct Backend {
  epocanva_backend_handle handle = 0;
  explicit Backend(const Fixture& fixture, bool hunspell = false) {
    std::vector<epocanva_backend_resource> resources;
    for (const auto language : {EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_FR, EPOCANVA_BACKEND_JA}) {
      auto row = Init<epocanva_backend_resource>();
      row.language = language; row.model_path = View(fixture.path_utf8); row.model_sha256 = View(fixture.sha256);
      if (hunspell && language == EPOCANVA_BACKEND_FR) {
        row.conversion_path = View(fixture.aff_path_utf8); row.conversion_sha256 = View(fixture.aff_sha256);
        row.auxiliary_path = View(fixture.dic_path_utf8); row.auxiliary_sha256 = View(fixture.dic_sha256);
      }
      resources.push_back(row);
    }
    auto options = Init<epocanva_backend_options>();
    options.resources = resources.data(); options.resource_count = static_cast<std::uint32_t>(resources.size());
    options.allow_test_fixture = 1;
    Need(epocanva_backend_open(&options, &handle) == EPOCANVA_OK && handle,
         "fixture backend opens (requested Hunspell must be available)");
  }
  ~Backend() { if (handle) Check(epocanva_backend_close(handle) == EPOCANVA_OK, "backend cleanup"); }
};
struct Session {
  epocanva_backend_session_handle handle = 0;
  epocanva_backend_request request{};
  Session(Backend& backend, epocanva_backend_session_options options,
          const epocanva_backend_profile* profile = nullptr) : request(Request(options)) {
    const auto status = profile ? epocanva_backend_profile_session_open(backend.handle, profile, &options, &handle) :
        epocanva_backend_session_open(backend.handle, &options, &handle);
    Need(status == EPOCANVA_OK && handle, "fixture session opens");
  }
  ~Session() { if (handle) Check(epocanva_backend_session_close(handle) == EPOCANVA_OK, "session cleanup"); }
};
struct MenuRow { epocanva_backend_candidate candidate{}; std::string text; };
struct Menu { epocanva_backend_snapshot snapshot{}; std::string raw, preedit; std::vector<MenuRow> rows; };
Menu Snapshot(Session& session) {
  Menu menu; menu.snapshot = Init<epocanva_backend_snapshot>();
  const auto status = epocanva_backend_read_snapshot(session.handle, &session.request, &menu.snapshot, nullptr, 0);
  Need(status == EPOCANVA_OK || status == EPOCANVA_BUFFER_TOO_SMALL, "snapshot sizing");
  std::vector<char> bytes(static_cast<std::size_t>(menu.snapshot.required_utf8_bytes) + 1, '!');
  Need(epocanva_backend_read_snapshot(session.handle, &session.request, &menu.snapshot,
      bytes.data(), menu.snapshot.required_utf8_bytes) == EPOCANVA_OK, "snapshot full copy");
  Check(bytes.back() == '!', "snapshot copy does not append terminator");
  menu.raw.assign(bytes.data() + menu.snapshot.raw_input.utf8_byte_offset,
                  static_cast<std::size_t>(menu.snapshot.raw_input.utf8_byte_count));
  menu.preedit.assign(bytes.data() + menu.snapshot.preedit.utf8_byte_offset,
                      static_cast<std::size_t>(menu.snapshot.preedit.utf8_byte_count));
  for (std::uint32_t ordinal = 0; ordinal < menu.snapshot.candidate_count; ++ordinal) {
    MenuRow row; row.candidate = Init<epocanva_backend_candidate>();
    Need(epocanva_backend_read_candidate(session.handle, &session.request, menu.snapshot.snapshot_token,
        ordinal, &row.candidate, nullptr, 0) == EPOCANVA_BUFFER_TOO_SMALL, "candidate sizing");
    std::vector<char> output(static_cast<std::size_t>(row.candidate.required_utf8_bytes) + 1, '!');
    Need(epocanva_backend_read_candidate(session.handle, &session.request, menu.snapshot.snapshot_token,
        ordinal, &row.candidate, output.data(), row.candidate.required_utf8_bytes) == EPOCANVA_OK, "candidate copy");
    Check(output.back() == '!', "candidate copy does not append terminator");
    row.text.assign(output.data() + row.candidate.text.utf8_byte_offset,
                    static_cast<std::size_t>(row.candidate.text.utf8_byte_count));
    menu.rows.push_back(std::move(row));
  }
  return menu;
}
Menu Query(Session& session, std::uint32_t kind = EPOCANVA_BACKEND_COMPLETION,
           std::uint32_t maximum_candidates = 64) {
  auto query = Init<epocanva_backend_query>();
  query.context = session.request.context; query.learning_enabled = session.request.learning_enabled;
  query.query_kind = kind; query.max_candidates = maximum_candidates; query.max_continuation_words = 1;
  Need(epocanva_backend_query_candidates(session.handle, &query) == EPOCANVA_OK, "candidate query");
  auto menu = Snapshot(session);
  Check(menu.rows.size() <= maximum_candidates, "union respects caller candidate-record cap");
  return menu;
}
const MenuRow* Find(const Menu& menu, std::string_view text, bool original = false) {
  const auto found = std::find_if(menu.rows.begin(), menu.rows.end(), [&](const auto& row) {
    return row.text == text && ((row.candidate.kind == EPOCANVA_BACKEND_RAW) == original);
  });
  return found == menu.rows.end() ? nullptr : &*found;
}
void Input(Session& session, std::string_view text) {
  Need(epocanva_backend_set_input(session.handle, &session.request, View(text)) == EPOCANVA_OK, "set fixture input");
}
void Context(Session& session, std::string_view text) {
  Need(epocanva_backend_set_context(session.handle, &session.request, View(text)) == EPOCANVA_OK, "set fixture context");
}
epocanva_backend_text_confirmation Confirmation(const Session& session, std::uint64_t token, std::string_view text) {
  auto confirmation = Init<epocanva_backend_text_confirmation>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  confirmation.request = session.request; confirmation.event_token = token; confirmation.text = View(text);
  return confirmation;
}

void ContextBoundaries(const pred::Model& model) {
  const auto hello = Id("en", "hello"), good = Id("en", "good"), world = Id("en", "world");
  const pred::Context prior{hello};
  Check(epocanva::backend::ResolveContext(model, "en", "good", prior) == pred::Context({hello, good}),
        "known continuation keeps preceding context");
  for (const auto text : {"zzzz good", "zzzz-good", "zzzz!good", "\ngood", "\rgood", u8"\u2028good"}) {
    Check(epocanva::backend::ResolveContext(model, "en", text, prior) ==
              (std::string_view(text) == "zzzz-good" ? pred::Context{} : pred::Context{good}),
          "unknown and line boundary never reconnect to pre-event context");
  }
  Check(epocanva::backend::ResolveContext(model, "en", "good world", prior) == pred::Context({good, world}),
        "context retains only last two admitted lexical records");
  Check(epocanva::backend::ResolveContext(model, "en", "good zzzz", prior).empty(),
        "trailing unknown clears context");
  Check(epocanva::backend::ResolveContext(model, "en", "don't", prior) == pred::Context({hello, Id("en", "don't")}),
        "internal ASCII apostrophe matches admitted whole word");
  const pred::Context chinese_prior{Id("zh", u8"你好")};
  Check(epocanva::backend::ResolveContext(model, "zh", u8"世界", chinese_prior) ==
            pred::Context({Id("zh", u8"你好"), Id("zh", u8"世界")}), "Chinese known suffix context");
  Check(epocanva::backend::ResolveContext(model, "zh", u8"陌世界", chinese_prior) ==
            pred::Context{Id("zh", u8"世界")}, "Chinese OOV scalar breaks previous context");
  Check(epocanva::backend::ResolveContext(model, "ja", u8"未知か", {Id("ja", u8"カ")}) ==
            pred::Context{Id("ja", u8"か")}, "Japanese OOV prefix breaks previous context");
  Check(prior == pred::Context{hello}, "context resolution never mutates caller history");
}

void FlowContextAndRejectedAck(const pred::Model& model) {
  learn::Catalog catalog{"backend.regressions", std::string(64, 'a'), {}};
  for (const auto& row : model.lexemes()) catalog.admitted_ids.push_back(row.id);
  learn::Access access{learn::PrivacyClass::kNormal, false, 17, 3};
  auto session = flow::Session::Create(catalog, std::string(64, 'b'), access);
  Need(static_cast<bool>(session), "flow fixture creates");
  const auto ids = epocanva::backend::ResolveContext(model, "zh", u8"你好");
  Check(session->SetContext(ids, access) == flow::Status::kOk && session->Context(access) == ids,
        "Chinese transient context available while learning disabled");
  Check(session->SetInput("a", "a", access) == flow::Status::kOk, "flow input");
  flow::Candidate candidate; candidate.text_utf8 = "good"; candidate.lexeme_ids = {Id("en", "good")};
  candidate.input_end_utf8_byte = 1;
  Check(session->Publish({candidate}, true, access) == flow::Status::kOk, "flow publish");
  flow::Snapshot old; session->ReadSnapshot(access, &old);
  Check(session->SetContext({Id("en", "hello")}, access) == flow::Status::kOk, "context update invalidates menu");
  Check(session->Select(old.token, old.candidates.front().token, "", access) == flow::Status::kStale,
        "old candidate tokens cannot survive imported context");
  Check(session->SetContext({Id("en", "hello"), Id("en", "good"), Id("en", "world")}, access) == flow::Status::kInvalid,
        "more than two context IDs refused");
  flow::Status other_thread = flow::Status::kOk;
  std::thread worker([&] { other_thread = session->SetContext({}, access); }); worker.join();
  Check(other_thread == flow::Status::kWrongThread, "SetContext owner thread enforced");
  session->Publish({candidate}, true, access);
  session->ReadSnapshot(access, &old);
  Need(session->Select(old.token, old.candidates.front().token, "", access) == flow::Status::kOk, "flow select");
  flow::Commit commit; session->ReadCommit(access, &commit);
  Check(session->SetContext({}, access) == flow::Status::kBusy, "context cannot interrupt pending commit");
  auto rejected = session->Acknowledge(commit.token, false, [access] { return access; });
  Check(rejected.status == flow::Status::kBusy && !rejected.consumed, "unread commit remains guarded even on negative ACK");
  session->MarkCommitCopied(commit.token, access);
  unsigned persistence_calls = 0;
  rejected = session->Acknowledge(commit.token, false, [access] { return access; },
      [&](const auto&, const auto&) { ++persistence_calls; return learn::Status::kOk; });
  Check(rejected.status == flow::Status::kOk && rejected.consumed && !rejected.context_advanced &&
        !rejected.learned && persistence_calls == 0 && session->Context(access).empty(),
        "applied=false consumes once without context or learning");
  Check(session->Acknowledge(commit.token, true, [access] { return access; }).status == flow::Status::kStale,
        "negative ACK token cannot later learn");
}

void JapaneseOutput(Backend& backend) {
  auto profile = Profile(11, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_KATAKANA,
                         EPOCANVA_BACKEND_OUTPUT_HIRAGANA, "ja-JP");
  auto options = SessionOptions(profile.language, profile.input_mode);
  Session session(backend, options, &profile);
  Need(epocanva_backend_push_utf8(session.handle, &session.request, View("ka")) == EPOCANVA_OK, "Japanese input");
  const auto menu = Query(session);
  Check(menu.preedit == u8"か" && Find(menu, u8"か") && !Find(menu, u8"カ"),
        "explicit v2 Hiragana output beats legacy Katakana input hint");
  auto snapshot = Init<epocanva_backend_profile_snapshot>(2); snapshot.snapshot = Init<epocanva_backend_snapshot>();
  std::array<char, 32> bytes{};
  Check(epocanva_backend_read_profile_snapshot(session.handle, &session.request, &snapshot, bytes.data(), bytes.size()) == EPOCANVA_OK &&
        snapshot.profile_id == profile.profile_id && snapshot.output_mode == EPOCANVA_BACKEND_OUTPUT_HIRAGANA,
        "profile metadata agrees with actual Japanese display script");
  Session legacy(backend, options);
  Input(legacy, "ka");
  Check(Query(legacy).preedit == u8"カ", "unchanged v1 Katakana mode still emits Katakana");
}

void AliasUnion(Backend& backend, const pred::Model& model) {
  Session session(backend, SessionOptions(EPOCANVA_BACKEND_FR, EPOCANVA_BACKEND_LATIN));
  Context(session, "avant"); Input(session, u8"éco");
  const auto menu = Query(session);
  const auto* candidate = Find(menu, u8"école");
  Need(candidate != nullptr, "French aliased completion present");
  const auto lower = Id("fr", u8"école"), upper = Id("fr", u8"École");
  const pred::Context context{Id("fr", "avant")};
  Need(model.Score(context, lower).log_probability > model.Score(context, upper).log_probability,
       "fixture lower-count alias actually wins conditional model score");
  Check(candidate->candidate.lexeme_count == 1 && candidate->candidate.lexeme_ids[0] == lower,
        "candidate union replaces high-unigram alias with best conditional identity");
  Near(candidate->candidate.score, model.Score(context, lower).log_probability,
       "union score uses one conditional probability, no lexical count duplication");
}

void FailedProfileOpen(Backend& backend) {
  auto profile = Profile(41, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
                         EPOCANVA_BACKEND_OUTPUT_LATIN, "en-US");
  auto options = SessionOptions(profile.language, profile.input_mode);
  auto bad = options; bad.learning_enabled = 2;
  epocanva_backend_session_handle refused = 99;
  Check(epocanva_backend_profile_session_open(backend.handle, &profile, &bad, &refused) == EPOCANVA_INVALID_ARGUMENT && refused == 0,
        "malformed session flag rejected without a handle");
  profile.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_DVORAK;
  Session retry(backend, options, &profile);
  profile.profile_id = 42;
  bad = options; bad.context.privacy_class = EPOCANVA_PRIVACY_SENSITIVE;
  Check(epocanva_backend_profile_session_open(backend.handle, &profile, &bad, &refused) == EPOCANVA_PRIVACY_DENIED && refused == 0,
        "sensitive session open rejected without registration");
  profile.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_NEUTRAL;
  Session privacy_retry(backend, options, &profile);
  profile.profile_id = 43;
  epocanva_status status = EPOCANVA_OK;
  std::thread worker([&] { status = epocanva_backend_profile_session_open(backend.handle, &profile, &options, &refused); });
  worker.join();
  Check(status == EPOCANVA_WRONG_THREAD && refused == 0, "wrong-thread profile session has no side effects");
  profile.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
  Session thread_retry(backend, options, &profile);
  auto incompatible = profile; incompatible.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_DVORAK;
  Check(epocanva_backend_profile_session_open(backend.handle, &incompatible, &options, &refused) == EPOCANVA_BACKEND_INVALID_PROFILE && refused == 0,
        "successfully registered profile cannot be silently rebound");
}

void ConfirmationAndPermissions(Backend& backend, const pred::Model& model) {
  Session session(backend, SessionOptions(EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN));
  Context(session, "hello");
  auto confirmation = Confirmation(session, 1, "zzzz good");
  auto ack = Init<epocanva_backend_ack_result>();
  Need(epocanva_backend_confirm_text(session.handle, &confirmation, &ack) == EPOCANVA_OK && ack.consumed &&
       ack.context_advanced && !ack.learned, "confirmed ordinary phrase only advances transient context");
  auto menu = Query(session, EPOCANVA_BACKEND_NEXT_UNIT);
  const auto* morning = Find(menu, "morning");
  Need(morning != nullptr, "next-unit candidate after OOV phrase");
  Near(morning->candidate.score, model.Score({Id("en", "good")}, Id("en", "morning")).log_probability,
       "OOV phrase does not resurrect preceding hello trigram");
  Check(epocanva_backend_confirm_text(session.handle, &confirmation, &ack) == EPOCANVA_STALE_SNAPSHOT && !ack.consumed,
        "ordinary confirmation event token consumed exactly once");
  Input(session, "g"); menu = Query(session);
  const auto old = menu.snapshot.snapshot_token;
  auto bad_request = session.request; ++bad_request.context.generation;
  Check(epocanva_backend_set_context(session.handle, &bad_request, View("world")) == EPOCANVA_STALE_SNAPSHOT,
        "stale generation cannot replace context");
  bad_request = session.request; bad_request.learning_enabled = 2;
  Check(epocanva_backend_set_context(session.handle, &bad_request, View("world")) == EPOCANVA_INVALID_ARGUMENT,
        "malformed learning flag refused");
  bad_request = session.request; bad_request.context.reserved = 1;
  Check(epocanva_backend_set_context(session.handle, &bad_request, View("world")) == EPOCANVA_INVALID_ARGUMENT,
        "malformed context flag refused");
  Check(Snapshot(session).snapshot.snapshot_token == old, "invalid requests preserve current menu identity");
  const std::vector<std::string> invalid_utf8{std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2), std::string("\x80", 1)};
  for (const auto& bytes : invalid_utf8) {
    Check(epocanva_backend_push_utf8(session.handle, &session.request, View(bytes)) == EPOCANVA_INVALID_UTF8,
          "malformed scalar push rejected");
    Check(epocanva_backend_set_context(session.handle, &session.request, View(bytes)) == EPOCANVA_INVALID_UTF8,
          "malformed scalar context rejected");
  }
  Check(Snapshot(session).snapshot.snapshot_token == old, "malformed UTF8 does not mutate the snapshot");
  confirmation.request.context.privacy_class = EPOCANVA_PRIVACY_SENSITIVE;
  const auto denied = epocanva_backend_confirm_text(session.handle, &confirmation, &ack);
  Check(denied == EPOCANVA_PRIVACY_DENIED && !ack.learned && !ack.context_advanced,
        "sensitive duplicate confirmation still enforces current permission");
  const auto cleared = Snapshot(session);
  Check(cleared.raw.empty() && cleared.preedit.empty() && cleared.rows.empty(),
        "stale sensitive event clears transient input and candidate menu");
  menu = Query(session, EPOCANVA_BACKEND_NEXT_WORD);
  morning = Find(menu, "morning");
  Need(morning != nullptr, "unconditional next-word candidate present");
  Near(morning->candidate.score, model.Score({}, Id("en", "morning")).log_probability,
       "stale sensitive event also clears previous context");
  confirmation.request = session.request;
  Check(epocanva_backend_confirm_text(session.handle, &confirmation, &ack) == EPOCANVA_STALE_SNAPSHOT,
        "privacy clear never lowers confirmation high-water token");
}

void CopyAndNegativeAck(Backend& backend) {
  auto options = SessionOptions(EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN); options.learning_enabled = 1;
  Session session(backend, options);
  Context(session, "hello"); Input(session, "good");
  const auto menu = Query(session);
  const auto* good = Find(menu, "good"); Need(good != nullptr, "admitted exact candidate");
  Need(epocanva_backend_select(session.handle, &session.request, menu.snapshot.snapshot_token,
      good->candidate.candidate_token) == EPOCANVA_OK, "select commit candidate");
  auto commit = Init<epocanva_backend_commit>();
  Need(epocanva_backend_read_commit(session.handle, &session.request, &commit, nullptr, 0) == EPOCANVA_BUFFER_TOO_SMALL,
       "commit sizing never acknowledges");
  auto ack = Init<epocanva_backend_ack_result>();
  Check(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 1, &ack) == EPOCANVA_BUSY && !ack.consumed,
        "sizing cannot enable positive acknowledgement");
  std::vector<char> buffer(static_cast<std::size_t>(commit.required_utf8_bytes) + 1, '!');
  Need(commit.required_utf8_bytes > 0, "commit contains nonempty fixture text");
  Check(epocanva_backend_read_commit(session.handle, &session.request, &commit, buffer.data(),
      commit.required_utf8_bytes - 1) == EPOCANVA_BUFFER_TOO_SMALL &&
      std::all_of(buffer.begin(), buffer.end(), [](char byte) { return byte == '!'; }),
      "undersized commit copy writes no partial output");
  Check(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 0, &ack) == EPOCANVA_BUSY,
        "undersized copy also cannot enable negative acknowledgement");
  Check(epocanva_backend_set_context(session.handle, &session.request, View("world")) == EPOCANVA_BUSY,
        "context import cannot replace outstanding commit");
  Need(epocanva_backend_read_commit(session.handle, &session.request, &commit, buffer.data(),
      commit.required_utf8_bytes) == EPOCANVA_OK && buffer.back() == '!', "complete commit copy enables ACK only");
  Check(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 2, &ack) == EPOCANVA_INVALID_ARGUMENT && !ack.consumed,
        "invalid applied flag cannot consume commit");
  Need(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 0, &ack) == EPOCANVA_OK &&
      ack.consumed && !ack.learned && !ack.context_advanced, "negative applied ACK consumes without learning or context");
  Check(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 1, &ack) == EPOCANVA_STALE_SNAPSHOT,
        "replayed negative ACK cannot learn");
  const auto after = Snapshot(session);
  Check(after.raw.empty() && after.preedit.empty() && after.rows.empty(), "negative ACK removes transient composition");
}

void CandidateLimits(Backend& backend) {
  auto profile = Profile(61, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
                         EPOCANVA_BACKEND_OUTPUT_LATIN, "en-US");
  Session session(backend, SessionOptions(profile.language, profile.input_mode), &profile);
  for (const std::uint32_t cap : {1u, 2u, 16u, 64u}) {
    Input(session, "prefix");
    const auto menu = Query(session, EPOCANVA_BACKEND_COMPLETION, cap);
    Check(menu.rows.size() == cap && !menu.rows.empty() && menu.rows.front().text == "prefix" &&
          menu.rows.front().candidate.kind == EPOCANVA_BACKEND_RAW && menu.rows.front().candidate.lexeme_count == 0,
          "union cap includes preserved raw entry even with more than 64 matches");
  }
  auto query = Init<epocanva_backend_query>(); query.context = session.request.context;
  query.query_kind = EPOCANVA_BACKEND_COMPLETION; query.max_continuation_words = 1;
  for (const auto invalid_cap : {0u, 65u}) {
    query.max_candidates = invalid_cap;
    Check(epocanva_backend_query_candidates(session.handle, &query) == EPOCANVA_INVALID_ARGUMENT,
          "out-of-range candidate-record cap rejected");
  }
}

void HunspellPenalty(const Fixture& fixture) {
  Backend backend(fixture, true);
  Session session(backend, SessionOptions(EPOCANVA_BACKEND_FR, EPOCANVA_BACKEND_LATIN));
  Input(session, "bonjor");
  const auto menu = Query(session);
  const auto* corrected = Find(menu, "bonjour");
  Need(corrected && corrected->candidate.kind == EPOCANVA_BACKEND_SPELLING && corrected->candidate.lexeme_count == 1,
       "real Hunspell fixture supplies attested correction");
  Near(corrected->candidate.score, -4.0 + fixture.model->Score({}, Id("fr", "bonjour")).log_probability,
       "Hunspell retains spelling penalty plus one model log probability");
}

int Finish() {
  std::cout << "context-regressions checks=" << checks << " failures=" << failures << '\n';
  std::cout << (failures ? "RESULT FAIL\n" : "RESULT PASS\n");
  return failures ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--forcefail") {
    Check(false, "intentional failing regression assertion"); return Finish();
  }
  if ((argc != 3 && argc != 4) || std::string_view(argv[1]) != "--unit" ||
      (argc == 4 && std::string_view(argv[3]) != "--hunspell")) return 2;
  try {
    const auto scratch = std::filesystem::weakly_canonical(std::filesystem::u8path(argv[2]));
    const auto repository = std::filesystem::weakly_canonical(std::filesystem::u8path(__FILE__)).parent_path().parent_path().parent_path();
    const auto relative = scratch.lexically_relative(repository / "build");
    Need(std::filesystem::u8path(argv[2]).is_absolute() && !relative.empty() && relative != "." &&
         *relative.begin() != ".." && std::distance(relative.begin(), relative.end()) >= 2,
         "scratch directory is explicitly inside build/preset/subdirectory");
    Fixture fixture(scratch, argc == 4);
    ContextBoundaries(*fixture.model);
    FlowContextAndRejectedAck(*fixture.model);
    {
      Backend backend(fixture);
      JapaneseOutput(backend);
      AliasUnion(backend, *fixture.model);
      FailedProfileOpen(backend);
      ConfirmationAndPermissions(backend, *fixture.model);
      CopyAndNegativeAck(backend);
      CandidateLimits(backend);
    }
    if (argc == 4) HunspellPenalty(fixture);
  } catch (...) { Check(false, "regression could not complete"); }
  return Finish();
}
