// Real-resource regression coverage, not a product-readiness assertion.
// CLI: epocanva_backend_profiles <repository-root> <run-root-under-repository/build> [registered-runtime-sha256]
// CMake: C++17, /utf-8 /permissive-, repository include directory; link
// epocanva_backend and epocanva_prediction_core. No generated model is substituted.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "epocanva_backend.h"
#include "engine/data_store/windows_learning_repository.h"
#include "engine/prediction/model.h"
#include "platforms/windows/product/product_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace pred = epocanva::prediction;
namespace learn = epocanva::learning;

std::uint64_t checks = 0;
std::uint64_t failures = 0;
std::uint64_t passed_cases = 0;
std::uint64_t verified_assets = 0;
std::uint64_t next_context_target = 1000;
struct Failed {};

void Check(bool value, const char* condition, int source_line) {
  ++checks;
  if (value) return;
  ++failures;
  std::cerr << "FAIL condition=" << condition << " line=" << source_line << '\n' << std::flush;
  throw Failed{};
}
#define REQUIRE(value, condition) Check((value), (condition), __LINE__)
void Expect(epocanva_status actual, epocanva_status expected,
            const char* condition, int source_line) {
  if (actual != expected)
    std::cerr << "status=" << actual << " expected=" << expected << '\n';
  Check(actual == expected, condition, source_line);
}
#define EXPECT(actual, expected, condition) Expect((actual), (expected), (condition), __LINE__)

template<class F> void Case(const char* name, F&& operation) {
  const auto before = failures;
  try {
    operation();
  } catch (const Failed&) {
  } catch (...) {
    ++failures;
    std::cerr << "FAIL condition=unexpected-exception\n";
  }
  if (failures == before) {
    ++passed_cases;
    std::cout << "PASS case=" << name << " checks=" << checks << '\n' << std::flush;
  } else {
    std::cerr << "FAIL case=" << name << " failures=" << failures << '\n' << std::flush;
  }
}

template<class T> T Init(std::uint32_t abi_version = EPOCANVA_BACKEND_ABI_VERSION) {
  T value{};
  value.struct_size_bytes = static_cast<std::uint32_t>(sizeof(T));
  value.abi_version = abi_version;
  return value;
}
epocanva_utf8_view View(std::string_view text_utf8) {
  return {text_utf8.data(), static_cast<std::uint64_t>(text_utf8.size())};
}
bool Near(double left, double right) {
  return std::fabs(left - right) <= 3e-11 * (1 + std::fabs(left) + std::fabs(right));
}
bool Boundary(std::string_view text_utf8, std::uint64_t offset_utf8_byte) {
  return offset_utf8_byte <= text_utf8.size() &&
      (offset_utf8_byte == text_utf8.size() ||
       (static_cast<unsigned char>(text_utf8[static_cast<std::size_t>(offset_utf8_byte)]) & 0xc0) != 0x80);
}
std::string Slice(const std::string& bytes_utf8, epocanva_utf8_span span) {
  REQUIRE(span.utf8_byte_offset <= bytes_utf8.size() &&
      span.utf8_byte_count <= bytes_utf8.size() - span.utf8_byte_offset, "span-within-utf8-buffer");
  REQUIRE(Boundary(bytes_utf8, span.utf8_byte_offset) &&
      Boundary(bytes_utf8, span.utf8_byte_offset + span.utf8_byte_count), "span-utf8-boundaries");
  return bytes_utf8.substr(static_cast<std::size_t>(span.utf8_byte_offset),
                          static_cast<std::size_t>(span.utf8_byte_count));
}
void Verify(const fs::path& path, std::string_view expected_sha256) {
  REQUIRE(fs::is_regular_file(path), "required-real-asset-present");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  REQUIRE(file.is_open(), "real-asset-digest-readable");
  const auto end = file.tellg();
  REQUIRE(end > 0 && static_cast<std::uintmax_t>(end) <= (256ull << 20),
          "real-asset-size-within-verified-test-bound");
  const auto storage_bytes = static_cast<std::uint64_t>(end);
  std::string bytes(static_cast<std::size_t>(storage_bytes), '\0');
  file.seekg(0);
  REQUIRE(static_cast<bool>(file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))),
          "real-asset-full-read");
  std::string actual_sha256;
  std::wstring reason;
  REQUIRE(epocanva::product::Sha256(bytes.data(), bytes.size(), &actual_sha256, &reason),
          "real-asset-sha256-computed");
  REQUIRE(actual_sha256 == expected_sha256, "real-asset-matches-reviewed-pin");
  ++verified_assets;
}
bool Under(const fs::path& path, const fs::path& parent) {
  const auto relative = path.lexically_relative(parent);
  if (relative.empty() || relative == "." || relative.is_absolute()) return false;
  return std::none_of(relative.begin(), relative.end(), [](const fs::path& component) {
    return component == "..";
  });
}
fs::path IsolatedRoot(const fs::path& repository_root, const fs::path& requested_root) {
  const auto build_root = fs::canonical(repository_root / "build");
  const auto run_parent = fs::weakly_canonical(fs::absolute(requested_root));
  REQUIRE(Under(run_parent, build_root), "run-root-is-inside-repository-build");
  fs::create_directories(run_parent);
  // Exercise wide CLI paths and UTF-8 ABI paths on every run. Never remove an
  // existing directory or reuse another run's counts, compiled data or locks.
  for (unsigned ordinal = 0; ordinal < 100; ++ordinal) {
    const auto name = L"profiles-\u56de\u6b78-" + std::to_wstring(::GetCurrentProcessId()) +
        L"-" + std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(ordinal);
    const auto child = run_parent / name;
    if (fs::create_directory(child)) return child;
  }
  REQUIRE(false, "fresh-run-directory-created");
  return {};
}

struct ModelPin {
  const char* language;
  const char* sha256;
};
// Full-file pins from docs/reports/backend-prediction-development.md, budget
// model SHA256 list. The reference loader never trusts a freshly calculated
// digest, a model's own source field, or an arbitrary replacement report.
constexpr std::array<ModelPin, 5> kModels{{
    {"en", "51717a82412b7f82abd4bf81a77cd3dc39c0392e184f211c4cad2fe31021a2ad"},
    {"zh", "cce13364b25a56940fa1546e738a1571fcd04ed45ee8e91f2ed4ba8ecaa25998"},
    {"fr", "e5d6550dcf0da3fb1fefbf8bb7ddd6618dcd6bf3bfd0e2bc50f8c7cbe056b792"},
    {"ja", "b40382f5e977d01c40a3feb9d735ffdb72698610b9a41f53a9a55045303c0c77"},
    {"ko", "4bb6010cc7145d42361f07e616ceeef3117f1ea7c7a229794442030bd0aa3c31"},
}};
struct FilePin { const char* path; const char* sha256; };
// Prepared resource pins from build/backend-chinese-ranked/manifest.json. This
// receipt is the ranked preparation described in backend-prediction-development.md.
// Pin the shared inputs here, then explicitly deploy in this run's own directory.
constexpr std::array<FilePin, 5> kChinese{{
    {"default.yaml", "b51f10d546611893483c7c916ff7a213a6f3cbcebdba7f6e30dabdca34b806fa"},
    {"epocanva_backend_pinyin.schema.yaml", "6a6a0ed33acc892fe04e94e132ff2cfdd21140cf565b14a5af9a126e9ea2b8e4"},
    {"epocanva_backend_terra.dict.yaml", "f316163a376ef3041d9c512d6de86b57110c20528f084cf8fb9fe1ffd2640ad7"},
    {"epocanva_backend_zhuyin.schema.yaml", "926f445115860cf099eb7802f802e51014761d44bc86906094ad918a91fbf170"},
    {"zhuyin.yaml", "77931ab77952fdb2535f8addebc0918453e0220a50ca176380971203f14c48be"},
}};
// French pair: data/manifests/backend-zh-fr.sources.json, LibreOffice files.
constexpr char kAffSha[] = "c176610cd5dc4846806a65ddd029f422d87978bf58f224aa44222662a16a2de5";
constexpr char kDicSha[] = "b78a868e31dd6e373b6c3217969afb898a9acde828a5e7ef97308da42218c88c";
// JA: report above and build/backend-japanese-data/lexicon.manifest.json.
constexpr char kJapaneseSha[] = "e0308bb1e09e33915a40ca487ca3c27081d9564876f655a23fb332e2d6b3d011";
// DLL: docs/reports/multilingual-foundation.md and the existing
// build/librime-english/engine-build-receipt.json (logging=false).
constexpr char kRimeSha[] = "15650540880774c356fc8fa0a0c0ef5701c7d5b33154c21c7156d521de977c97";

