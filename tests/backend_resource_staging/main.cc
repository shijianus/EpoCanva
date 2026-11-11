// Loads real staged bytes with the production decoder; no fixture model or
// alternate decoding implementation. Collection indices count TSV records;
// all string offsets and spans below count UTF-8 code units (bytes).
#include "engine/prediction/abbreviation.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace p = epocanva::prediction;
namespace a = epocanva::prediction::abbreviation;

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  Require(static_cast<bool>(file), "staged file missing");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Expect(const a::Decoder& decoder, const std::string& language,
            const std::string& key_utf8, const std::vector<p::LexemeId>& ids) {
  a::Options options;
  options.enable_initialism = false;
  options.max_trie_nodes = a::kMaxResourceTrieNodes;
  options.max_explicit_expansions = a::kMaxResourceRecords;
  const auto result = decoder.Decode(key_utf8, {}, language, options);
  Require(result.status == p::Status::kOk && result.complete, "staged key decoding failed");
  bool found = false;
  for (const auto& candidate : result.suggestions) {
    if (candidate.lexeme_ids == ids) found = true;
  }
  Require(found, "staged expansion IDs missing from decoded suggestions");
}
void CheckLanguage(const std::filesystem::path& root, const std::string& language,
                   std::size_t expected_records) {
  const auto directory = root / "data" / "backend" / language;
  p::LoadOptions loading;
  loading.allow_test_fixture = false;
  const auto model = p::Model::Load(directory / "model.tsv", loading);
  Require(model && model->available(), "production model rejected");
  Require(model->metadata().source == p::SourceKind::kProductionCorpus, "nonproduction model");
  const auto source = Read(directory / "abbreviations.tsv");
  a::BuildInfo info;
  const auto decoder = a::Decoder::Build(model, source, {}, &info);
  Require(decoder && info.status == p::Status::kOk, "production decoder rejected staged abbreviations");
  Require(info.explicit_expansions == expected_records, "unexpected expansion record count");
  std::size_t begin_utf8_byte = source.find('\n') + 1;
  std::size_t records = 0;
  while (begin_utf8_byte < source.size()) {
    const auto end_utf8_byte = source.find('\n', begin_utf8_byte);
    Require(end_utf8_byte != std::string::npos, "missing final LF");
    const auto line = source.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte);
    const auto tab1_utf8_byte = line.find('\t');
    const auto tab2_utf8_byte = line.find('\t', tab1_utf8_byte + 1);
    Require(tab1_utf8_byte != std::string::npos && tab2_utf8_byte != std::string::npos,
            "missing abbreviation columns");
    Require(line.substr(0, tab1_utf8_byte) == language, "wrong resource language");
    const auto key_utf8 = line.substr(tab1_utf8_byte + 1, tab2_utf8_byte - tab1_utf8_byte - 1);
    std::vector<p::LexemeId> ids;
    std::size_t id_begin_utf8_byte = tab2_utf8_byte + 1;
    for (;;) {
      const auto space_utf8_byte = line.find(' ', id_begin_utf8_byte);
      const auto id = std::stoull(line.substr(id_begin_utf8_byte,
          space_utf8_byte == std::string::npos ? std::string::npos :
          space_utf8_byte - id_begin_utf8_byte));
      const auto* word = model->Find(id);
      Require(word && word->kind == p::TokenKind::kWord && word->language_id == language,
              "expansion does not resolve to model words");
      ids.push_back(id);
      if (space_utf8_byte == std::string::npos) break;
      id_begin_utf8_byte = space_utf8_byte + 1;
    }
    Expect(*decoder, language, key_utf8, ids);
    // Irregular shorthand must also work before the full key is typed. These
    // substrings end on known ASCII scalar boundaries, never inside UTF-8.
    if (key_utf8 == "asap") Expect(*decoder, language, "asa", ids);
    if (key_utf8 == "mme") Expect(*decoder, language, "mm", ids);
    if (key_utf8 == "c.-\xc3\xa0-d.") Expect(*decoder, language, "c.-\xc3\xa0", ids);
    ++records;
    begin_utf8_byte = end_utf8_byte + 1;
  }
  Require(records == expected_records, "resource record count changed");
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    const auto root = std::filesystem::u8path(argv[1]);
    CheckLanguage(root, "en", 36);
    CheckLanguage(root, "fr", 4);
    std::cout << "RESULT PASS\n";
    return 0;
  } catch (const std::exception&) {
    std::cerr << "staged abbreviation decoder check failed\n";
    return 1;
  }
}
