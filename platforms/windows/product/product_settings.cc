#include "product_settings.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "product_io.h"

namespace epocanva {
namespace product {
namespace {

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool ParseIntExact(const std::string& text, int* value) {
  if (text.empty() || text.size() > 4) {
    return false;
  }
  int result = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    result = result * 10 + (ch - '0');
  }
  *value = result;
  return true;
}

bool ParseBoolExact(const std::string& text, bool* value) {
  if (text == "0") {
    *value = false;
    return true;
  }
  if (text == "1") {
    *value = true;
    return true;
  }
  return false;
}

SettingsLoadResult Corrupt(const wchar_t* reason) {
  SettingsLoadResult result;
  result.settings = ProductSettings::Defaults();
  result.status = SettingsLoadStatus::kCorrupt;
  result.diagnostic = reason;
  return result;
}

// A fixed key=value reader for the keys this product writes. It deliberately does
// not use GetPrivateProfileStringW: that API caches file contents per path, and an atomic
// replace cannot invalidate the cache without rewriting the file. Unknown keys are
// ignored for forward compatibility; a missing or duplicate known key is corruption.
// `candidate_display` is optional so settings files written before W3-B still load.
bool ReadSettingsMap(const std::filesystem::path& path,
                     std::map<std::string, std::string>* values,
                     std::wstring* why) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(path, &bytes, why)) {
    return false;
  }
  std::string text(bytes.begin(), bytes.end());
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  bool in_settings = false;
  bool section_seen = false;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::string raw =
        text.substr(start, newline == std::string::npos ? std::string::npos
                                                       : newline - start);
    std::string line = Trim(raw);
    start = newline == std::string::npos ? text.size() + 1 : newline + 1;
    if (line.empty() || line.front() == ';' || line.front() == '#') {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      in_settings = line == "[settings]";
      section_seen = section_seen || in_settings;
      continue;
    }
    if (!in_settings) {
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      throw std::runtime_error("a settings line has no '='");
    }
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (key.empty()) {
      throw std::runtime_error("a settings line has an empty key");
    }
    const bool known = key == "format" || key == "completion_enabled" ||
                       key == "completion_candidates" || key == "spelling_enabled" ||
                       key == "spelling_candidates" || key == "candidate_display" ||
                       key == "morphology_enabled" || key == "abbreviation_enabled" ||
                       key == "two_edit_spelling_enabled" || key == "profile_id";
    if (!known) {
      continue;
    }
    if (!values->emplace(key, value).second) {
      throw std::runtime_error("a settings key appears twice");
    }
  }
  if (!section_seen) {
    throw std::runtime_error("the [settings] section is missing");
  }
  return true;
}

}  // namespace

bool ProductSettings::valid() const {
  return profile_id >= 1 && profile_id <= 9999 && completion_candidates >= 1 && completion_candidates <= 7 &&
         spelling_candidates >= 1 && spelling_candidates <= 3;
}

