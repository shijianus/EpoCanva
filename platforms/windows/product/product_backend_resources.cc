#include "product_backend_resources.h"
#include "product_io.h"
#include "../local_path/local_path.h"
#include <fstream>
#include <set>
#include <sstream>
#include <limits>
namespace epocanva::product {
namespace {
const char* kColumns = "profile_id\tlanguage\tinput_mode\toutput_mode\tkeyboard_layout\tvariant_id\tmodel_path\tmodel_sha256\tconversion_path\tconversion_sha256\tauxiliary_path\tauxiliary_sha256\tabbreviation_path\tabbreviation_sha256";
bool Number(const std::string& text, std::uint32_t* out) {
  if (text.empty() || text.size() > 9) return false;
  std::uint32_t value = 0;
  for (const auto ch : text) { if (ch < '0' || ch > '9') return false; value = value * 10 + ch - '0'; }
  *out = value; return true;
}
bool ResourcePath(const ProductPaths& paths, const std::string& relative, const std::string& hash,
                  bool optional, std::filesystem::path* path, std::string* digest) {
  if (optional && relative == "-" && hash == "-") { path->clear(); digest->clear(); return true; }
  if (relative.empty() || relative == "-" || relative.front() == '/' || relative.back() == '/' ||
      relative.find('\\') != relative.npos || relative.find(':') != relative.npos ||
      relative.find('\0') != relative.npos || !IsLowercaseHexSha256(hash)) return false;
  std::istringstream parts(relative); std::string component;
  while (std::getline(parts, component, '/')) if (component.empty() || component == "." || component == "..") return false;
  *path = paths.product_root / std::filesystem::u8path(relative); *digest = hash;
  return path->is_absolute();  // backend pins every component and validates bytes before loading
}
}
bool LoadBackendResources(const ProductPaths& paths, windows::engine::UnifiedBackendConfig* config,
                           std::wstring* why) {
  if (!config) return false;
  const auto file = paths.data_dir / L"backend" / L"resources.tsv";
  auto fail = [why]() { if (why) *why = L"backend resource manifest is missing, invalid or not local"; return false; };
  windows::local_path::PreparedPath prepared;
  if (!windows::local_path::Prepare(file, &prepared)) return fail();
  windows::local_path::PinnedPath manifest_pin;
  if (!manifest_pin.Open(prepared, false)) return fail();
  std::ifstream stream(manifest_pin.path(), std::ios::binary | std::ios::ate);
  if (!stream || stream.tellg() < 0 || stream.tellg() > 65536) return fail();
  stream.seekg(0); std::string line;
  auto next = [&]() { if (!std::getline(stream, line)) return false; if (!line.empty() && line.back() == '\r') line.pop_back(); return true; };
  if (!next() || line != "epocanva-backend-resources\t1" || !next() || line != kColumns) return fail();
  windows::engine::UnifiedBackendConfig out; out.enabled = true;
  std::set<std::uint32_t> ids;
  while (next()) {
    if (line.empty()) continue;
    std::vector<std::string> fields; std::size_t begin = 0;
    for (;;) { const auto end = line.find('\t', begin); fields.push_back(line.substr(begin, end - begin)); if (end == line.npos) break; begin = end + 1; }
    if (fields.size() != 14 || out.profiles.size() == 32) return fail();
    windows::engine::BackendResourceConfig resource; auto& p = resource.profile;
    if (!Number(fields[0], &p.profile_id) || !p.profile_id || !ids.insert(p.profile_id).second ||
        !Number(fields[1], &p.language) || p.language < 1 || p.language > 5 ||
        !Number(fields[2], &p.input_mode) || !Number(fields[3], &p.output_mode) ||
        !Number(fields[4], &p.keyboard_layout)) return fail();
    if (fields[5].empty() || fields[5].size() > 64 || fields[5].find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != fields[5].npos) return fail();
    p.variant_id = fields[5];
    if (!ResourcePath(paths, fields[6], fields[7], false, &resource.model_path, &resource.model_sha256) ||
        !ResourcePath(paths, fields[8], fields[9], true, &resource.conversion_path, &resource.conversion_sha256) ||
        !ResourcePath(paths, fields[10], fields[11], true, &resource.auxiliary_path, &resource.auxiliary_sha256) ||
        !ResourcePath(paths, fields[12], fields[13], true, &p.abbreviation_path, &p.abbreviation_sha256)) return fail();
    out.use_rime = out.use_rime || p.language == EPOCANVA_BACKEND_ZH;
    out.profiles.push_back(std::move(resource));
  }
  if (out.profiles.empty()) return fail();
  *config = std::move(out); return true;
}
}
