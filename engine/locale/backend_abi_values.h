// Numeric values copied from engine/include/epocanva_backend.h.
//
// The locale layer deliberately does not include the backend ABI header: it is a
// platform-independent identity layer and the backend ABI depends on the engine
// ABI. That direction must not be reversed just to share constants. Instead, this
// is the single place inside the locale layer where the stable numeric values
// appear, and tests/locale_registry checks every one of them against the public
// header. A drift on either side makes that test fail; no other locale source
// may repeat the numbers.
#ifndef EPOCANVA_LOCALE_BACKEND_ABI_VALUES_H_
#define EPOCANVA_LOCALE_BACKEND_ABI_VALUES_H_

#include <cstdint>

namespace epocanva {
namespace locale {
namespace abi_values {

// EPOCANVA_BACKEND_{EN,ZH,FR,JA,KO} (epocanva_backend.h).
constexpr std::uint32_t kLanguageEn = 1;
constexpr std::uint32_t kLanguageZh = 2;
constexpr std::uint32_t kLanguageFr = 3;
constexpr std::uint32_t kLanguageJa = 4;
constexpr std::uint32_t kLanguageKo = 5;

// EPOCANVA_BACKEND_{LATIN,PINYIN,ZHUYIN,ROMAJI,KATAKANA,TWO_SET}.
constexpr std::uint32_t kInputLatin = 1;
constexpr std::uint32_t kInputPinyin = 2;
constexpr std::uint32_t kInputZhuyin = 3;
constexpr std::uint32_t kInputRomaji = 4;
constexpr std::uint32_t kInputKatakana = 5;
constexpr std::uint32_t kInputTwoSet = 6;
// EPOCANVA_BACKEND_KANA is declared after the output enum in the public header
// but belongs to the input-mode set.
constexpr std::uint32_t kInputKana = 7;

// EPOCANVA_BACKEND_OUTPUT_{LATIN,HANZI,HIRAGANA,KATAKANA,KANJI,HANGUL}.
constexpr std::uint32_t kOutputLatin = 1;
constexpr std::uint32_t kOutputHanzi = 2;
constexpr std::uint32_t kOutputHiragana = 3;
constexpr std::uint32_t kOutputKatakana = 4;
constexpr std::uint32_t kOutputKanji = 5;
constexpr std::uint32_t kOutputHangul = 6;

// EPOCANVA_BACKEND_KEYBOARD_{NEUTRAL,US_QWERTY,FRENCH_AZERTY,GERMAN_QWERTZ,DVORAK}.
constexpr std::uint32_t kKeyboardNeutral = 0;
constexpr std::uint32_t kKeyboardUsQwerty = 1;
constexpr std::uint32_t kKeyboardFrenchAzerty = 2;
constexpr std::uint32_t kKeyboardGermanQwertz = 3;
constexpr std::uint32_t kKeyboardDvorak = 4;

}  // namespace abi_values
}  // namespace locale
}  // namespace epocanva
#endif  // EPOCANVA_LOCALE_BACKEND_ABI_VALUES_H_