SettingsLoadResult LoadProductSettings(const std::filesystem::path& path) {
  SettingsLoadResult result;
  result.settings = ProductSettings::Defaults();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    result.status = SettingsLoadStatus::kMissing;
    result.diagnostic = L"settings.ini does not exist; built-in defaults are shown";
    return result;
  }

  std::map<std::string, std::string> values;
  std::wstring read_why;
  try {
    if (!ReadSettingsMap(path, &values, &read_why)) {
      result.status = SettingsLoadStatus::kCorrupt;
      result.diagnostic = read_why.empty() ? L"settings.ini cannot be read"
                                           : read_why;
      return result;
    }
  } catch (const std::exception& error) {
    result.status = SettingsLoadStatus::kCorrupt;
    result.diagnostic = Utf8ToWide(std::string("settings.ini is malformed: ") +
                                   error.what());
    return result;
  }

  auto find = [&values](const char* key) -> const std::string* {
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
  };
  const std::string* format = find("format");
  if (format == nullptr) {
    return Corrupt(L"format is missing");
  }
  int format_number = 0;
  if (!ParseIntExact(*format, &format_number)) {
    return Corrupt(L"format is not a decimal integer");
  }
  if (format_number != 1) {
    result.status = SettingsLoadStatus::kUnsupportedVersion;
    result.diagnostic =
        L"settings.ini uses an unsupported format; built-in defaults are shown";
    return result;
  }

  const std::string* completion_enabled = find("completion_enabled");
  const std::string* completion_candidates = find("completion_candidates");
  const std::string* spelling_enabled = find("spelling_enabled");
  const std::string* spelling_candidates = find("spelling_candidates");
  if (completion_enabled == nullptr) {
    return Corrupt(L"completion_enabled is missing");
  }
  if (completion_candidates == nullptr) {
    return Corrupt(L"completion_candidates is missing");
  }
  if (spelling_enabled == nullptr) {
    return Corrupt(L"spelling_enabled is missing");
  }
  if (spelling_candidates == nullptr) {
    return Corrupt(L"spelling_candidates is missing");
  }

  ProductSettings loaded;
  if (!ParseBoolExact(*completion_enabled, &loaded.completion_enabled)) {
    return Corrupt(L"completion_enabled is not 0 or 1");
  }
  if (!ParseBoolExact(*spelling_enabled, &loaded.spelling_enabled)) {
    return Corrupt(L"spelling_enabled is not 0 or 1");
  }
  if (!ParseIntExact(*completion_candidates, &loaded.completion_candidates) ||
      loaded.completion_candidates < 1 || loaded.completion_candidates > 7) {
    return Corrupt(L"completion_candidates is outside 1..7");
  }
  if (!ParseIntExact(*spelling_candidates, &loaded.spelling_candidates) ||
      loaded.spelling_candidates < 1 || loaded.spelling_candidates > 3) {
    return Corrupt(L"spelling_candidates is outside 1..3");
  }

  const std::string* candidate_display = find("candidate_display");
  if (candidate_display != nullptr) {
    if (*candidate_display == "host") {
      loaded.candidate_display = CandidateDisplay::kHost;
    } else if (*candidate_display == "always_self") {
      loaded.candidate_display = CandidateDisplay::kAlwaysSelf;
    } else {
      return Corrupt(L"candidate_display is not host or always_self");
    }
  }

  const std::pair<const char*, bool*> optional_flags[] = {
      {"morphology_enabled", &loaded.morphology_enabled},
      {"abbreviation_enabled", &loaded.abbreviation_enabled},
      {"two_edit_spelling_enabled", &loaded.two_edit_spelling_enabled}};
  for (const auto& flag : optional_flags) {
    const std::string* value = find(flag.first);
    if (value != nullptr && !ParseBoolExact(*value, flag.second)) {
      return Corrupt(L"an extended English setting is not 0 or 1");
    }
  }

  const auto profile = find("profile_id");
  if (profile && (!ParseIntExact(*profile, &loaded.profile_id) || !loaded.valid()))
    return Corrupt(L"profile_id is invalid");
  result.settings = loaded;
  result.status = SettingsLoadStatus::kLoaded;
  return result;
}

const wchar_t* DescribeSettingsLoadStatus(SettingsLoadStatus status) {
  switch (status) {
    case SettingsLoadStatus::kLoaded:
      return L"loaded";
    case SettingsLoadStatus::kMissing:
      return L"missing (defaults)";
    case SettingsLoadStatus::kCorrupt:
      return L"corrupt (defaults)";
    case SettingsLoadStatus::kUnsupportedVersion:
      return L"unsupported version (defaults)";
  }
  return L"unknown";
}

std::string BuildSettingsText(const ProductSettings& settings) {
  std::string text;
  text += "; EpoCanva settings, format 1.\n";
  text += "; Written by EpoCanva.exe. The input engine applies these values at its next\n";
  text += "; start, after the generated Rime patch is deployed.\n";
  text += "[settings]\n";
  text += "format=1\n";
  text += "profile_id=" + std::to_string(settings.profile_id) + "\n";
  text += "completion_enabled=";
  text += settings.completion_enabled ? "1" : "0";
  text += "\ncompletion_candidates=";
  text += std::to_string(settings.completion_candidates);
  text += "\nspelling_enabled=";
  text += settings.spelling_enabled ? "1" : "0";
  text += "\nspelling_candidates=";
  text += std::to_string(settings.spelling_candidates);
  text += "\ncandidate_display=";
  text += settings.candidate_display == CandidateDisplay::kAlwaysSelf ? "always_self"
                                                                      : "host";
  text += "\nmorphology_enabled=";
  text += settings.morphology_enabled ? "1" : "0";
  text += "\nabbreviation_enabled=";
  text += settings.abbreviation_enabled ? "1" : "0";
  text += "\ntwo_edit_spelling_enabled=";
  text += settings.two_edit_spelling_enabled ? "1" : "0";
  text += "\n";
  return text;
}

