#include "backend_manifest.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "backend_abi_values.h"

namespace epocanva {
namespace locale {
namespace {

const char* LanguageEnumName(std::uint32_t value) {
  switch (value) {
    case abi_values::kLanguageEn: return "EPOCANVA_BACKEND_EN";
    case abi_values::kLanguageZh: return "EPOCANVA_BACKEND_ZH";
    case abi_values::kLanguageFr: return "EPOCANVA_BACKEND_FR";
    case abi_values::kLanguageJa: return "EPOCANVA_BACKEND_JA";
    case abi_values::kLanguageKo: return "EPOCANVA_BACKEND_KO";
    default: return nullptr;
  }
}

const char* InputModeEnumName(std::uint32_t value) {
  switch (value) {
    case abi_values::kInputLatin: return "EPOCANVA_BACKEND_LATIN";
    case abi_values::kInputPinyin: return "EPOCANVA_BACKEND_PINYIN";
    case abi_values::kInputZhuyin: return "EPOCANVA_BACKEND_ZHUYIN";
    case abi_values::kInputRomaji: return "EPOCANVA_BACKEND_ROMAJI";
    case abi_values::kInputKatakana: return "EPOCANVA_BACKEND_KATAKANA";
    case abi_values::kInputTwoSet: return "EPOCANVA_BACKEND_TWO_SET";
    case abi_values::kInputKana: return "EPOCANVA_BACKEND_KANA";
    default: return nullptr;
  }
}

const char* OutputModeEnumName(std::uint32_t value) {
  switch (value) {
    case abi_values::kOutputLatin: return "EPOCANVA_BACKEND_OUTPUT_LATIN";
    case abi_values::kOutputHanzi: return "EPOCANVA_BACKEND_OUTPUT_HANZI";
    case abi_values::kOutputHiragana: return "EPOCANVA_BACKEND_OUTPUT_HIRAGANA";
    case abi_values::kOutputKatakana: return "EPOCANVA_BACKEND_OUTPUT_KATAKANA";
    case abi_values::kOutputKanji: return "EPOCANVA_BACKEND_OUTPUT_KANJI";
    case abi_values::kOutputHangul: return "EPOCANVA_BACKEND_OUTPUT_HANGUL";
    default: return nullptr;
  }
}

const char* KeyboardLayoutEnumName(std::uint32_t value) {
  switch (value) {
    case abi_values::kKeyboardNeutral: return "EPOCANVA_BACKEND_KEYBOARD_NEUTRAL";
    case abi_values::kKeyboardUsQwerty: return "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY";
    case abi_values::kKeyboardFrenchAzerty: return "EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY";
    case abi_values::kKeyboardGermanQwertz: return "EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ";
    case abi_values::kKeyboardDvorak: return "EPOCANVA_BACKEND_KEYBOARD_DVORAK";
    default: return nullptr;
  }
}

const char* ImplementationName(SchemeImplementation implementation) {
  switch (implementation) {
    case SchemeImplementation::kImplemented: return "implemented";
    case SchemeImplementation::kUnverified: return "unverified";
    case SchemeImplementation::kAbsent: return "absent";
  }
  return "absent";
}

const LocaleProfile* FindProfile(const std::vector<LocaleProfile>& profiles,
                                 std::string_view profile_id) {
  for (const LocaleProfile& profile : profiles) {
    if (profile.profile_id == profile_id) return &profile;
  }
  return nullptr;
}

const InputScheme* FindScheme(const LocaleProfile& profile, std::string_view scheme_id) {
  for (const InputScheme& scheme : profile.schemes) {
    if (scheme.scheme_id == scheme_id) return &scheme;
  }
  return nullptr;
}

void AppendJsonString(std::ostringstream* out, std::string_view text) {
  *out << '"';
  for (const char byte : text) {
    const unsigned char value = static_cast<unsigned char>(byte);
    switch (byte) {
      case '"': *out << "\\\""; break;
      case '\\': *out << "\\\\"; break;
      case '\n': *out << "\\n"; break;
      case '\r': *out << "\\r"; break;
      case '\t': *out << "\\t"; break;
      default:
        if (value < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          *out << "\\u00" << kHex[value >> 4] << kHex[value & 0x0f];
        } else {
          *out << byte;
        }
    }
  }
  *out << '"';
}

}  // namespace

const char* BackendManifestErrorName(BackendManifestError error) {
  switch (error) {
    case BackendManifestError::kOk: return "ok";
    case BackendManifestError::kEmpty: return "empty";
    case BackendManifestError::kDuplicateNumericId: return "duplicate_numeric_id";
    case BackendManifestError::kUnknownLocaleProfile: return "unknown_locale_profile";
    case BackendManifestError::kSchemeNotInProfile: return "scheme_not_in_profile";
    case BackendManifestError::kBindingForAbsentScheme: return "binding_for_absent_scheme";
    case BackendManifestError::kDuplicateConfiguration: return "duplicate_configuration";
    case BackendManifestError::kInvalidNumericField: return "invalid_numeric_field";
    case BackendManifestError::kMissingEnumName: return "missing_enum_name";
    case BackendManifestError::kEnumNameValueMismatch: return "enum_name_value_mismatch";
    case BackendManifestError::kLocaleTagMismatch: return "locale_tag_mismatch";
    case BackendManifestError::kSchemeImplementationMismatch:
      return "scheme_implementation_mismatch";
  }
  return "invalid";
}

BackendManifestError ValidateBackendManifestRows(
    const std::vector<BackendManifestRow>& rows,
    const std::vector<LocaleProfile>& profiles) {
  if (rows.empty()) return BackendManifestError::kEmpty;

  std::set<std::uint32_t> ids;
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                      std::string>> configurations;
  for (const BackendManifestRow& row : rows) {
    if (row.numeric_id == 0 || row.language == 0 || row.input_mode == 0 ||
        row.output_mode == 0) {
      return BackendManifestError::kInvalidNumericField;
    }
    const char* language = LanguageEnumName(row.language);
    const char* input = InputModeEnumName(row.input_mode);
    const char* output = OutputModeEnumName(row.output_mode);
    const char* keyboard = KeyboardLayoutEnumName(row.keyboard_layout);
    if (language == nullptr || input == nullptr || output == nullptr || keyboard == nullptr) {
      return BackendManifestError::kInvalidNumericField;
    }
    if (row.language_enum.empty() || row.input_mode_enum.empty() ||
        row.output_mode_enum.empty() || row.keyboard_layout_enum.empty()) {
      return BackendManifestError::kMissingEnumName;
    }
    if (row.language_enum != language || row.input_mode_enum != input ||
        row.output_mode_enum != output || row.keyboard_layout_enum != keyboard) {
      return BackendManifestError::kEnumNameValueMismatch;
    }
    if (!ids.insert(row.numeric_id).second) {
      return BackendManifestError::kDuplicateNumericId;
    }
    const LocaleProfile* profile = FindProfile(profiles, row.locale_profile_id);
    if (profile == nullptr) return BackendManifestError::kUnknownLocaleProfile;
    if (row.language_tag != profile->tag.language) {
      return BackendManifestError::kLocaleTagMismatch;
    }
    const InputScheme* scheme = FindScheme(*profile, row.scheme_id);
    if (scheme == nullptr) return BackendManifestError::kSchemeNotInProfile;
    if (scheme->implementation == SchemeImplementation::kAbsent) {
      return BackendManifestError::kBindingForAbsentScheme;
    }
    if (row.scheme_implementation != ImplementationName(scheme->implementation)) {
      return BackendManifestError::kSchemeImplementationMismatch;
    }
    const auto configuration = std::make_tuple(row.language, row.input_mode,
                                               row.output_mode, row.keyboard_layout,
                                               row.variant_id);
    if (!configurations.insert(configuration).second) {
      return BackendManifestError::kDuplicateConfiguration;
    }
  }
  return BackendManifestError::kOk;
}

std::vector<BackendManifestRow> BackendManifestRows() {
  std::vector<BackendManifestRow> rows;
  for (const LocaleProfile& profile : BuiltinProfiles()) {
    for (const BackendBinding& binding : profile.backend_bindings) {
      const InputScheme* scheme = FindScheme(profile, binding.scheme_id);
      BackendManifestRow row;
      row.numeric_id = binding.numeric_id;
      row.locale_profile_id = profile.profile_id;
      row.language_tag = profile.tag.language;
      row.language = binding.language;
      row.language_enum = LanguageEnumName(binding.language) == nullptr
                              ? std::string()
                              : LanguageEnumName(binding.language);
      row.input_mode = binding.input_mode;
      row.input_mode_enum = InputModeEnumName(binding.input_mode) == nullptr
                                ? std::string()
                                : InputModeEnumName(binding.input_mode);
      row.output_mode = binding.output_mode;
      row.output_mode_enum = OutputModeEnumName(binding.output_mode) == nullptr
                                 ? std::string()
                                 : OutputModeEnumName(binding.output_mode);
      row.keyboard_layout = binding.keyboard_layout;
      row.keyboard_layout_enum =
          KeyboardLayoutEnumName(binding.keyboard_layout) == nullptr
              ? std::string()
              : KeyboardLayoutEnumName(binding.keyboard_layout);
      row.variant_id = binding.variant_id;
      row.scheme_id = binding.scheme_id;
      row.scheme_implementation =
          scheme == nullptr ? std::string() : ImplementationName(scheme->implementation);
      rows.push_back(std::move(row));
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const BackendManifestRow& left, const BackendManifestRow& right) {
              return left.numeric_id < right.numeric_id;
            });
  return rows;
}

std::string BackendManifestJson() {
  const std::vector<LocaleProfile> profiles = BuiltinProfiles();
  const std::vector<BackendManifestRow> rows = BackendManifestRows();
  if (ValidateBackendManifestRows(rows, profiles) != BackendManifestError::kOk) return {};

  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": " << kBackendManifestFormat << ",\n";
  out << "  \"owner\": ";
  AppendJsonString(&out, kBackendManifestOwner);
  out << ",\n";
  out << "  \"bindings\": [\n";
  for (std::size_t ordinal = 0; ordinal < rows.size(); ++ordinal) {
    const BackendManifestRow& row = rows[ordinal];
    out << "    {\n";
    out << "      \"numeric_id\": " << row.numeric_id << ",\n";
    out << "      \"locale_profile\": ";
    AppendJsonString(&out, row.locale_profile_id);
    out << ",\n";
    out << "      \"language_tag\": ";
    AppendJsonString(&out, row.language_tag);
    out << ",\n";
    out << "      \"language\": ";
    AppendJsonString(&out, row.language_enum);
    out << ",\n";
    out << "      \"language_value\": " << row.language << ",\n";
    out << "      \"input_mode\": ";
    AppendJsonString(&out, row.input_mode_enum);
    out << ",\n";
    out << "      \"input_mode_value\": " << row.input_mode << ",\n";
    out << "      \"output_mode\": ";
    AppendJsonString(&out, row.output_mode_enum);
    out << ",\n";
    out << "      \"output_mode_value\": " << row.output_mode << ",\n";
    out << "      \"keyboard_layout\": ";
    AppendJsonString(&out, row.keyboard_layout_enum);
    out << ",\n";
    out << "      \"keyboard_layout_value\": " << row.keyboard_layout << ",\n";
    out << "      \"variant_id\": ";
    AppendJsonString(&out, row.variant_id);
    out << ",\n";
    out << "      \"scheme_id\": ";
    AppendJsonString(&out, row.scheme_id);
    out << ",\n";
    out << "      \"scheme_implementation\": ";
    AppendJsonString(&out, row.scheme_implementation);
    out << "\n    }" << (ordinal + 1 == rows.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace locale
}  // namespace epocanva
