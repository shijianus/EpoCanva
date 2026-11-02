#include "dictionary.h"
#include "test_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
// Reference engine used ONLY in tests; never exposed through the public wrapper.
#include "hunspell.hxx"

namespace fr = epocanva::french_dictionary;
namespace fs = std::filesystem;
namespace {

class CheckFailure : public std::exception {};
void Check(bool condition, int line) {
  if (!condition) {
    // No raw input/result/path is printed, even on assertion failure.
    std::cout << "ASSERTION FAILED line " << line << '\n';
    throw CheckFailure();
  }
}
#define CHECK(value) Check(static_cast<bool>(value), __LINE__)

fr::ExpectedHashes ProductionHashes() {
  return {test_config::kAffHash, test_config::kDicHash};
}
fs::path Aff() { return fs::u8path(test_config::kAffPath); }
fs::path Dic() { return fs::u8path(test_config::kDicPath); }
std::shared_ptr<const fr::Dictionary> LoadProduction() {
  fr::LoadInfo info;
  auto dictionary = fr::Dictionary::Load(Aff(), Dic(), ProductionHashes(), &info);
  if (!dictionary) std::cout << "LOAD STATUS " << fr::StatusName(info.status) << '\n';
  CHECK(dictionary);
  CHECK(info.status == fr::Status::kOk);
  CHECK(info.aff_hash_verified && info.dic_hash_verified);
  CHECK(dictionary->available());
  return dictionary;
}
bool Contains(const fr::ListResult& result, std::string_view expected) {
  return std::find(result.values_utf8.begin(), result.values_utf8.end(), expected) !=
         result.values_utf8.end();
}
void Correct(const fr::Dictionary& dictionary, std::string_view word) {
  const auto result = dictionary.Spell(word);
  CHECK(result.status == fr::Status::kOk);
  CHECK(result.correct && !result.forbidden);
}
void Incorrect(const fr::Dictionary& dictionary, std::string_view word) {
  const auto result = dictionary.Spell(word);
  CHECK(result.status == fr::Status::kOk);
  CHECK(!result.correct);
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  CHECK(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string Hash(const std::string& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  CHECK(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
  std::array<unsigned char, 32> digest{};
  const auto status = BCryptHash(algorithm, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  CHECK(status >= 0);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : digest) {
    result.push_back(hex[byte >> 4u]); result.push_back(hex[byte & 15u]);
  }
  return result;
}
void WriteBytes(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  CHECK(stream.good());
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  CHECK(stream.good());
}
struct Pair {
  fs::path aff;
  fs::path dic;
  fr::ExpectedHashes hashes;
};
Pair MakePair(std::string_view test_name, const std::string& aff, const std::string& dic) {
  const auto root = fs::u8path(test_config::kScratchPath) / fs::u8path(test_name);
  Pair pair{root / "rules.aff", root / "explicitly-paired-words.dic", {Hash(aff), Hash(dic)}};
  WriteBytes(pair.aff, aff); WriteBytes(pair.dic, dic);
  return pair;
}
std::shared_ptr<const fr::Dictionary> LoadPair(const Pair& pair) {
  fr::LoadInfo info;
  auto dictionary = fr::Dictionary::Load(pair.aff, pair.dic, pair.hashes, &info);
  if (!dictionary) std::cout << "LOAD STATUS " << fr::StatusName(info.status) << '\n';
  CHECK(dictionary);
  CHECK(info.status == fr::Status::kOk);
  return dictionary;
}
std::string NativePath(const fs::path& path) {
  return fs::path(L"\\\\?\\" + fs::absolute(path).make_preferred().native()).u8string();
}
std::unique_ptr<Hunspell> Reference() {
  return std::make_unique<Hunspell>(NativePath(Aff()).c_str(), NativePath(Dic()).c_str());
}

void ProductionLoad() {
  const auto dictionary = LoadProduction();
  const auto& source = dictionary->source();
  CHECK(source.available && source.hashes_verified);
  CHECK(source.hunspell_revision == test_config::kRevision);
  CHECK(source.hashes.aff_sha256 == test_config::kAffHash);
  CHECK(source.hashes.dic_sha256 == test_config::kDicHash);
  CHECK(source.dictionary_entry_hint == 84782);
  CHECK(source.dictionary_entry_records == 84749);
  // Broad actual vocabulary, not fixed tiny candidate lists.
  for (const auto word : {u8"bonjour", u8"bibliothèque", u8"électroencéphalogramme",
                          u8"hippopotame", u8"rhétorique", u8"métamorphose"}) {
    Correct(*dictionary, word);
  }
  const auto aff = ReadBytes(Aff());
  for (const auto directive : {"FLAG long", "FULLSTRIP", "NEEDAFFIX", "FORBIDDENWORD",
                               "CIRCUMFIX", "KEEPCASE", "NOSUGGEST", "PFX ", "SFX ",
                               "ICONV", "OCONV", "MAP ", "REP ", "BREAK "}) {
    CHECK(aff.find(directive) != std::string::npos);
  }
}
void Accents() {
  const auto dictionary = LoadProduction();
  for (const auto word : {u8"école", u8"français", u8"élève", u8"cœur", u8"œuvre",
                          u8"garçon", u8"ÉCOLE", u8"e\u0301cole", u8"l’école"}) {
    Correct(*dictionary, word);
  }
  for (const auto pair : {std::pair{u8"ecole", u8"école"},
                         std::pair{u8"francais", u8"français"},
                         std::pair{u8"eleve", u8"élève"},
                         std::pair{u8"coeur", u8"cœur"},
                         std::pair{u8"garcon", u8"garçon"}}) {
    const auto result = dictionary->Suggest(pair.first, fr::kMaxResults);
    CHECK(result.status == fr::Status::kOk);
    CHECK(Contains(result, pair.second));
  }
  const auto elision = dictionary->Suggest(u8"l'ecole", fr::kMaxResults);
  CHECK(elision.status == fr::Status::kOk);
  CHECK(Contains(elision, u8"l’école"));  // Native OCONV typographic apostrophe.
}
void Plurals() {
  const auto dictionary = LoadProduction();
  for (const auto word : {u8"cheval", u8"chevaux", u8"travaux", u8"bijoux", u8"bateaux",
                          u8"yeux", u8"journaux", u8"écoles", u8"petites"}) Correct(*dictionary, word);
  // "travails" is an admitted specialist plural: the real row is travail/X.S.
  // Do not impose an invented generic -al plural rule over the dictionary.
  Correct(*dictionary, u8"travails");
  for (const auto word : {u8"chevals", u8"journals", u8"écoleses"}) Incorrect(*dictionary, word);
  CHECK(Contains(dictionary->Suggest(u8"écoleses", fr::kMaxResults), u8"écoles"));
  CHECK(Contains(dictionary->Stem(u8"écoles"), u8"école"));
}
void Conjugation() {
  const auto dictionary = LoadProduction();
  for (const auto word : {u8"mangeons", u8"mangeraient", u8"finissons", u8"allaient",
                          u8"sommes", u8"étaient", u8"faisaient", u8"construisirent"}) Correct(*dictionary, word);
  for (const auto word : {u8"mangeont", u8"finissonses", u8"construirent"}) Incorrect(*dictionary, word);
  CHECK(Contains(dictionary->Suggest(u8"construirent", fr::kMaxResults), u8"construisirent"));
  CHECK(Contains(dictionary->Stem(u8"mangeons"), u8"manger"));
  const auto analysis = dictionary->Analyze(u8"mangeons");
  CHECK(analysis.status == fr::Status::kOk && analysis.input_correct);
  CHECK(!analysis.values_utf8.empty());
  bool has_stem = false;
  for (const auto& record : analysis.values_utf8) has_stem |= record.find("st:manger") != std::string::npos;
  CHECK(has_stem);
  const auto invalid = dictionary->Generate(u8"manger", u8"finissonses");
  CHECK(invalid.status == fr::Status::kInvalidWord && invalid.values_utf8.empty());
}
void NativeParity() {
  const auto dictionary = LoadProduction();
  const auto native = Reference();
  // Choose unambiguous corrections: native search has time cutoffs, so marginal
  // alternatives on ambiguous words need not be identical between two calls.
  for (const auto word : {u8"ecole", u8"construirent", u8"l'ecole", u8"coeur"}) {
    CHECK(dictionary->Suggest(word, fr::kMaxResults).values_utf8 == native->suggest(std::string(word)));
  }
  for (const auto word : {u8"mangeons", u8"chevaux", u8"petites", u8"écoles"}) {
    CHECK(dictionary->Analyze(word, fr::kMaxResults).values_utf8 == native->analyze(std::string(word)));
    CHECK(dictionary->Stem(word, fr::kMaxResults).values_utf8 == native->stem(std::string(word)));
  }
  for (const auto pair : {std::pair{u8"école", u8"maisons"}, std::pair{u8"manger", u8"finissons"},
                         std::pair{u8"cheval", u8"bateaux"}}) {
    const auto generated = dictionary->Generate(pair.first, pair.second, fr::kMaxResults);
    CHECK(generated.status == fr::Status::kOk);
    const auto reference = native->generate(std::string(pair.first), std::string(pair.second));
    CHECK(generated.values_utf8 == reference);
    for (const auto& form : generated.values_utf8) Correct(*dictionary, form);
  }
}
void Protection() {
  const auto dictionary = LoadProduction();
  for (const auto input : {u8"https://exemple.fr/ecole", u8"www.exemple.fr", u8"user@exemple.fr",
                           u8"C:\\ecole\\mot.txt", u8"/tmp/ecole", u8"ecole.cpp", u8"get_value",
                           u8"ecole()", u8"a->ecole", u8"ecole42", u8"123456", u8"bonjour monde",
                           u8"<?xml?><query type='add'><word>secret</word></query>", u8"ecole\nsecret",
                           u8"ecole\u00a0secret", u8"ecole９"}) {
    const auto result = dictionary->Suggest(input);
    CHECK(result.status == fr::Status::kProtectedText);
    CHECK(result.values_utf8.empty());
    CHECK(dictionary->Analyze(input).values_utf8.empty());
    CHECK(dictionary->Stem(input).values_utf8.empty());
    CHECK(dictionary->Generate(input, u8"écoles").values_utf8.empty());
    CHECK(dictionary->Generate(u8"école", input).values_utf8.empty());
  }
  const std::string nul("ecole\0private", 13);
  CHECK(dictionary->Suggest(nul).status == fr::Status::kProtectedText);
  const auto before = dictionary->Spell("zqxvprivate");
  dictionary->Suggest("<?xml?><query type='add'><word>zqxvprivate</word></query>");
  CHECK(!before.correct && !dictionary->Spell("zqxvprivate").correct);
}
void Bounds() {
  const auto dictionary = LoadProduction();
  const auto full = dictionary->Suggest(u8"mot", fr::kMaxResults);
  CHECK(full.values_utf8.size() > 1);
  const auto first = dictionary->Suggest(u8"mot", 1);
  CHECK(first.values_utf8.size() == 1 && first.truncated);
  CHECK(first.values_utf8.front() == full.values_utf8.front());
  CHECK(dictionary->Suggest(u8"ecole", 0).values_utf8.empty());
  CHECK(dictionary->Analyze(u8"mangeons", 0).values_utf8.empty());
  CHECK(dictionary->Stem(u8"mangeons", 0).values_utf8.empty());
  CHECK(dictionary->Generate(u8"école", u8"maisons", 0).values_utf8.empty());
  CHECK(dictionary->Suggest(u8"mot", std::numeric_limits<std::size_t>::max()).values_utf8.size() <= fr::kMaxResults);
  CHECK(dictionary->Suggest("").status == fr::Status::kEmptyInput);
  CHECK(dictionary->Suggest(std::string(fr::kMaxQueryUtf8Bytes + 1, 'a')).status == fr::Status::kInputTooLong);
  std::string multibyte;
  for (int record = 0; record < 129; ++record) multibyte += u8"é";
  CHECK(multibyte.size() == 258);
  CHECK(dictionary->Suggest(multibyte).status == fr::Status::kInputTooLong);
  for (const auto& malformed : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                std::string("\xf4\x90\x80\x80", 4), std::string("\xc3", 1),
                                std::string("\x80", 1), std::string("url/\xc3", 5)}) {
    CHECK(dictionary->Suggest(malformed).status == fr::Status::kInvalidUtf8);
    CHECK(dictionary->Spell(malformed).status == fr::Status::kInvalidUtf8);
  }
  CHECK(dictionary->Suggest(u8"\U0001f600").status == fr::Status::kInvalidWord);
}
void Availability() {
  const auto unavailable = fr::Dictionary::Unavailable();
  CHECK(!unavailable->available() && !unavailable->source().hashes_verified);
  CHECK(unavailable->Spell(u8"bonjour").status == fr::Status::kUnavailable);
  CHECK(unavailable->Suggest(u8"ecole").status == fr::Status::kUnavailable);
  CHECK(unavailable->Analyze(u8"mangeons").status == fr::Status::kUnavailable);
  CHECK(unavailable->Stem(u8"chevaux").status == fr::Status::kUnavailable);
  CHECK(unavailable->Generate(u8"école", u8"maisons").status == fr::Status::kUnavailable);
}
void Failures() {
  const auto missing = fs::u8path(test_config::kScratchPath) / "never-created.dic";
  CHECK(!fs::exists(missing));
  fr::LoadInfo info;
  CHECK(!fr::Dictionary::Load(missing, Dic(), ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kMissingFile && !info.aff_hash_verified);
  CHECK(!fr::Dictionary::Load(Aff(), missing, ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kMissingFile && info.aff_hash_verified && !info.dic_hash_verified);
  auto hashes = ProductionHashes(); hashes.dic_sha256[0] = '0';
  CHECK(!fr::Dictionary::Load(Aff(), Dic(), hashes, &info));
  CHECK(info.status == fr::Status::kHashMismatch && info.aff_hash_verified && !info.dic_hash_verified);
  hashes = ProductionHashes(); hashes.aff_sha256[0] = '0';
  CHECK(!fr::Dictionary::Load(Aff(), Dic(), hashes, &info));
  CHECK(info.status == fr::Status::kHashMismatch && !info.aff_hash_verified);
  for (const auto& invalid : {fr::ExpectedHashes{}, fr::ExpectedHashes{test_config::kAffHash, ""},
                              fr::ExpectedHashes{"NOT-A-HASH", test_config::kDicHash}}) {
    CHECK(!fr::Dictionary::Load(Aff(), Dic(), invalid, &info));
    CHECK(info.status == fr::Status::kInvalidOptions);
  }
  CHECK(!fr::Dictionary::Load("https://invalid.example/fr.aff", Dic(), ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kInvalidOptions);
  CHECK(!fr::Dictionary::Load(L"\\\\invalid.example\\dict\\fr.aff", Dic(), ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kInvalidOptions);
  CHECK(!fr::Dictionary::Load(fs::path(Aff().native() + L":stream"), Dic(), ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kInvalidOptions);

  const auto modified = MakePair("corrupt_hash", ReadBytes(Aff()), ReadBytes(Dic()) + "extra-record\n");
  CHECK(!fr::Dictionary::Load(modified.aff, modified.dic, ProductionHashes(), &info));
  CHECK(info.status == fr::Status::kHashMismatch);
  const auto empty = MakePair("empty", "", "1\nbonjour\n");
  CHECK(!fr::Dictionary::Load(empty.aff, empty.dic, empty.hashes, &info));
  CHECK(info.status == fr::Status::kInvalidDictionary);
  const auto bad_header = MakePair("bad_header", "SET UTF-8\n", "not-a-count\nbonjour\n");
  CHECK(!fr::Dictionary::Load(bad_header.aff, bad_header.dic, bad_header.hashes, &info));
  CHECK(info.status == fr::Status::kInvalidDictionary);
  const auto bad_encoding = MakePair("bad_encoding", "SET ISO8859-1\n", "1\nbonjour\n");
  CHECK(!fr::Dictionary::Load(bad_encoding.aff, bad_encoding.dic, bad_encoding.hashes, &info));
  CHECK(info.status == fr::Status::kUnsupportedEncoding);
  const auto invalid_utf8 = MakePair("invalid_utf8", "SET UTF-8\n", "1\n\xc0\xaf\n");
  CHECK(!fr::Dictionary::Load(invalid_utf8.aff, invalid_utf8.dic, invalid_utf8.hashes, &info));
  CHECK(info.status == fr::Status::kInvalidDictionary);
  const auto long_line = MakePair("long_line", "SET UTF-8\n#" + std::string(65536, 'x') + "\n", "1\nbonjour\n");
  CHECK(!fr::Dictionary::Load(long_line.aff, long_line.dic, long_line.hashes, &info));
  CHECK(info.status == fr::Status::kInvalidDictionary);
  const auto missing_entry = MakePair("missing_entry", "SET UTF-8\n", "1\n");
  CHECK(!fr::Dictionary::Load(missing_entry.aff, missing_entry.dic, missing_entry.hashes, &info));
  CHECK(info.status == fr::Status::kInvalidDictionary);
  const auto valid = MakePair("non_sibling", "SET UTF-8\n", "1\nbonjour\n");
  CHECK(!fs::exists(valid.aff.parent_path() / "rules.dic"));
  Correct(*LoadPair(valid), "bonjour"); // Explicit .dic path, no sibling guessing.
}

void ComplexRules() {
  // Small independent fixtures exercise features that may have no reachable
  // French entry. They supplement, NEVER replace, the full production tests.
  const auto pair = MakePair("complex_rules", u8R"(SET UTF-8
FLAG long
TRY abcdefghijklmnopqrstuvwxyzé
FULLSTRIP
NEEDAFFIX NA
FORBIDDENWORD FW
CIRCUMFIX CX
KEEPCASE KC
NOSUGGEST NS
ICONV 1
ICONV ’ '
OCONV 1
OCONV ' ’
MAP 1
MAP eé
REP 1
REP f ph
BREAK 1
BREAK -
PFX PX Y 1
PFX PX 0 pre .
SFX SX Y 1
SFX SX 0 s . is:plural
SFX FS Y 1
SFX FS aller vais aller
SFX NR Y 1
SFX NR 0 er .
PFX CP Y 1
PFX CP 0 ge/CX .
SFX CS Y 1
SFX CS 0 en/CX .
)", u8R"(13
mot/PXSX	po:noun
nom/SX	po:noun
aller/FS
radic/NANR
secret/FW
widget/KC
zqxv/NS
wurzel/CPCS
phare
école
l'école
alpha
beta
)");
  const auto dictionary = LoadPair(pair);
  for (const auto word : {u8"premots", u8"vais", u8"radicer", u8"gewurzelen",
                          u8"widget", u8"zqxv", u8"l’école", u8"alpha-beta"}) Correct(*dictionary, word);
  for (const auto word : {u8"radic", u8"secret", u8"gewurzel", u8"wurzelen", u8"Widget"}) Incorrect(*dictionary, word);
  CHECK(dictionary->Spell("secret").forbidden);
  const auto hidden = dictionary->Suggest("zqxw", fr::kMaxResults);
  CHECK(!Contains(hidden, "zqxv"));
  CHECK(Contains(dictionary->Suggest("fare", fr::kMaxResults), "phare"));
  CHECK(Contains(dictionary->Suggest("ecole", fr::kMaxResults), u8"école"));
  CHECK(Contains(dictionary->Generate("nom", "mots"), "noms"));
  const auto tagged = dictionary->Analyze("noms");
  CHECK(!tagged.values_utf8.empty() && tagged.values_utf8.front().find("po:noun") != std::string::npos);
  CHECK(Contains(dictionary->Stem("premots"), "mot"));
  CHECK(dictionary->Generate("radic", "mots").status == fr::Status::kInvalidWord);
}
void Concurrency() {
  const auto dictionary = LoadProduction();
  const auto expected = dictionary->Suggest(u8"francais");
  CHECK(Contains(expected, u8"français"));
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  for (int thread_ordinal = 0; thread_ordinal < 6; ++thread_ordinal) {
    workers.emplace_back([&] {
      for (int repeat = 0; repeat < 20; ++repeat) {
        const auto suggestion = dictionary->Suggest(u8"francais");
        // Native time-budget cutoffs can change marginal alternatives. Require
        // the same real correction and valid bounded output, not an identical
        // full list that Hunspell does not promise even on one thread.
        if (!dictionary->Spell(u8"mangeons").correct ||
            suggestion.status != fr::Status::kOk || !Contains(suggestion, u8"français") ||
            suggestion.values_utf8.size() > 16 ||
            !Contains(dictionary->Stem(u8"écoles"), u8"école") ||
            dictionary->Analyze(u8"mangeons").values_utf8.empty()) failed = true;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  CHECK(!failed);
}
void StaticLoad() {
  const auto pair = MakePair(u8"static_load_é_漢字", ReadBytes(Aff()), ReadBytes(Dic()));
  const auto dictionary = LoadPair(pair);
  CHECK(fs::remove(pair.aff)); CHECK(fs::remove(pair.dic));
  Correct(*dictionary, u8"mangeons");
  CHECK(Contains(dictionary->Suggest(u8"ecole"), u8"école"));
  CHECK(Contains(dictionary->Stem(u8"écoles"), u8"école"));
  CHECK(fs::is_empty(pair.aff.parent_path()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::cout << "INVALID MODE\n"; return 2; }
  const std::string_view mode(argv[1]);
  try {
    if (mode == "production_load") ProductionLoad();
    else if (mode == "accents") Accents();
    else if (mode == "plurals") Plurals();
    else if (mode == "conjugation") Conjugation();
    else if (mode == "native_parity") NativeParity();
    else if (mode == "protection") Protection();
    else if (mode == "bounds") Bounds();
    else if (mode == "availability") Availability();
    else if (mode == "failures") Failures();
    else if (mode == "complex_rules") ComplexRules();
    else if (mode == "concurrency") Concurrency();
    else if (mode == "static_load") StaticLoad();
    else if (mode == "force_failure") CHECK(false);
    else { std::cout << "INVALID MODE\n"; return 2; }
    std::cout << "RESULT PASS\n";
    return 0;
  } catch (...) {
    std::cout << "RESULT FAIL\n";
    return 1;
  }
}