bool SaveProductSettings(const std::filesystem::path& path,
                         const ProductSettings& settings,
                         std::string* settings_sha256, std::wstring* why) {
  if (!settings.valid()) {
    if (why != nullptr) {
      *why = L"refusing to save an out-of-range settings value";
    }
    return false;
  }
  const std::string text = BuildSettingsText(settings);
  if (!WriteTextAtomically(path, text, why)) {
    return false;
  }
  if (settings_sha256 != nullptr) {
    if (!Sha256(text.data(), text.size(), settings_sha256, why)) {
      return false;
    }
  }
  return true;
}

std::string BuildSchemaPatch(const ProductSettings& settings) {
  const int completions =
      settings.completion_enabled ? settings.completion_candidates : 0;
  const int suggestions =
      settings.spelling_enabled ? settings.spelling_candidates : 0;
  std::string patch;
  patch += "# Generated by EpoCanva.exe. Do not edit; the next save overwrites this file.\n";
  patch += "# Applied by the engine at its next start.\n";
  patch += "patch:\n";
  patch += "  \"english/max_completions\": " + std::to_string(completions) + "\n";
  patch += "  \"english/enable_suggestions\": ";
  patch += settings.spelling_enabled ? "true" : "false";
  patch += "\n  \"english/max_suggestions\": " + std::to_string(suggestions) + "\n";
  patch += "  \"english/enable_morphology\": ";
  patch += settings.morphology_enabled ? "true\n" : "false\n";
  patch += "  \"english/enable_abbreviation\": ";
  patch += settings.abbreviation_enabled ? "true\n" : "false\n";
  patch += "  \"english/enable_two_edit_spelling\": ";
  patch += settings.two_edit_spelling_enabled ? "true\n" : "false\n";
  return patch;
}

bool SaveSettingsBundle(const ProductPaths& paths, const ProductSettings& settings,
                        std::string* settings_sha256, std::wstring* why) {
  if (!EnsureUserDirectories(paths, why)) {
    return false;
  }
  if (!SaveProductSettings(paths.settings_file, settings, settings_sha256, why)) {
    return false;
  }
  const std::string patch = BuildSchemaPatch(settings);
  if (!WriteTextAtomically(paths.schema_patch_file, patch, why)) {
    return false;
  }
  const std::string marker =
      settings_sha256 != nullptr ? *settings_sha256 + "\n" : std::string();
  if (marker.empty()) {
    if (why != nullptr) {
      *why = L"the settings digest is unavailable, so no deployment can be requested";
    }
    return false;
  }
  if (!WriteTextAtomically(paths.deploy_marker_file, marker, why)) {
    ::DeleteFileW(paths.deploy_marker_file.c_str());
    return false;
  }
  return true;
}

bool ReadDeployMarkerToken(const std::filesystem::path& marker_path,
                           std::string* token) {
  std::vector<std::uint8_t> bytes;
  std::wstring why;
  if (!ReadFileBytes(marker_path, &bytes, &why)) {
    return false;
  }
  std::string text(bytes.begin(), bytes.end());
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
          text.back() == '\t')) {
    text.pop_back();
  }
  if (!IsLowercaseHexSha256(text)) {
    return false;
  }
  *token = text;
  return true;
}

bool ClearDeployMarkerIfToken(const std::filesystem::path& marker_path,
                              const std::string& token) {
  std::error_code ec;
  if (!std::filesystem::exists(marker_path, ec) || ec) {
    return true;
  }
  std::string current;
  if (!ReadDeployMarkerToken(marker_path, &current)) {
    return false;
  }
  if (current != token) {
    return false;
  }
  if (::DeleteFileW(marker_path.c_str())) {
    return true;
  }
  return ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

}  // namespace product
}  // namespace epocanva