enum Binding : std::size_t {
  kEnglishQwerty, kEnglishDvorak, kEnglishQwertz, kEnglishNeutral,
  kFrenchAzerty, kFrenchQwerty, kPinyin, kZhuyin,
  kRomajiKanji, kRomajiHiragana, kRomajiKatakana, kKatakana,
  kKanaKanji, kKanaHiragana, kKanaKatakana, kTwoSet, kHangul,
  kBindingCount
};
struct Fixture {
  fs::path repository_root;
  fs::path run_root;
  fs::path learning_root;
  std::array<std::string, 5> model_paths;
  std::array<std::shared_ptr<const pred::Model>, 5> models;
  std::array<epocanva_backend_resource, 5> resources{};
  std::array<epocanva_backend_profile_resource, kBindingCount> bindings{};
  std::string aff_path, dic_path, japanese_path;
  std::string rime_sha256;
  std::string rime_path, shared_path, user_path, log_path, learning_path;
  std::array<std::string, 2> schema_paths;
  std::array<epocanva_utf8_view, 2> schema_views{};
  epocanva_engine_options rime = Init<epocanva_engine_options>();

  Fixture(const fs::path& repository, const fs::path& requested_run_root,
          std::string runtime_sha256 = kRimeSha)
      : repository_root(fs::canonical(repository)),
        run_root(IsolatedRoot(repository_root, requested_run_root)),
        learning_root(run_root / "learning"), rime_sha256(std::move(runtime_sha256)) {
    for (std::size_t ordinal = 0; ordinal < kModels.size(); ++ordinal) {
      const auto path = repository_root / "build/backend-training-budget" /
          kModels[ordinal].language / "model.tsv";
      Verify(path, kModels[ordinal].sha256);
      model_paths[ordinal] = path.u8string();
      pred::BuildInfo info;
      models[ordinal] = pred::Model::Load(path, {}, &info);
      REQUIRE(models[ordinal] && info.status == pred::Status::kOk &&
          info.payload_fingerprint_verified, "reference-production-model-loaded");
      REQUIRE(models[ordinal]->available() &&
          models[ordinal]->metadata().source == pred::SourceKind::kProductionCorpus,
          "real-model-is-production-corpus");
      auto& resource = resources[ordinal];
      resource = Init<epocanva_backend_resource>();
      resource.language = static_cast<std::uint32_t>(ordinal + 1);
      resource.model_path = View(model_paths[ordinal]);
      resource.model_sha256 = View(kModels[ordinal].sha256);
    }
    const auto aff = repository_root / "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff";
    const auto dic = repository_root / "build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic";
    const auto japanese = repository_root / "build/backend-japanese-data/lexicon.bin";
    Verify(aff, kAffSha); Verify(dic, kDicSha); Verify(japanese, kJapaneseSha);
    aff_path = aff.u8string(); dic_path = dic.u8string(); japanese_path = japanese.u8string();
    resources[2].conversion_path = View(aff_path);
    resources[2].conversion_sha256 = View(kAffSha);
    resources[2].auxiliary_path = View(dic_path);
    resources[2].auxiliary_sha256 = View(kDicSha);
    resources[3].conversion_path = View(japanese_path);
    resources[3].conversion_sha256 = View(kJapaneseSha);

    REQUIRE(rime_sha256.size() == 64 && std::all_of(rime_sha256.begin(), rime_sha256.end(), [](char byte) {
      return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    }), "runtime-sha256-explicitly-registered");
    const auto dll = repository_root / "build/librime-english/rime/install/lib/rime.dll";
    Verify(dll, rime_sha256);
    const auto shared = run_root / "rime/shared";
    const auto user = run_root / "rime/user";
    const auto logs = run_root / "rime/logs";
    fs::create_directories(shared); fs::create_directories(user);
    fs::create_directories(logs); fs::create_directories(learning_root);
    for (const auto& pin : kChinese) {
      const auto source = repository_root / "build/backend-chinese-ranked/shared" / pin.path;
      Verify(source, pin.sha256);
      REQUIRE(fs::copy_file(source, shared / pin.path), "isolated-chinese-input-copied");
      Verify(shared / pin.path, pin.sha256);
    }
    rime_path = dll.u8string(); shared_path = shared.u8string();
    user_path = user.u8string(); log_path = logs.u8string(); learning_path = learning_root.u8string();
    schema_paths[0] = (shared / "epocanva_backend_pinyin.schema.yaml").u8string();
    schema_paths[1] = (shared / "epocanva_backend_zhuyin.schema.yaml").u8string();
    for (std::size_t ordinal = 0; ordinal < schema_paths.size(); ++ordinal)
      schema_views[ordinal] = View(schema_paths[ordinal]);
    rime.rime_dll_path = View(rime_path);
    rime.shared_data_dir = View(shared_path); rime.user_data_dir = View(user_path);
    rime.log_dir = View(log_path); rime.expected_rime_version = View("1.17.0");
    rime.deployment_schema_paths = schema_views.data();
    rime.deployment_schema_count = static_cast<std::uint32_t>(schema_views.size());

    Bind(kEnglishQwerty, 101, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "en-US");
    Bind(kEnglishDvorak, 102, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_DVORAK, "en-US");
    Bind(kEnglishQwertz, 103, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ, "en-GB");
    Bind(kEnglishNeutral, 104, EPOCANVA_BACKEND_EN, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "en");
    Bind(kFrenchAzerty, 201, EPOCANVA_BACKEND_FR, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY, "fr-FR");
    Bind(kFrenchQwerty, 202, EPOCANVA_BACKEND_FR, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_LATIN, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "fr-FR");
    Bind(kPinyin, 301, EPOCANVA_BACKEND_ZH, EPOCANVA_BACKEND_PINYIN,
         EPOCANVA_BACKEND_OUTPUT_HANZI, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "zh-pinyin");
    Bind(kZhuyin, 302, EPOCANVA_BACKEND_ZH, EPOCANVA_BACKEND_ZHUYIN,
         EPOCANVA_BACKEND_OUTPUT_HANZI, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "zh-zhuyin");
    Bind(kRomajiKanji, 401, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_ROMAJI,
         EPOCANVA_BACKEND_OUTPUT_KANJI, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "ja-JP");
    Bind(kRomajiHiragana, 402, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_ROMAJI,
         EPOCANVA_BACKEND_OUTPUT_HIRAGANA, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "ja-JP");
    Bind(kRomajiKatakana, 403, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_ROMAJI,
         EPOCANVA_BACKEND_OUTPUT_KATAKANA, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "ja-JP");
    Bind(kKatakana, 404, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_KATAKANA,
         EPOCANVA_BACKEND_OUTPUT_KATAKANA, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "ja");
    Bind(kKanaKanji, 405, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_KANA,
         EPOCANVA_BACKEND_OUTPUT_KANJI, EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "ja-JP");
    Bind(kKanaHiragana, 406, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_KANA,
         EPOCANVA_BACKEND_OUTPUT_HIRAGANA, EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "ja-JP");
    Bind(kKanaKatakana, 407, EPOCANVA_BACKEND_JA, EPOCANVA_BACKEND_KANA,
         EPOCANVA_BACKEND_OUTPUT_KATAKANA, EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "ja-JP");
    Bind(kTwoSet, 501, EPOCANVA_BACKEND_KO, EPOCANVA_BACKEND_TWO_SET,
         EPOCANVA_BACKEND_OUTPUT_HANGUL, EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "ko-KR");
    Bind(kHangul, 502, EPOCANVA_BACKEND_KO, EPOCANVA_BACKEND_LATIN,
         EPOCANVA_BACKEND_OUTPUT_HANGUL, EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "ko-KR");
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  void Bind(Binding ordinal, std::uint32_t id, std::uint32_t language,
            std::uint32_t input_mode, std::uint32_t output_mode,
            std::uint32_t keyboard_layout, const char* variant) {
    auto& binding = bindings[ordinal];
    binding.resource = resources[language - 1];
    binding.profile = Init<epocanva_backend_profile>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    auto& profile = binding.profile;
    profile.profile_id = id; profile.language = language;
    profile.input_mode = input_mode; profile.output_mode = output_mode;
    profile.keyboard_layout = keyboard_layout; profile.variant_id = View(variant);
  }
  epocanva_backend_profile_options Options(bool deploy) {
    rime.deployment_schema_paths = deploy ? schema_views.data() : nullptr;
    rime.deployment_schema_count = deploy ? static_cast<std::uint32_t>(schema_views.size()) : 0;
    auto options = Init<epocanva_backend_profile_options>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    options.profiles = bindings.data();
    options.profile_count = static_cast<std::uint32_t>(bindings.size());
    options.allow_test_fixture = 0; options.rime_options = &rime;
    options.learning_root = View(learning_path);
    return options;
  }
  const pred::Model& Model(std::uint32_t language) const { return *models.at(language - 1); }
  learn::Catalog Catalog(std::uint32_t language) const {
    learn::Catalog catalog;
    catalog.name_space = std::string("backend.") + kModels.at(language - 1).language + ".v1";
    catalog.sha256 = kModels.at(language - 1).sha256;
    for (const auto& row : Model(language).lexemes())
      if (row.language_id == kModels.at(language - 1).language) catalog.admitted_ids.push_back(row.id);
    REQUIRE(!catalog.admitted_ids.empty(), "reference-catalog-admits-real-ids");
    return catalog;
  }
};

struct Backend {
  epocanva_backend_handle handle = 0;
  explicit Backend(Fixture& fixture, bool deploy = true) {
    auto options = fixture.Options(deploy);
    EXPECT(epocanva_backend_open_profiles(&options, &handle), EPOCANVA_OK, "real-profiles-backend-open");
    REQUIRE(handle != 0, "backend-handle-nonzero");
  }
  ~Backend() {
    if (handle && epocanva_backend_close(handle) != EPOCANVA_OK) {
      ++failures; std::cerr << "FAIL condition=backend-destructor-close\n";
    }
  }
  void Close() {
    EXPECT(epocanva_backend_close(handle), EPOCANVA_OK, "backend-close"); handle = 0;
  }
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
};
struct Session {
  epocanva_backend_session_handle handle = 0;
  epocanva_backend_request request = Init<epocanva_backend_request>();
  epocanva_backend_profile profile;
  Session(const Backend& backend, const epocanva_backend_profile& binding, bool learning = false)
      : profile(binding) {
    auto options = Init<epocanva_backend_session_options>();
    options.context = {++next_context_target, 1, EPOCANVA_PRIVACY_NORMAL, 0};
    options.language = profile.language; options.input_mode = profile.input_mode;
    options.learning_enabled = learning ? 1u : 0u;
    request.context = options.context; request.learning_enabled = options.learning_enabled;
    EXPECT(epocanva_backend_profile_session_open(backend.handle, &profile, &options, &handle),
           EPOCANVA_OK, "profile-session-open");
    REQUIRE(handle != 0, "session-handle-nonzero");
  }
  ~Session() {
    if (handle && epocanva_backend_session_close(handle) != EPOCANVA_OK) {
      ++failures; std::cerr << "FAIL condition=session-destructor-close\n";
    }
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};
struct Row {
  epocanva_backend_candidate metadata{};
  std::string text_utf8;
  std::vector<pred::LexemeId> ids;
};
struct Menu {
  epocanva_backend_profile_snapshot profile{};
  std::string raw_utf8, preedit_utf8;
  std::vector<Row> rows;
};
Menu Read(const Session& session, const pred::Model& model) {
  Menu menu;
  menu.profile = Init<epocanva_backend_profile_snapshot>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  menu.profile.snapshot = Init<epocanva_backend_snapshot>();
  const auto sized = epocanva_backend_read_profile_snapshot(
      session.handle, &session.request, &menu.profile, nullptr, 0);
  REQUIRE(sized == EPOCANVA_OK || sized == EPOCANVA_BUFFER_TOO_SMALL, "profile-snapshot-sizing");
  const auto required_utf8_bytes = menu.profile.snapshot.required_utf8_bytes;
  REQUIRE(required_utf8_bytes <= 8192, "snapshot-utf8-byte-bound");
  std::string bytes_utf8(static_cast<std::size_t>(required_utf8_bytes), '\0');
  EXPECT(epocanva_backend_read_profile_snapshot(session.handle, &session.request, &menu.profile,
      bytes_utf8.data(), bytes_utf8.size()), EPOCANVA_OK, "profile-snapshot-copy");
  const auto& snapshot = menu.profile.snapshot;
  REQUIRE(menu.profile.profile_id == session.profile.profile_id &&
      menu.profile.output_mode == session.profile.output_mode &&
      menu.profile.keyboard_layout == session.profile.keyboard_layout, "snapshot-exact-profile-binding");
  REQUIRE(menu.profile.unit_kind == EPOCANVA_BACKEND_UNIT_LEXICAL_TOKEN, "next-unit-is-lexical-token");
  REQUIRE(snapshot.language == session.profile.language && snapshot.input_mode == session.profile.input_mode &&
      snapshot.context_target == session.request.context.context_target &&
      snapshot.generation == session.request.context.generation, "snapshot-session-identity");
  menu.raw_utf8 = Slice(bytes_utf8, snapshot.raw_input);
  menu.preedit_utf8 = Slice(bytes_utf8, snapshot.preedit);
  REQUIRE(pred::IsValidUtf8(menu.raw_utf8) && pred::IsValidUtf8(menu.preedit_utf8) &&
      Boundary(menu.preedit_utf8, snapshot.caret_utf8_byte_offset), "snapshot-valid-utf8-caret-byte-offset");
  REQUIRE(snapshot.candidate_count <= 64, "candidate-record-count-bound");
  for (std::uint32_t ordinal = 0; ordinal < snapshot.candidate_count; ++ordinal) {
    Row row; row.metadata = Init<epocanva_backend_candidate>();
    EXPECT(epocanva_backend_read_candidate(session.handle, &session.request, snapshot.snapshot_token,
        ordinal, &row.metadata, nullptr, 0), EPOCANVA_BUFFER_TOO_SMALL, "candidate-sizing");
    REQUIRE(row.metadata.required_utf8_bytes > 0 && row.metadata.required_utf8_bytes <= 4096,
            "candidate-utf8-byte-bound");
    bytes_utf8.assign(static_cast<std::size_t>(row.metadata.required_utf8_bytes), '\0');
    EXPECT(epocanva_backend_read_candidate(session.handle, &session.request, snapshot.snapshot_token,
        ordinal, &row.metadata, bytes_utf8.data(), bytes_utf8.size()), EPOCANVA_OK, "candidate-copy");
    row.text_utf8 = Slice(bytes_utf8, row.metadata.text);
    REQUIRE(pred::IsValidUtf8(row.text_utf8) && std::isfinite(row.metadata.score), "candidate-valid-utf8-score");
    REQUIRE(row.metadata.snapshot_token == snapshot.snapshot_token && row.metadata.candidate_token &&
        row.metadata.lexeme_count <= 8, "candidate-token-and-id-count");
    REQUIRE(row.metadata.input_begin_utf8_byte <= row.metadata.input_end_utf8_byte &&
        Boundary(menu.raw_utf8, row.metadata.input_begin_utf8_byte) &&
        Boundary(menu.raw_utf8, row.metadata.input_end_utf8_byte), "candidate-input-utf8-byte-boundaries");
    row.ids.assign(row.metadata.lexeme_ids, row.metadata.lexeme_ids + row.metadata.lexeme_count);
    for (const auto id : row.ids) {
      const auto* admitted = model.Find(id);
      REQUIRE(admitted && admitted->language_id == kModels[session.profile.language - 1].language,
              "candidate-ids-are-admitted-by-real-language-model");
    }
    if (row.metadata.kind == EPOCANVA_BACKEND_RAW)
      REQUIRE(row.ids.empty(), "raw-candidate-cannot-learn");
    menu.rows.push_back(std::move(row));
  }
  return menu;
}
Menu Query(const Session& session, const pred::Model& model,
           std::uint32_t kind = EPOCANVA_BACKEND_COMPLETION, std::uint32_t max_words = 1) {
  auto query = Init<epocanva_backend_query>();
  query.context = session.request.context; query.learning_enabled = session.request.learning_enabled;
  query.query_kind = kind; query.max_candidates = kind == EPOCANVA_BACKEND_COMPLETION ? 64u : 8u;
  query.max_continuation_words = max_words;
  EXPECT(epocanva_backend_query_candidates(session.handle, &query), EPOCANVA_OK, "profile-query");
  return Read(session, model);
}
const Row& Find(const Menu& menu, std::string_view text_utf8, bool admitted = false) {
  const auto found = std::find_if(menu.rows.begin(), menu.rows.end(), [&](const Row& row) {
    return row.text_utf8 == text_utf8 && (!admitted || !row.ids.empty());
  });
  REQUIRE(found != menu.rows.end(), "fixed-public-target-present");
  return *found;
}
void SetInput(const Session& session, std::string_view text_utf8) {
  EXPECT(epocanva_backend_set_input(session.handle, &session.request, View(text_utf8)),
         EPOCANVA_OK, "replace-input");
}
void SetContext(const Session& session, std::string_view text_utf8) {
  EXPECT(epocanva_backend_set_context(session.handle, &session.request, View(text_utf8)),
         EPOCANVA_OK, "replace-transient-context");
}
void Cancel(const Session& session) {
  EXPECT(epocanva_backend_cancel(session.handle, &session.request), EPOCANVA_OK, "cancel-uncommitted-input");
}
void OldMenuRejected(const Session& session, const Menu& old) {
  REQUIRE(!old.rows.empty(), "stale-probe-has-real-candidate");
  auto row = Init<epocanva_backend_candidate>();
  EXPECT(epocanva_backend_read_candidate(session.handle, &session.request,
      old.profile.snapshot.snapshot_token, 0, &row, nullptr, 0),
      EPOCANVA_STALE_SNAPSHOT, "old-candidate-read-rejected");
  EXPECT(epocanva_backend_select(session.handle, &session.request,
      old.profile.snapshot.snapshot_token, old.rows.front().metadata.candidate_token),
      EPOCANVA_STALE_SNAPSHOT, "old-candidate-select-rejected");
}
std::string Join(const std::vector<std::string>& tokens_utf8, std::uint32_t language) {
  std::string text_utf8;
  for (const auto& token_utf8 : tokens_utf8) {
    const bool punctuation = token_utf8.size() == 1 &&
        std::string_view(".,!?;:").find(token_utf8[0]) != std::string_view::npos;
    if (!text_utf8.empty() && language != EPOCANVA_BACKEND_ZH &&
        language != EPOCANVA_BACKEND_JA && !punctuation) text_utf8.push_back(' ');
    text_utf8 += token_utf8;
  }
  return text_utf8;
}
Menu ExpectPrediction(const Session& session, const pred::Model& model, const pred::Context& context,
                      std::uint32_t kind = EPOCANVA_BACKEND_NEXT_UNIT,
                      const pred::PersonalCounts* personal = nullptr) {
  pred::PredictOptions options;
  options.language_id = kModels[session.profile.language - 1].language;
  options.max_suggestions = 8;
  options.max_words = kind == EPOCANVA_BACKEND_CONTINUATION ? 2u : 1u;
  const auto reference = model.Predict(std::nullopt, context, options, personal);
  REQUIRE(reference.status == pred::Status::kOk || reference.status == pred::Status::kBeamPruned,
          "reference-prediction-status");
  REQUIRE(!reference.suggestions.empty(), "reference-real-context-produces-candidates");
  auto menu = Query(session, model, kind, static_cast<std::uint32_t>(options.max_words));
  REQUIRE(menu.rows.size() == reference.suggestions.size() &&
      bool(menu.profile.snapshot.search_complete) == reference.complete, "prediction-count-and-completeness-match-reference");
  for (std::size_t ordinal = 0; ordinal < menu.rows.size(); ++ordinal) {
    REQUIRE(menu.rows[ordinal].metadata.kind == EPOCANVA_BACKEND_PREDICTION &&
        menu.rows[ordinal].ids == reference.suggestions[ordinal].lexeme_ids &&
        menu.rows[ordinal].text_utf8 == Join(reference.suggestions[ordinal].tokens_utf8, session.profile.language) &&
        Near(menu.rows[ordinal].metadata.score, reference.suggestions[ordinal].log_probability),
        "ranked-prediction-ids-text-and-score-match-model-oracle");
    if (kind == EPOCANVA_BACKEND_NEXT_UNIT)
      REQUIRE(menu.rows[ordinal].ids.size() == 1, "next-unit-emits-one-model-lexical-token");
  }
  return menu;
}
pred::LexemeId Known(const pred::Model& model, std::uint32_t language,
                     std::string_view text_utf8, pred::LexemeId expected_id) {
  const auto* row = model.Find(kModels[language - 1].language, text_utf8);
  REQUIRE(row && row->id == expected_id && row->kind == pred::TokenKind::kWord,
          "fixed-public-context-has-known-catalog-id");
  return row->id;
}

learn::Snapshot Disk(const Fixture& fixture, std::uint32_t language) {
  learn::WindowsLearningRepository repository(fixture.learning_root);
  learn::Snapshot snapshot;
  const learn::Access access{learn::PrivacyClass::kNormal, true, 900, 1};
  const auto status = repository.LoadSnapshot(fixture.Catalog(language), [access] { return access; }, &snapshot);
  REQUIRE(status == learn::Status::kOk, "isolated-learning-snapshot-readable");
  return snapshot;
}
bool Same(const learn::Snapshot& left, const learn::Snapshot& right) {
  if (left.revision != right.revision || left.words.size() != right.words.size() ||
      left.pairs.size() != right.pairs.size()) return false;
  for (std::size_t ordinal = 0; ordinal < left.words.size(); ++ordinal)
    if (left.words[ordinal].id != right.words[ordinal].id ||
        left.words[ordinal].count != right.words[ordinal].count) return false;
  for (std::size_t ordinal = 0; ordinal < left.pairs.size(); ++ordinal)
    if (left.pairs[ordinal].previous_id != right.pairs[ordinal].previous_id ||
        left.pairs[ordinal].id != right.pairs[ordinal].id ||
        left.pairs[ordinal].count != right.pairs[ordinal].count) return false;
  return true;
}
std::uint64_t Count(const learn::Snapshot& snapshot, pred::LexemeId id) {
  for (const auto& word : snapshot.words) if (word.id == id) return word.count;
  return 0;
}
std::uint64_t PairCount(const learn::Snapshot& snapshot, pred::LexemeId previous, pred::LexemeId id) {
  for (const auto& pair : snapshot.pairs) if (pair.previous_id == previous && pair.id == id) return pair.count;
  return 0;
}
void Unchanged(const Fixture& fixture, std::uint32_t language, const learn::Snapshot& before) {
  REQUIRE(Same(Disk(fixture, language), before), "no-unacknowledged-learning-reached-disk");
}
epocanva_backend_commit CopyCommit(const Session& session, const std::string& expected_utf8) {
  auto commit = Init<epocanva_backend_commit>();
  EXPECT(epocanva_backend_read_commit(session.handle, &session.request, &commit, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "commit-sizing-does-not-consume");
  REQUIRE(commit.commit_token && commit.awaiting_ack && commit.required_utf8_bytes <= 8192,
          "commit-token-and-utf8-byte-bound");
  std::string bytes_utf8(static_cast<std::size_t>(commit.required_utf8_bytes), '\0');
  EXPECT(epocanva_backend_read_commit(session.handle, &session.request, &commit,
      bytes_utf8.data(), bytes_utf8.size()), EPOCANVA_OK, "copy-entire-commit");
  REQUIRE(Slice(bytes_utf8, commit.text) == expected_utf8 &&
      Slice(bytes_utf8, commit.remaining_preedit).empty(), "commit-has-selected-public-text");
  return commit;
}
epocanva_backend_ack_result Ack(const Session& session, std::uint64_t token, bool applied = true) {
  auto ack = Init<epocanva_backend_ack_result>();
  EXPECT(epocanva_backend_ack_commit(session.handle, &session.request, token, applied ? 1u : 0u, &ack),
         EPOCANVA_OK, "acknowledge-host-edit");
  REQUIRE(ack.consumed == 1, "ack-consumed-exactly-once");
  return ack;
}
void Select(const Session& session, const Menu& menu, const Row& row) {
  EXPECT(epocanva_backend_select(session.handle, &session.request,
      menu.profile.snapshot.snapshot_token, row.metadata.candidate_token), EPOCANVA_OK, "select-real-candidate");
}

struct PublicConversion { Binding binding; const char* input_utf8; const char* target_utf8; };
constexpr std::array<PublicConversion, 14> kConversions{{
    {kEnglishQwerty, "hel", "hello"},
    {kFrenchAzerty, "francais", u8"français"},
    {kFrenchQwerty, u8"écol", u8"école"},
    {kPinyin, "nihao", u8"你好"},
    {kZhuyin, "su3cl3", u8"你好"},
    {kRomajiKanji, "nihonn", u8"日本"},
    {kRomajiHiragana, "nihonn", u8"にほん"},
    {kRomajiKatakana, "nihonn", u8"ニホン"},
    {kKatakana, "nihonn", u8"ニホン"},
    {kKanaKanji, u8"にほん", u8"日本"},
    {kKanaHiragana, u8"ニホン", u8"にほん"},
    {kKanaKatakana, u8"にほん", u8"ニホン"},
    {kTwoSet, "dkssud", u8"안녕"},
    {kHangul, u8"안녕", u8"안녕"},
}};
void Conversions(const Backend& backend, const Fixture& fixture) {
  for (const auto& test : kConversions) {
    Session session(backend, fixture.bindings[test.binding].profile);
    const auto& model = fixture.Model(session.profile.language);
    SetInput(session, test.input_utf8);
    auto menu = Query(session, model);
    REQUIRE(menu.raw_utf8 == test.input_utf8, "converter-retains-raw-input-utf8");
    const auto& target = Find(menu, test.target_utf8, session.profile.language == EPOCANVA_BACKEND_EN ||
        session.profile.language == EPOCANVA_BACKEND_FR || session.profile.language == EPOCANVA_BACKEND_KO);
    REQUIRE(target.metadata.kind != EPOCANVA_BACKEND_RAW, "converter-target-is-not-ascii-pass-through");
    if (session.profile.language == EPOCANVA_BACKEND_ZH)
      REQUIRE(target.metadata.kind == EPOCANVA_BACKEND_CONVERSION && target.ids.empty(),
              "rime-text-menu-has-no-invented-learning-ids");
    if (session.profile.language == EPOCANVA_BACKEND_JA &&
        session.profile.output_mode != EPOCANVA_BACKEND_OUTPUT_KANJI) {
      REQUIRE(menu.preedit_utf8 == test.target_utf8, "kana-output-mode-controls-preedit-script");
      REQUIRE(std::none_of(menu.rows.begin(), menu.rows.end(), [](const Row& row) {
        return row.text_utf8 == u8"日本";
      }), "kana-only-profile-does-not-return-kanji-conversion");
    }
    Select(session, menu, target);
    const auto commit = CopyCommit(session, test.target_utf8);
    const auto ack = Ack(session, commit.commit_token);
    REQUIRE(!ack.learned, "conversion-consent-off-does-not-learn");
    auto duplicate = Init<epocanva_backend_ack_result>();
    EXPECT(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 1, &duplicate),
           EPOCANVA_STALE_SNAPSHOT, "duplicate-conversion-ack-rejected");
  }
}
void KanaNextOutputUnavailable(const Backend& backend, const Fixture& fixture) {
  constexpr std::array<Binding, 4> kana_only_profiles{{
      kRomajiHiragana, kRomajiKatakana, kKanaHiragana, kKanaKatakana}};
  constexpr std::array<std::uint32_t, 3> query_kinds{{
      EPOCANVA_BACKEND_NEXT_WORD, EPOCANVA_BACKEND_NEXT_UNIT, EPOCANVA_BACKEND_CONTINUATION}};
  for (const auto binding : kana_only_profiles) {
    Session session(backend, fixture.bindings[binding].profile);
    for (const auto kind : query_kinds) {
      auto query = Init<epocanva_backend_query>();
      query.context = session.request.context;
      query.learning_enabled = session.request.learning_enabled;
      query.query_kind = kind;
      query.max_candidates = 8;
      query.max_continuation_words = 1;
      EXPECT(epocanva_backend_query_candidates(session.handle, &query),
          EPOCANVA_BACKEND_UNAVAILABLE, "kana-only-next-output-is-explicitly-unavailable");
    }
  }
}
void Bindings(const Backend& backend, const Fixture& fixture) {
  Session qwerty(backend, fixture.bindings[kEnglishQwerty].profile);
  Session dvorak(backend, fixture.bindings[kEnglishDvorak].profile);
  Session qwertz(backend, fixture.bindings[kEnglishQwertz].profile);
  Session neutral(backend, fixture.bindings[kEnglishNeutral].profile);
  SetInput(qwerty, "hel"); SetInput(dvorak, "wor");
  SetInput(qwertz, "mor"); SetInput(neutral, "goo");
  const auto& model = fixture.Model(1);
  auto first = Query(qwerty, model); auto second = Query(dvorak, model);
  REQUIRE(Find(first, "hello", true).ids.size() == 1 &&
      Find(second, "world", true).ids.size() == 1, "same-language-layouts-use-real-lexicon");
  REQUIRE(Query(qwertz, model).raw_utf8 == "mor" &&
      Query(neutral, model).raw_utf8 == "goo" && first.raw_utf8 == "hel" && second.raw_utf8 == "wor",
      "same-language-profile-inputs-isolated");
  Cancel(qwerty);
  REQUIRE(Read(dvorak, model).raw_utf8 == "wor", "cancel-does-not-affect-other-profile");
  EXPECT(epocanva_backend_select(dvorak.handle, &dvorak.request,
      first.profile.snapshot.snapshot_token, first.rows.front().metadata.candidate_token),
      EPOCANVA_STALE_SNAPSHOT, "tokens-cannot-cross-profile-sessions");
  auto altered = fixture.bindings[kEnglishQwerty].profile;
  altered.keyboard_layout = EPOCANVA_BACKEND_KEYBOARD_DVORAK;
  auto options = Init<epocanva_backend_session_options>();
  options.context = {++next_context_target, 1, EPOCANVA_PRIVACY_NORMAL, 0};
  options.language = altered.language; options.input_mode = altered.input_mode;
  epocanva_backend_session_handle refused = 0;
  EXPECT(epocanva_backend_profile_session_open(backend.handle, &altered, &options, &refused),
         EPOCANVA_BACKEND_INVALID_PROFILE, "profile-id-cannot-silently-rebind-keyboard");
  REQUIRE(refused == 0, "changed-profile-has-no-session");
}

struct PublicContext {
  Binding binding;
  const char* first_utf8;
  pred::LexemeId first_id;
  const char* second_utf8;
  pred::LexemeId second_id;
};
constexpr std::array<PublicContext, 5> kContexts{{
    {kEnglishQwerty, "good", UINT64_C(11777008999757419033), "morning", UINT64_C(6271964512396262284)},
    {kPinyin, u8"你", UINT64_C(12852798573465549172), u8"好", UINT64_C(12460066214126951804)},
    {kFrenchAzerty, "je", UINT64_C(7472415942223301344), "suis", UINT64_C(1696087427504875741)},
    {kRomajiKanji, u8"私", UINT64_C(8685926257875259095), u8"は", UINT64_C(11456407488207766463)},
    {kTwoSet, u8"나는", UINT64_C(14543959826619956553), u8"한국", UINT64_C(4635749007646777561)},
}};
void Contexts(const Backend& backend, const Fixture& fixture) {
  for (const auto& test : kContexts) {
    Session session(backend, fixture.bindings[test.binding].profile);
    const auto language = session.profile.language;
    const auto& model = fixture.Model(language);
    const pred::Context context{Known(model, language, test.first_utf8, test.first_id),
                                Known(model, language, test.second_utf8, test.second_id)};
    auto baseline = ExpectPrediction(session, model, {});
    const std::string preceding_utf8 = std::string(test.first_utf8) + " " + test.second_utf8;
    SetContext(session, preceding_utf8);
    OldMenuRejected(session, baseline);
    auto next = ExpectPrediction(session, model, context);
    REQUIRE(next.rows.front().ids != baseline.rows.front().ids, "real-context-changes-top-ranked-lexical-token");
    auto same = ExpectPrediction(session, model, context, EPOCANVA_BACKEND_NEXT_WORD);
    REQUIRE(next.rows.front().ids == same.rows.front().ids, "next-word-and-next-unit-agree-on-one-token");
    ExpectPrediction(session, model, context, EPOCANVA_BACKEND_CONTINUATION);
    SetContext(session, test.second_utf8);
    ExpectPrediction(session, model, {test.second_id});
    SetContext(session, "");
    ExpectPrediction(session, model, {});
  }
}

// A Model-only oracle for the fixed two-initial grammar. It does not call the
// abbreviation decoder. First initials use its documented 16-record beam;
// second words are independently scored over ALL admitted m-prefix lexemes.
// This is a bounded-decoder regression, not a claim of globally exact decoding.
struct Phrase { std::vector<pred::LexemeId> ids; std::string text_utf8; double score = 0; };
Phrase AbbreviationOracle(const pred::Model& model, const pred::Context& context) {
  pred::PredictOptions options;
  options.language_id = "en"; options.max_suggestions = 17;
  const auto first = model.Predict(std::string_view("g"), context, options);
  REQUIRE(first.status == pred::Status::kOk && first.complete && !first.suggestions.empty(),
          "initial-prefix-oracle-search-complete");
  pred::PredictOptions second_options = options;
  second_options.max_suggestions = 1;
  std::optional<Phrase> best;
  for (std::size_t start_ordinal = 0;
       start_ordinal < first.suggestions.size() && start_ordinal < 16; ++start_ordinal) {
    const auto& start = first.suggestions[start_ordinal];
    REQUIRE(start.lexeme_ids.size() == 1, "initial-prefix-is-one-lexeme");
    const auto* entry = model.Find(start.lexeme_ids.front());
    if (entry->kind != pred::TokenKind::kWord) continue;
    auto previous = context;
    previous.push_back(entry->id);
    if (previous.size() > 2) previous.erase(previous.begin());
    const auto second = model.Predict(std::string_view("m"), previous, second_options);
    REQUIRE(second.status == pred::Status::kOk && second.complete && !second.suggestions.empty(),
            "second-prefix-oracle-search-complete");
    const auto& end = second.suggestions.front();
    REQUIRE(end.lexeme_ids.size() == 1, "second-prefix-is-one-lexeme");
    const auto* end_entry = model.Find(end.lexeme_ids.front());
    REQUIRE(end_entry && end_entry->kind == pred::TokenKind::kWord,
            "second-prefix-produces-admitted-word");
    Phrase candidate{{entry->id, end_entry->id}, entry->normalized_utf8 + " " + end_entry->normalized_utf8,
                     start.log_probability + end.log_probability};
    if (!best || candidate.score > best->score ||
        (candidate.score == best->score && candidate.ids < best->ids)) best = std::move(candidate);
  }
  REQUIRE(best.has_value(), "real-model-initialism-has-complete-phrase");
  return *best;
}
const Row& TopExpansion(const Menu& menu) {
  const auto found = std::find_if(menu.rows.begin(), menu.rows.end(), [](const Row& row) {
    return row.metadata.kind == EPOCANVA_BACKEND_EXPANSION && row.ids.size() == 2;
  });
  REQUIRE(found != menu.rows.end(), "real-two-word-expansion-present");
  return *found;
}
void Abbreviations(const Backend& backend, const Fixture& fixture) {
  Session session(backend, fixture.bindings[kEnglishQwerty].profile);
  const auto& model = fixture.Model(1);
  const pred::Context context{Known(model, 1, "have", UINT64_C(13325501036273435738)),
                              Known(model, 1, "a", UINT64_C(9897245132611483951))};
  const auto empty_oracle = AbbreviationOracle(model, {});
  const auto context_oracle = AbbreviationOracle(model, context);
  REQUIRE(empty_oracle.ids != context_oracle.ids, "pinned-model-has-real-abbreviation-context-flip");
  EXPECT(epocanva_backend_push_utf8(session.handle, &session.request, View("g")), EPOCANVA_OK, "push-first-initial");
  EXPECT(epocanva_backend_push_utf8(session.handle, &session.request, View("m")), EPOCANVA_OK, "append-second-initial");
  auto empty = Query(session, model);
  const auto& first = TopExpansion(empty);
  REQUIRE(first.ids == empty_oracle.ids && first.text_utf8 == empty_oracle.text_utf8 &&
      Near(first.metadata.score, empty_oracle.score), "empty-context-expansion-is-reference-highest");
  SetContext(session, "have a");
  OldMenuRejected(session, empty);
  auto contextual = Query(session, model);
  const auto& second = TopExpansion(contextual);
  REQUIRE(second.ids == context_oracle.ids && second.text_utf8 == context_oracle.text_utf8 &&
      Near(second.metadata.score, context_oracle.score), "imported-context-expansion-is-reference-highest");
  REQUIRE(first.ids != second.ids, "backend-abbreviation-leader-flips-with-model-context");
  SetInput(session, "g m");
  auto separated = Query(session, model);
  REQUIRE(TopExpansion(separated).ids == context_oracle.ids, "separated-initialism-preserves-context-ranking");
}

void Edits(const Backend& backend, const Fixture& fixture) {
  Session session(backend, fixture.bindings[kEnglishQwerty].profile);
  const auto& model = fixture.Model(1);
  SetInput(session, "hel"); auto original = Query(session, model);
  auto repeated = Query(session, model);
  OldMenuRejected(session, original);
  REQUIRE(Find(repeated, "hello", true).ids == Find(original, "hello", true).ids,
          "requery-keeps-model-answer-with-fresh-tokens");
  EXPECT(epocanva_backend_push_ascii(session.handle, &session.request, 'l'), EPOCANVA_OK, "ascii-edit");
  OldMenuRejected(session, repeated);
  auto edited = Query(session, model);
  REQUIRE(edited.raw_utf8 == "hell", "append-updates-raw-input");
  EXPECT(epocanva_backend_backspace(session.handle, &session.request), EPOCANVA_OK, "backspace-edit");
  OldMenuRejected(session, edited);
  auto restored = Query(session, model);
  REQUIRE(restored.raw_utf8 == "hel" && !Find(restored, "hello", true).ids.empty(), "backspace-requery-restores-candidate");
  SetInput(session, "wor"); OldMenuRejected(session, restored);
  auto before_context = Query(session, model);
  SetContext(session, "have a"); OldMenuRejected(session, before_context);
  auto imported = Query(session, model);
  REQUIRE(imported.raw_utf8 == "wor", "context-import-preserves-uncommitted-input");
  auto stale = session.request; ++stale.context.generation;
  EXPECT(epocanva_backend_set_input(session.handle, &stale, View("hel")), EPOCANVA_STALE_SNAPSHOT, "stale-generation-cannot-edit");
  stale = session.request; ++stale.context.context_target;
  EXPECT(epocanva_backend_select(session.handle, &stale, imported.profile.snapshot.snapshot_token,
      imported.rows.front().metadata.candidate_token), EPOCANVA_STALE_SNAPSHOT, "stale-target-cannot-select");
  Cancel(session); OldMenuRejected(session, imported);
  const auto cleared = Read(session, model);
  REQUIRE(cleared.raw_utf8.empty() && cleared.preedit_utf8.empty() && cleared.rows.empty(), "cancel-clears-transient-state");
  ExpectPrediction(session, model, {});

  Session french(backend, fixture.bindings[kFrenchAzerty].profile);
  EXPECT(epocanva_backend_push_utf8(french.handle, &french.request, View("caf")), EPOCANVA_OK, "push-french-prefix");
  EXPECT(epocanva_backend_push_utf8(french.handle, &french.request, View(u8"é")), EPOCANVA_OK, "push-french-nonascii-scalar");
  auto accented = Query(french, fixture.Model(3));
  REQUIRE(accented.raw_utf8 == u8"café" && accented.preedit_utf8 == u8"café" &&
      !Find(accented, u8"café", true).ids.empty(), "unicode-french-input-keeps-real-admitted-candidate");
  EXPECT(epocanva_backend_backspace(french.handle, &french.request), EPOCANVA_OK, "backspace-whole-french-scalar");
  OldMenuRejected(french, accented);
  REQUIRE(Query(french, fixture.Model(3)).raw_utf8 == "caf", "backspace-does-not-leave-partial-utf8");
  EXPECT(epocanva_backend_push_utf8(french.handle, &french.request, View("\xc0\xaf")), EPOCANVA_INVALID_UTF8, "malformed-utf8-push-rejected");
  EXPECT(epocanva_backend_set_context(french.handle, &french.request, View("\xed\xa0\x80")), EPOCANVA_INVALID_UTF8, "surrogate-context-rejected");
}

epocanva_backend_ack_result Confirm(const Session& session, std::uint64_t event_token,
                                    std::string_view text_utf8, epocanva_status expected = EPOCANVA_OK) {
  auto confirmation = Init<epocanva_backend_text_confirmation>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  confirmation.request = session.request; confirmation.event_token = event_token;
  confirmation.text = View(text_utf8);
  auto result = Init<epocanva_backend_ack_result>();
  EXPECT(epocanva_backend_confirm_text(session.handle, &confirmation, &result), expected, "confirm-ordinary-host-text");
  return result;
}
void Confirmations(const Backend& backend, const Fixture& fixture) {
  Session session(backend, fixture.bindings[kEnglishQwerty].profile, true);
  const auto& model = fixture.Model(1);
  const auto before = Disk(fixture, 1);
  const auto have = Known(model, 1, "have", UINT64_C(13325501036273435738));
  const auto a = Known(model, 1, "a", UINT64_C(9897245132611483951));
  const auto good = Known(model, 1, "good", UINT64_C(11777008999757419033));
  const auto morning = Known(model, 1, "morning", UINT64_C(6271964512396262284));
  SetContext(session, "have");
  SetInput(session, "good"); auto unselected = Query(session, model);
  Unchanged(fixture, 1, before);
  auto ack = Confirm(session, 10, "a");
  REQUIRE(ack.consumed && ack.context_advanced && !ack.learned, "ordinary-confirmation-is-transient-only");
  OldMenuRejected(session, unselected);
  auto contextual = ExpectPrediction(session, model, {have, a});
  REQUIRE(Read(session, model).raw_utf8.empty(), "confirmation-clears-unselected-raw-input");
  auto duplicate = Confirm(session, 10, "good", EPOCANVA_STALE_SNAPSHOT);
  REQUIRE(!duplicate.consumed && !duplicate.learned && !duplicate.context_advanced, "duplicate-event-does-not-advance-context");
  Confirm(session, 9, "morning", EPOCANVA_STALE_SNAPSHOT);
  auto same = ExpectPrediction(session, model, {have, a});
  REQUIRE(same.rows.front().ids == contextual.rows.front().ids, "event-dedup-keeps-context-ranking");
  ack = Confirm(session, 11, "good");
  REQUIRE(ack.consumed && !ack.learned, "next-confirmation-event-consumed");
  ExpectPrediction(session, model, {a, good});
  Confirm(session, 12, "\xc0\xaf", EPOCANVA_INVALID_UTF8);
  ack = Confirm(session, 12, "morning");
  REQUIRE(ack.context_advanced && !ack.learned, "invalid-confirmation-does-not-consume-event-token");
  ExpectPrediction(session, model, {good, morning});
  SetContext(session, "");
  ExpectPrediction(session, model, {});
  SetInput(session, "hello"); Query(session, model); Cancel(session);
  ExpectPrediction(session, model, {});
  Unchanged(fixture, 1, before);
}

void Persistence(const Backend& backend, const Fixture& fixture) {
  const auto& model = fixture.Model(1);
  const auto good = Known(model, 1, "good", UINT64_C(11777008999757419033));
  const auto morning = Known(model, 1, "morning", UINT64_C(6271964512396262284));
  auto before = Disk(fixture, 1);
  REQUIRE(before.revision == 0 && before.words.empty() && before.pairs.empty(), "isolated-run-starts-with-zero-learning");
  Session session(backend, fixture.bindings[kEnglishQwerty].profile, true);
  SetInput(session, "good"); auto menu = Query(session, model);
  const auto& row = Find(menu, "good", true);
  REQUIRE(row.ids == std::vector<pred::LexemeId>{good}, "confirmed-candidate-has-explicit-known-id");
  Unchanged(fixture, 1, before);
  Select(session, menu, row); Unchanged(fixture, 1, before);
  auto sizing = Init<epocanva_backend_commit>();
  EXPECT(epocanva_backend_read_commit(session.handle, &session.request, &sizing, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "learning-commit-sizing");
  auto result = Init<epocanva_backend_ack_result>();
  EXPECT(epocanva_backend_ack_commit(session.handle, &session.request, sizing.commit_token, 1, &result),
         EPOCANVA_BUSY, "sizing-alone-cannot-enable-learning");
  REQUIRE(!result.consumed && !result.learned, "uncopied-commit-not-consumed");
  EXPECT(epocanva_backend_set_context(session.handle, &session.request, View("have a")),
         EPOCANVA_BUSY, "context-import-cannot-replace-pending-commit");
  const auto commit = CopyCommit(session, "good");
  Unchanged(fixture, 1, before);
  result = Ack(session, commit.commit_token);
  REQUIRE(result.learned && result.context_advanced &&
      result.persistence_status == static_cast<std::uint32_t>(learn::Status::kOk), "ack-persists-confirmed-id");
  auto learned = Disk(fixture, 1);
  REQUIRE(learned.revision == 1 && learned.words.size() == 1 && Count(learned, good) == 1 && learned.pairs.empty(),
          "disk-contains-exactly-one-confirmed-id-count");
  EXPECT(epocanva_backend_ack_commit(session.handle, &session.request, commit.commit_token, 1, &result),
         EPOCANVA_STALE_SNAPSHOT, "duplicate-ack-cannot-double-count-disk");
  Unchanged(fixture, 1, learned);

  SetInput(session, "morning"); menu = Query(session, model);
  const auto& next = Find(menu, "morning", true);
  REQUIRE(next.ids == std::vector<pred::LexemeId>{morning}, "second-confirmed-candidate-has-explicit-known-id");
  Select(session, menu, next);
  auto next_commit = CopyCommit(session, "morning");
  Unchanged(fixture, 1, learned);
  result = Ack(session, next_commit.commit_token);
  REQUIRE(result.learned && result.context_advanced, "second-confirmed-id-learns");
  learned = Disk(fixture, 1);
  REQUIRE(learned.revision == 2 && learned.words.size() == 2 && Count(learned, good) == 1 &&
      Count(learned, morning) == 1 && learned.pairs.size() == 1 && PairCount(learned, good, morning) == 1,
      "disk-pair-only-links-two-confirmed-candidates");

  SetContext(session, "have a");
  SetInput(session, "good"); menu = Query(session, model);
  Select(session, menu, Find(menu, "good", true));
  next_commit = CopyCommit(session, "good");
  result = Ack(session, next_commit.commit_token);
  REQUIRE(result.learned, "candidate-after-context-import-learns-own-id");
  learned = Disk(fixture, 1);
  REQUIRE(learned.revision == 3 && Count(learned, good) == 2 && Count(learned, morning) == 1 &&
      learned.words.size() == 2 && learned.pairs.size() == 1 && PairCount(learned, good, morning) == 1,
      "imported-text-never-becomes-personal-learning-predecessor");

  SetInput(session, "hello"); menu = Query(session, model);
  REQUIRE(menu.rows.front().metadata.kind == EPOCANVA_BACKEND_RAW && menu.rows.front().ids.empty(),
          "raw-selection-has-no-confirmed-lexical-id");
  Select(session, menu, menu.rows.front());
  next_commit = CopyCommit(session, "hello"); result = Ack(session, next_commit.commit_token);
  REQUIRE(!result.learned && result.context_advanced, "raw-selection-can-advance-transient-context-without-learning");
  Unchanged(fixture, 1, learned);
  SetInput(session, "good"); menu = Query(session, model);
  Select(session, menu, Find(menu, "good", true));
  next_commit = CopyCommit(session, "good"); result = Ack(session, next_commit.commit_token, false);
  REQUIRE(!result.learned && !result.context_advanced, "unapplied-host-edit-does-not-learn");
  Unchanged(fixture, 1, learned);
}

void Privacy(const Backend& backend, const Fixture& fixture) {
  for (const auto& test : kContexts) {
    const auto& profile = fixture.bindings[test.binding].profile;
    const auto& model = fixture.Model(profile.language);
    const auto before = Disk(fixture, profile.language);
    const std::array<std::uint32_t, 2> privacy_classes{{EPOCANVA_PRIVACY_SENSITIVE, EPOCANVA_PRIVACY_UNKNOWN}};
    for (const std::uint32_t privacy : privacy_classes) {
      auto options = Init<epocanva_backend_session_options>();
      options.context = {++next_context_target, 1, static_cast<std::uint32_t>(privacy), 0};
      options.language = profile.language; options.input_mode = profile.input_mode; options.learning_enabled = 1;
      epocanva_backend_session_handle refused = 0;
      EXPECT(epocanva_backend_profile_session_open(backend.handle, &profile, &options, &refused),
             EPOCANVA_PRIVACY_DENIED, "private-profile-session-open-denied");
      REQUIRE(refused == 0, "private-session-not-created");
      Session session(backend, profile, true);
      const auto conversion = std::find_if(kConversions.begin(), kConversions.end(), [&](const auto& entry) {
        return entry.binding == test.binding;
      });
      REQUIRE(conversion != kConversions.end(), "privacy-test-has-real-converter-case");
      SetInput(session, conversion->input_utf8); auto menu = Query(session, model);
      auto denied = session.request; denied.context.privacy_class = static_cast<std::uint32_t>(privacy);
      EXPECT(epocanva_backend_set_input(session.handle, &denied, View(conversion->input_utf8)), EPOCANVA_PRIVACY_DENIED, "private-set-input-denied");
      EXPECT(epocanva_backend_push_ascii(session.handle, &denied, 'a'), EPOCANVA_PRIVACY_DENIED, "private-ascii-push-denied");
      EXPECT(epocanva_backend_push_utf8(session.handle, &denied, View("a")), EPOCANVA_PRIVACY_DENIED, "private-utf8-push-denied");
      EXPECT(epocanva_backend_backspace(session.handle, &denied), EPOCANVA_PRIVACY_DENIED, "private-backspace-denied");
      EXPECT(epocanva_backend_set_context(session.handle, &denied, View(test.first_utf8)), EPOCANVA_PRIVACY_DENIED, "private-context-import-denied");
      const std::array<std::uint32_t, 4> query_kinds{{EPOCANVA_BACKEND_COMPLETION,
          EPOCANVA_BACKEND_NEXT_WORD, EPOCANVA_BACKEND_NEXT_UNIT, EPOCANVA_BACKEND_CONTINUATION}};
      for (const auto kind : query_kinds) {
        auto query = Init<epocanva_backend_query>(); query.context = denied.context;
        query.learning_enabled = 1; query.query_kind = kind; query.max_candidates = 8; query.max_continuation_words = 1;
        EXPECT(epocanva_backend_query_candidates(session.handle, &query), EPOCANVA_PRIVACY_DENIED, "private-query-kind-denied");
      }
      std::array<char, 32> sentinel; sentinel.fill('!');
      auto snapshot = Init<epocanva_backend_snapshot>();
      EXPECT(epocanva_backend_read_snapshot(session.handle, &denied, &snapshot, sentinel.data(), sentinel.size()),
             EPOCANVA_PRIVACY_DENIED, "private-v1-snapshot-denied");
      auto profile_snapshot = Init<epocanva_backend_profile_snapshot>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
      profile_snapshot.snapshot = Init<epocanva_backend_snapshot>();
      EXPECT(epocanva_backend_read_profile_snapshot(session.handle, &denied, &profile_snapshot, sentinel.data(), sentinel.size()),
             EPOCANVA_PRIVACY_DENIED, "private-profile-snapshot-denied");
      auto candidate = Init<epocanva_backend_candidate>();
      EXPECT(epocanva_backend_read_candidate(session.handle, &denied, menu.profile.snapshot.snapshot_token, 0,
          &candidate, sentinel.data(), sentinel.size()), EPOCANVA_PRIVACY_DENIED, "private-candidate-read-denied");
      EXPECT(epocanva_backend_select(session.handle, &denied, menu.profile.snapshot.snapshot_token,
          menu.rows.front().metadata.candidate_token), EPOCANVA_PRIVACY_DENIED, "private-selection-denied");
      auto denied_commit = Init<epocanva_backend_commit>();
      EXPECT(epocanva_backend_read_commit(session.handle, &denied, &denied_commit, sentinel.data(), sentinel.size()),
             EPOCANVA_PRIVACY_DENIED, "private-commit-read-denied");
      REQUIRE(std::all_of(sentinel.begin(), sentinel.end(), [](char byte) { return byte == '!'; }) &&
          !snapshot.required_utf8_bytes && !profile_snapshot.snapshot.required_utf8_bytes &&
          !candidate.required_utf8_bytes && !denied_commit.required_utf8_bytes, "private-reads-return-no-text-bytes");

      Select(session, menu, Find(menu, conversion->target_utf8));
      const auto commit = CopyCommit(session, conversion->target_utf8);
      auto result = Init<epocanva_backend_ack_result>();
      EXPECT(epocanva_backend_ack_commit(session.handle, &denied, commit.commit_token, 1, &result),
             EPOCANVA_PRIVACY_DENIED, "privacy-rechecked-at-confirmed-candidate-ack");
      REQUIRE(result.consumed && !result.learned && !result.context_advanced, "private-ack-discards-without-learning");
      EXPECT(epocanva_backend_read_commit(session.handle, &session.request, &denied_commit, nullptr, 0),
             EPOCANVA_NO_COMMIT, "private-ack-cannot-be-replayed-after-normal-restored");
      ExpectPrediction(session, model, {});

      SetContext(session, test.first_utf8); SetInput(session, conversion->input_utf8);
      auto confirmation = Init<epocanva_backend_text_confirmation>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
      confirmation.request = denied; confirmation.event_token = 1; confirmation.text = View(test.second_utf8);
      result = Init<epocanva_backend_ack_result>();
      EXPECT(epocanva_backend_confirm_text(session.handle, &confirmation, &result),
             EPOCANVA_PRIVACY_DENIED, "private-ordinary-confirmation-denied");
      REQUIRE(result.consumed && !result.learned && !result.context_advanced, "private-confirmation-discards-transient-text");
      auto cleared = Read(session, model);
      REQUIRE(cleared.raw_utf8.empty() && cleared.preedit_utf8.empty() && cleared.rows.empty(),
              "private-confirmation-clears-composition");
      ExpectPrediction(session, model, {});
      SetInput(session, conversion->input_utf8); menu = Query(session, model);
      Select(session, menu, Find(menu, conversion->target_utf8)); CopyCommit(session, conversion->target_utf8);
      // Explicit discard is allowed for private fields by the session contract;
      // every content/prediction/learning operation above is denied.
      EXPECT(epocanva_backend_cancel(session.handle, &denied), EPOCANVA_OK, "private-cancel-is-discard-only");
      EXPECT(epocanva_backend_read_commit(session.handle, &session.request, &denied_commit, nullptr, 0),
             EPOCANVA_NO_COMMIT, "private-cancel-discards-pending-commit");
      cleared = Read(session, model);
      REQUIRE(cleared.raw_utf8.empty() && cleared.preedit_utf8.empty() && cleared.rows.empty(), "private-cancel-leaves-no-composition");
      ExpectPrediction(session, model, {});
      Unchanged(fixture, profile.language, before);
    }
  }
}

void Admission(Fixture& fixture) {
  auto binding = fixture.bindings[kEnglishQwerty];
  auto options = Init<epocanva_backend_profile_options>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
  options.profiles = &binding; options.profile_count = 1;
  std::string wrong_sha256 = kModels[0].sha256;
  wrong_sha256[0] = wrong_sha256[0] == '0' ? '1' : '0';
  binding.resource.model_sha256 = View(wrong_sha256);
  epocanva_backend_handle refused = 0;
  EXPECT(epocanva_backend_open_profiles(&options, &refused), EPOCANVA_BACKEND_INVALID_RESOURCE,
         "real-model-rejects-wrong-independent-digest");
  REQUIRE(refused == 0, "wrong-resource-has-no-backend");
  std::array<epocanva_backend_profile_resource, 2> duplicate{{fixture.bindings[kEnglishQwerty], fixture.bindings[kEnglishQwerty]}};
  options.profiles = duplicate.data(); options.profile_count = 2;
  EXPECT(epocanva_backend_open_profiles(&options, &refused), EPOCANVA_BACKEND_INVALID_PROFILE, "duplicate-profile-id-rejected");
  REQUIRE(refused == 0, "duplicate-id-has-no-backend");
  // A real model alone is not a Kanji converter. The main test backend always
  // includes the pinned decoder; this separate negative case must stay unavailable.
  binding = fixture.bindings[kRomajiKanji];
  binding.resource.conversion_path = {}; binding.resource.conversion_sha256 = {};
  options.profiles = &binding; options.profile_count = 1;
  epocanva_backend_handle model_only = 0;
  EXPECT(epocanva_backend_open_profiles(&options, &model_only), EPOCANVA_OK, "model-only-profile-resource-loads");
  auto session_options = Init<epocanva_backend_session_options>();
  session_options.context = {++next_context_target, 1, EPOCANVA_PRIVACY_NORMAL, 0};
  session_options.language = binding.profile.language; session_options.input_mode = binding.profile.input_mode;
  epocanva_backend_session_handle session = 0;
  const auto status = epocanva_backend_profile_session_open(model_only, &binding.profile, &session_options, &session);
  if (session) EXPECT(epocanva_backend_session_close(session), EPOCANVA_OK, "unexpected-model-only-session-cleanup");
  EXPECT(epocanva_backend_close(model_only), EPOCANVA_OK, "model-only-backend-close");
  EXPECT(status, EPOCANVA_BACKEND_UNAVAILABLE, "kanji-session-without-real-converter-unavailable");
  REQUIRE(session == 0, "no-fake-kanji-session");
}
void Reopened(const Backend& backend, const Fixture& fixture) {
  const auto snapshot = Disk(fixture, 1);
  const auto& model = fixture.Model(1);
  const auto good = Known(model, 1, "good", UINT64_C(11777008999757419033));
  const auto morning = Known(model, 1, "morning", UINT64_C(6271964512396262284));
  REQUIRE(snapshot.revision == 3 && snapshot.words.size() == 2 && snapshot.pairs.size() == 1 &&
      Count(snapshot, good) == 2 && Count(snapshot, morning) == 1 && PairCount(snapshot, good, morning) == 1,
      "reopen-retains-exact-confirmed-id-counts");
  pred::PersonalCounts personal;
  for (const auto& row : snapshot.words) personal.unigrams.push_back({row.id, row.count});
  for (const auto& row : snapshot.pairs) personal.bigrams.push_back({row.previous_id, row.id, row.count});
  Session session(backend, fixture.bindings[kEnglishDvorak].profile, true);
  auto learned = ExpectPrediction(session, model, {}, EPOCANVA_BACKEND_NEXT_WORD, &personal);
  REQUIRE(learned.rows.front().ids == std::vector<pred::LexemeId>{good}, "reopened-public-ranking-exposes-confirmed-learning");
  session.request.learning_enabled = 0;
  auto static_only = ExpectPrediction(session, model, {}, EPOCANVA_BACKEND_NEXT_WORD);
  REQUIRE(static_only.rows.front().ids != learned.rows.front().ids, "consent-off-does-not-read-personal-ranking");
  session.request.learning_enabled = 1;
  SetContext(session, "good");
  ExpectPrediction(session, model, {good}, EPOCANVA_BACKEND_NEXT_UNIT, &personal);
  Unchanged(fixture, 1, snapshot);
}

int Run(const fs::path& repository_root, const fs::path& run_root,
        std::string runtime_sha256) {
  try {
    Fixture fixture(repository_root, run_root, std::move(runtime_sha256));
    Case("independent-resource-admission", [&] { Admission(fixture); });
    {
      Backend backend(fixture);
      Case("same-language-keyboard-bindings", [&] { Bindings(backend, fixture); });
      Case("kana-only-next-output-unavailable", [&] { KanaNextOutputUnavailable(backend, fixture); });
      Case("real-converter-profile-modes", [&] { Conversions(backend, fixture); });
      Case("five-language-context-ranking-oracles", [&] { Contexts(backend, fixture); });
      Case("model-derived-abbreviation-context-flip", [&] { Abbreviations(backend, fixture); });
      Case("edit-context-cancel-token-invalidation", [&] { Edits(backend, fixture); });
      Case("ordinary-confirmation-dedup-no-learning", [&] { Confirmations(backend, fixture); });
      Case("sensitive-and-unknown-all-content-paths", [&] { Privacy(backend, fixture); });
      Case("ack-to-disk-confirmed-id-counts", [&] { Persistence(backend, fixture); });
      backend.Close();
    }
    {
      Backend reopened(fixture, false);
      Case("disk-reopen-personal-ranking-and-consent", [&] { Reopened(reopened, fixture); });
      reopened.Close();
    }
  } catch (const Failed&) {
  } catch (...) {
    ++failures; std::cerr << "FAIL condition=setup-or-cleanup-exception\n";
  }
  std::cout << (failures ? "FAIL" : "PASS") << " cases=" << passed_cases << " checks=" << checks
            << " failures=" << failures << " verified_assets=" << verified_assets << '\n';
  return failures ? 1 : 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--force-failure") {
    std::cerr << "FAIL forced=1 checks=1 failures=1\n";
    return 1;
  }
  if (argc != 3 && argc != 4) {
    std::cerr << "FAIL argument_count=" << argc << " expected=3-or-4\n";
    return 2;
  }
  // Keep native wchar paths until the resource/ABI boundary. The console code
  // page must not decide how a repository or isolated learning path is decoded.
  const auto runtime_sha256 = argc == 4 ? epocanva::product::WideToUtf8(argv[3]) : std::string(kRimeSha);
  return Run(fs::path(argv[1]), fs::path(argv[2]), runtime_sha256);
}
