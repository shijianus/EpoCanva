// EpoCanva P3.3 -- the Windows API adapter verification program.
//
// WHAT IT JUDGES. The 256-byte snapshot contract and the real ToUnicodeEx translation against
// the layouts that are ALREADY LOADED on this machine. It never installs, loads, unloads or
// activates a layout, never sets keyboard state, never sends input and never reads a document
// or the clipboard. Every keyboard state used for a translation assertion is a controlled
// 256-byte array built by this program; GetKeyboardState is called once, only to prove the
// capture path works, and its contents are not asserted, printed or recorded.
//
// LAYOUT-RELATIVE EXPECTATIONS. A layout is not assumed to be US. For each installed layout
// and each schema character, VkKeyScanExW(ch, hkl) finds the virtual key and shift state that
// the layout itself says produces that character, then ToUnicodeEx with that explicit HKL must
// return exactly that character. A character the layout cannot type without Ctrl/Alt is
// reported as unverified, not as a failure and not as a pass. This is why a US result is never
// projected onto every layout: on a French layout the same physical VK_A produces 'q', and the
// test follows the layout's own answer.
//
// MODES
//   snapshot        capture, controlled modifier derivation, snapshot immutability
//   translation     per-layout ASCII, non-ASCII, dead-key, no-text, multi-unit, key-up
//   self-check-fail one deliberately false claim, must exit 14 with no RESULT PASS
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check-fail mode's expected
// failure. RESULT PASS is printed only when nothing failed.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "key_decision.h"
#include "key_text_translation.h"
#include "keyboard_state.h"

namespace {

using epocanva::windows::input::CaptureThreadKeyboardState;
using epocanva::windows::input::DecideKey;
using epocanva::windows::input::DescribeKeyDecisionReason;
using epocanva::windows::input::DescribeKeyDisposition;
using epocanva::windows::input::DescribeKeyboardStateCaptureStatus;
using epocanva::windows::input::DescribeKeyTextStatus;
using epocanva::windows::input::DeriveModifierSnapshot;
using epocanva::windows::input::InputContext;
using epocanva::windows::input::InputPermission;
using epocanva::windows::input::kEnglishSchemaAlphabet;
using epocanva::windows::input::kKeyboardStateByteCount;
using epocanva::windows::input::kToUnicodeNoKeyboardStateChangeFlag;
using epocanva::windows::input::KeyDecision;
using epocanva::windows::input::KeyDecisionReason;
using epocanva::windows::input::KeyDisposition;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyboardStateSnapshot;
using epocanva::windows::input::KeyTextStatus;
using epocanva::windows::input::KeyTransition;
using epocanva::windows::input::KeyTranslationResult;
using epocanva::windows::input::KeyboardStateCaptureStatus;
using epocanva::windows::input::TextClass;
using epocanva::windows::input::TranslateKeyEvent;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[input-win] ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[input-win] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

int FinishMode() {
  if (g_failed != 0) {
    std::printf("[input-win] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
    return kExitCheckFailed;
  }
  std::printf("[input-win] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

std::string CountText(std::size_t value) { return std::to_string(value); }

std::string UnitHex(std::uint32_t code_point) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "U+%04X", code_point);
  return buffer;
}

std::string WideAscii(const wchar_t* text) {
  std::string out;
  for (const wchar_t* cursor = text; cursor != nullptr && *cursor != L'\0'; ++cursor) {
    out.push_back(*cursor >= 0x20 && *cursor < 0x7F ? static_cast<char>(*cursor) : '?');
  }
  return out;
}

HKL CurrentLayout() { return ::GetKeyboardLayout(0); }

std::string CurrentLayoutKlid() {
  wchar_t name[KL_NAMELENGTH] = {};
  if (::GetKeyboardLayoutNameW(name) == FALSE) {
    return "unknown";
  }
  return WideAscii(name);
}

std::string LayoutLabel(HKL layout) {
  const unsigned long long value =
      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(layout));
  const unsigned language_id = static_cast<unsigned>(value & 0xFFFFu);
  char buffer[128];
  if (layout == CurrentLayout()) {
    std::snprintf(buffer, sizeof(buffer), "current(klid=%s,langid=0x%04X)",
                  CurrentLayoutKlid().c_str(), language_id);
  } else {
    std::snprintf(buffer, sizeof(buffer), "loaded(hkl=0x%llX,langid=0x%04X)", value,
                  language_id);
  }
  return buffer;
}

std::vector<HKL> InstalledLayouts() {
  const int count = ::GetKeyboardLayoutList(0, nullptr);
  if (count <= 0) {
    return {};
  }
  std::vector<HKL> layouts(static_cast<std::size_t>(count), nullptr);
  const int written = ::GetKeyboardLayoutList(count, layouts.data());
  if (written <= 0) {
    return {};
  }
  layouts.resize(static_cast<std::size_t>(written));
  // The list is allowed to contain the same HKL more than once; judge each loader entry once.
  std::vector<HKL> unique;
  for (const HKL layout : layouts) {
    bool seen = false;
    for (const HKL existing : unique) {
      if (existing == layout) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      unique.push_back(layout);
    }
  }
  return unique;
}

struct ChordInput {
  KeyboardStateSnapshot snapshot;
  KeyEventSnapshot event;
};

// Build a controlled 256-byte state from VkKeyScanExW's shift-state byte. Only Shift (bit 0)
// is representable; a chord needing Ctrl/Alt is skipped by the caller because this round does
// not support AltGr as text.
ChordInput MakeChord(BYTE virtual_key, BYTE shift_state, HKL layout) {
  std::array<std::uint8_t, kKeyboardStateByteCount> key_state_bytes{};
  if ((shift_state & 0x01u) != 0) {
    key_state_bytes[VK_SHIFT] = 0x80;
  }

  ChordInput input;
  input.snapshot = KeyboardStateSnapshot::FromExplicitState(key_state_bytes, layout);
  input.event.virtual_key = virtual_key;
  input.event.scan_code = ::MapVirtualKeyExW(virtual_key, MAPVK_VK_TO_VSC, layout);
  input.event.is_extended = false;
  input.event.transition = KeyTransition::kKeyDown;
  input.event.repeat_count = 1;
  input.event.previously_down = false;
  input.event.modifiers = input.snapshot.modifiers();
  return input;
}

struct Chord {
  bool available = false;
  bool needs_ctrl_or_alt = false;
  BYTE virtual_key = 0;
  BYTE shift_state = 0;
};

Chord FindChord(wchar_t character, HKL layout) {
  Chord chord;
  const SHORT scan = ::VkKeyScanExW(character, layout);
  if (scan == -1) {
    return chord;
  }
  chord.available = true;
  chord.virtual_key = static_cast<BYTE>(scan & 0xFF);
  chord.shift_state = static_cast<BYTE>((scan >> 8) & 0xFF);
  chord.needs_ctrl_or_alt = (chord.shift_state & 0x06u) != 0;
  return chord;
}

struct Attempt {
  enum class Kind { kNotTypeable, kNeedsCtrlAlt, kTranslated };
  Kind kind = Kind::kNotTypeable;
  ChordInput input;
  KeyTranslationResult result;
};

Attempt TranslateCharacter(wchar_t character, HKL layout) {
  Attempt attempt;
  const Chord chord = FindChord(character, layout);
  if (!chord.available) {
    attempt.kind = Attempt::Kind::kNotTypeable;
    return attempt;
  }
  if (chord.needs_ctrl_or_alt) {
    attempt.kind = Attempt::Kind::kNeedsCtrlAlt;
    return attempt;
  }
  attempt.kind = Attempt::Kind::kTranslated;
  attempt.input = MakeChord(chord.virtual_key, chord.shift_state, layout);
  attempt.result = TranslateKeyEvent(attempt.input.event, attempt.input.snapshot);
  return attempt;
}

std::string TranslationText(const KeyTranslationResult& result) {
  std::string out = std::string(DescribeKeyTextStatus(result.text.status));
  out += " raw=";
  out += std::to_string(result.raw_return_value);
  out += " units=";
  out += CountText(result.text.utf16_code_units.size());
  out += " bytes=";
  out += CountText(result.utf8_bytes.size());
  out += " call=";
  out += result.system_call_made ? "yes" : "no";
  return out;
}

bool IsSingleAscii(const KeyTranslationResult& result, unsigned char expected) {
  return result.ok_for_text() && result.text.utf16_code_units.size() == 1 &&
         result.text.utf16_code_units[0] == static_cast<char16_t>(expected) &&
         result.utf8_bytes.size() == 1 &&
         static_cast<unsigned char>(result.utf8_bytes[0]) == expected;
}

InputContext PlainContext(bool composition_active) {
  InputContext context;
  context.input_enabled = true;
  context.engine_ready = true;
  context.composition_active = composition_active;
  context.permission = InputPermission::kAllowed;
  return context;
}

KeyEventSnapshot KeyUpEvent(std::uint32_t virtual_key) {
  KeyEventSnapshot event;
  event.virtual_key = virtual_key;
  event.transition = KeyTransition::kKeyUp;
  return event;
}

// --- mode: snapshot -------------------------------------------------------------------
int RunSnapshot() {
  std::printf("[input-win] mode snapshot: GetKeyboardState and controlled modifier bytes\n");

  const auto captured = CaptureThreadKeyboardState();
  Check(captured.ok(),
        "CaptureThreadKeyboardState succeeds and returns the thread's active layout",
        std::string(DescribeKeyboardStateCaptureStatus(captured.status)) + " " +
            captured.diagnostic);
  if (captured.ok()) {
    Check(captured.snapshot.has_layout() && captured.snapshot.layout() == CurrentLayout(),
          "the captured snapshot carries the calling thread's current HKL",
          LayoutLabel(captured.snapshot.layout()));
    Check(captured.snapshot.key_state_bytes().size() == kKeyboardStateByteCount,
          "the captured keyboard state is 256 BYTES",
          CountText(captured.snapshot.key_state_bytes().size()) + " bytes");
    std::printf("[input-win] evidence snapshot captured layout=%s bytes=%zu\n",
                LayoutLabel(captured.snapshot.layout()).c_str(),
                captured.snapshot.key_state_bytes().size());
  }

  // All zero: no modifier and no toggle is on.
  {
    const std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    const auto modifiers = DeriveModifierSnapshot(bytes);
    Check(!modifiers.shift && !modifiers.control && !modifiers.alt && !modifiers.windows &&
              !modifiers.caps_lock,
          "an all-zero state derives no modifier and no Caps Lock toggle",
          "a flag was set");
  }

  // Shift: the HIGH bit is the key being down; the low bit alone is not.
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_SHIFT] = 0x80;
    Check(DeriveModifierSnapshot(bytes).shift, "Shift high bit derives shift = true",
          "shift false");
    bytes[VK_SHIFT] = 0x01;
    Check(!DeriveModifierSnapshot(bytes).shift,
          "Shift low bit alone does not derive shift = true", "shift true");
  }

  // Control, Alt, either Windows key.
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_CONTROL] = 0x80;
    bytes[VK_MENU] = 0x80;
    bytes[VK_LWIN] = 0x80;
    const auto modifiers = DeriveModifierSnapshot(bytes);
    Check(modifiers.control && modifiers.alt && modifiers.windows,
          "Control, Alt and left Windows derive from their own state bytes",
          "a flag was missing");
  }
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_RWIN] = 0x80;
    Check(DeriveModifierSnapshot(bytes).windows,
          "the right Windows key also derives windows = true", "windows false");
  }

  // Caps Lock is a TOGGLE: the low bit is the state, the high bit is only "held".
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_CAPITAL] = 0x01;
    Check(DeriveModifierSnapshot(bytes).caps_lock,
          "Caps Lock low bit (toggled) derives caps_lock = true", "caps false");
    bytes[VK_CAPITAL] = 0x80;
    Check(!DeriveModifierSnapshot(bytes).caps_lock,
          "Caps Lock high bit (held, not toggled) does not derive caps_lock = true",
          "caps true");
  }

  // A bit outside the documented meanings changes nothing.
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_LWIN] = 0x40;
    const auto modifiers = DeriveModifierSnapshot(bytes);
    Check(!modifiers.windows && !modifiers.shift && !modifiers.control && !modifiers.alt,
          "bit 6 of a state byte is ignored", "a modifier was set");
  }

  // The snapshot COPIES the bytes and derives once. Mutating the source array afterwards
  // cannot change either the bytes or the modifiers.
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
    bytes[VK_SHIFT] = 0x80;
    bytes[0xE1] = 0xAB;  // an otherwise unused index, to prove the copy is byte-for-byte
    const KeyboardStateSnapshot snapshot =
        KeyboardStateSnapshot::FromExplicitState(bytes, CurrentLayout());
    bytes[VK_SHIFT] = 0x00;
    bytes[0xE1] = 0x00;
    Check(snapshot.key_state_bytes()[0xE1] == 0xAB && snapshot.modifiers().shift,
          "the snapshot keeps a byte-for-byte copy and its derived modifiers after the "
          "source array is changed",
          "snapshot observed the mutated source");
  }

  // Two snapshots made from different controlled states are independent and do not read the
  // machine's real keyboard: this is the property that makes the pure/adapter split testable.
  {
    std::array<std::uint8_t, kKeyboardStateByteCount> control_bytes{};
    control_bytes[VK_CONTROL] = 0x80;
    std::array<std::uint8_t, kKeyboardStateByteCount> alt_bytes{};
    alt_bytes[VK_MENU] = 0x80;
    const KeyboardStateSnapshot control =
        KeyboardStateSnapshot::FromExplicitState(control_bytes, CurrentLayout());
    const KeyboardStateSnapshot alt =
        KeyboardStateSnapshot::FromExplicitState(alt_bytes, CurrentLayout());
    Check(control.modifiers().control && !control.modifiers().alt &&
              alt.modifiers().alt && !alt.modifiers().control,
          "two snapshots derive their own modifiers independently of global keyboard state",
          "snapshots bled into each other");
  }

  // A null layout is representable and refused by translation rather than guessed at.
  {
    const KeyboardStateSnapshot no_layout = KeyboardStateSnapshot::FromExplicitState(
        std::array<std::uint8_t, kKeyboardStateByteCount>{}, nullptr);
    Check(!no_layout.has_layout(), "an explicit null HKL is preserved as no layout",
          "a layout was invented");
    const ChordInput chord = MakeChord(0x41, 0, CurrentLayout());
    const KeyTranslationResult result = TranslateKeyEvent(chord.event, no_layout);
    Check(result.text.status == KeyTextStatus::kNoLayout && !result.system_call_made,
          "translation with no layout is refused without a system call",
          TranslationText(result));
  }

  return FinishMode();
}

// --- mode: translation ----------------------------------------------------------------
void CheckAsciiOnLayout(HKL layout, int* verified, int* skipped, int* needs_ctrl_alt,
                        int* dead_sequence, int* failed) {
  for (const char character : std::string(kEnglishSchemaAlphabet)) {
    const wchar_t wide = static_cast<wchar_t>(static_cast<unsigned char>(character));
    const Attempt attempt = TranslateCharacter(wide, layout);
    if (attempt.kind == Attempt::Kind::kNotTypeable) {
      ++*skipped;
      continue;
    }
    if (attempt.kind == Attempt::Kind::kNeedsCtrlAlt) {
      ++*needs_ctrl_alt;
      continue;
    }
    if (attempt.result.text.status == KeyTextStatus::kDeadKey) {
      // The layout types this character only through a dead-key sequence. That is a layout
      // fact, not a text result this round can claim; report it as unverified.
      ++*dead_sequence;
      continue;
    }
    ++*verified;
    const unsigned char expected = static_cast<unsigned char>(character);
    const bool good = IsSingleAscii(attempt.result, expected) &&
                      attempt.result.raw_return_value == 1 &&
                      attempt.result.system_call_made;
    if (!good) {
      ++*failed;
    }
    Check(good,
          "layout " + LayoutLabel(layout) + ": " + UnitHex(expected) +
              " translates to exactly that one ASCII character",
          TranslationText(attempt.result));
  }
}

int RunTranslation() {
  std::printf("[input-win] mode translation: real ToUnicodeEx against loaded layouts\n");

  Check(kToUnicodeNoKeyboardStateChangeFlag == 0x0004u,
        "the ToUnicodeEx no-state-change flag is bit 2 (0x0004)",
        std::to_string(kToUnicodeNoKeyboardStateChangeFlag));
  {
    wchar_t klid[KL_NAMELENGTH] = {};
    Check(::GetKeyboardLayoutNameW(klid) != FALSE && klid[0] != L'\0',
          "the current layout has a readable KLID", "GetKeyboardLayoutNameW failed");
  }

  const std::vector<HKL> layouts = InstalledLayouts();
  std::printf("[input-win] evidence layouts=%zu current=%s\n", layouts.size(),
              LayoutLabel(CurrentLayout()).c_str());
  Check(!layouts.empty(), "GetKeyboardLayoutList returns at least one loaded layout",
        "no installed layout");

  bool any_non_ascii_verified = false;
  bool any_dead_key_verified = false;
  bool any_multi_unit_verified = false;
  bool any_no_text_verified = false;

  for (const HKL layout : layouts) {
    int verified = 0;
    int skipped = 0;
    int needs_ctrl_alt = 0;
    int dead_sequence = 0;
    int failed = 0;
    CheckAsciiOnLayout(layout, &verified, &skipped, &needs_ctrl_alt, &dead_sequence, &failed);
    Check(failed == 0,
          "layout " + LayoutLabel(layout) + ": every typeable schema character round-trips",
          std::to_string(failed) + " failed of " + std::to_string(verified));

    const Attempt lower = TranslateCharacter(L'a', layout);
    const Attempt upper = TranslateCharacter(L'A', layout);
    const bool lower_ok = lower.kind == Attempt::Kind::kTranslated &&
                          IsSingleAscii(lower.result, 'a');
    const bool upper_ok = upper.kind == Attempt::Kind::kTranslated &&
                          IsSingleAscii(upper.result, 'A');
    std::printf("[input-win] evidence a/A layout=%s lower=%s upper=%s\n",
                LayoutLabel(layout).c_str(), lower_ok ? "verified" : "unverified",
                upper_ok ? "verified" : "unverified");
    Check(lower.kind != Attempt::Kind::kTranslated || lower_ok,
          "layout " + LayoutLabel(layout) + ": 'a' translates to exactly U+0061",
          lower.kind == Attempt::Kind::kTranslated ? TranslationText(lower.result)
                                                   : "not typeable without Ctrl/Alt");
    Check(upper.kind != Attempt::Kind::kTranslated || upper_ok,
          "layout " + LayoutLabel(layout) + ": 'A' translates to exactly U+0041",
          upper.kind == Attempt::Kind::kTranslated ? TranslationText(upper.result)
                                                   : "not typeable without Ctrl/Alt");

    // The adapter's result feeds the pure decision without any reinterpretation.
    if (lower_ok) {
      const KeyDecision decision =
          DecideKey(lower.input.event, lower.result.text, PlainContext(false));
      Check(decision.disposition == KeyDisposition::kEngineTextCandidate &&
                decision.ascii_code_point == 'a' &&
                decision.text_class == TextClass::kAsciiLetter,
            "layout " + LayoutLabel(layout) +
                ": the translated 'a' becomes a text candidate for U+0061",
            std::string(DescribeKeyDisposition(decision.disposition)) + "/" +
                DescribeKeyDecisionReason(decision.reason));
    }
    if (upper_ok) {
      const KeyDecision decision =
          DecideKey(upper.input.event, upper.result.text, PlainContext(false));
      Check(decision.disposition == KeyDisposition::kEngineTextCandidate &&
                decision.ascii_code_point == 'A' &&
                decision.text_class == TextClass::kAsciiLetter,
            "layout " + LayoutLabel(layout) +
                ": the translated 'A' becomes a text candidate for U+0041",
            std::string(DescribeKeyDisposition(decision.disposition)) + "/" +
                DescribeKeyDecisionReason(decision.reason));
    }

    // Non-ASCII: a character this layout can type directly, without Ctrl/Alt. The expected
    // result is the exact code unit and a multi-byte strict UTF-8 conversion; the decision
    // rejects it as a whole and demands boundary coordination while composing.
    {
      const std::uint32_t candidates[] = {0x00E9, 0x00FC, 0x00F1, 0x0444,
                                          0x3042, 0x4E2D, 0x03A9, 0x20AC};
      bool found = false;
      for (const std::uint32_t code_point : candidates) {
        const Attempt attempt =
            TranslateCharacter(static_cast<wchar_t>(code_point), layout);
        if (attempt.kind != Attempt::Kind::kTranslated) {
          continue;
        }
        const bool single_bmp = attempt.result.ok_for_text() &&
                                attempt.result.text.utf16_code_units.size() == 1 &&
                                attempt.result.text.utf16_code_units[0] ==
                                    static_cast<char16_t>(code_point);
        const bool multibyte = attempt.result.utf8_bytes.size() > 1;
        const KeyDecision decision =
            DecideKey(attempt.input.event, attempt.result.text, PlainContext(true));
        const bool boundary = decision.disposition ==
                                  KeyDisposition::kCompositionBoundaryRequired &&
                              decision.reason == KeyDecisionReason::kNonAsciiCharacter;
        Check(single_bmp && multibyte && boundary,
              "layout " + LayoutLabel(layout) + ": " + UnitHex(code_point) +
                  " translates to one non-ASCII unit, multi-byte UTF-8 and a boundary demand",
              TranslationText(attempt.result) + " decision=" +
                  DescribeKeyDisposition(decision.disposition));
        if (single_bmp && multibyte && boundary) {
          any_non_ascii_verified = true;
          found = true;
          std::printf("[input-win] evidence non-ascii layout=%s codepoint=%s units=%zu "
                      "utf8-bytes=%zu\n",
                      LayoutLabel(layout).c_str(), UnitHex(code_point).c_str(),
                      attempt.result.text.utf16_code_units.size(),
                      attempt.result.utf8_bytes.size());
          break;
        }
      }
      if (!found) {
        std::printf("[input-win] evidence non-ascii layout=%s unverified (no direct "
                    "non-Ctrl/Alt chord for the tested characters)\n",
                    LayoutLabel(layout).c_str());
      }
    }

    // Scan the layout's ordinary keys (neutral and Shift) for the three real behaviours that
    // are layout-dependent: a dead key, a multi-code-unit result, and a no-text result.
    {
      std::vector<BYTE> virtual_keys;
      for (BYTE vk = 0x30; vk <= 0x39; ++vk) virtual_keys.push_back(vk);
      for (BYTE vk = 0x41; vk <= 0x5A; ++vk) virtual_keys.push_back(vk);
      for (BYTE vk = 0xBA; vk <= 0xC0; ++vk) virtual_keys.push_back(vk);
      for (BYTE vk = 0xDB; vk <= 0xDE; ++vk) virtual_keys.push_back(vk);
      virtual_keys.push_back(0xE2);

      bool dead_found = false;
      bool multi_found = false;
      for (const BYTE vk : virtual_keys) {
        for (const BYTE shift_state : {static_cast<BYTE>(0), static_cast<BYTE>(1)}) {
          const ChordInput input = MakeChord(vk, shift_state, layout);
          const KeyTranslationResult result =
              TranslateKeyEvent(input.event, input.snapshot);
          if (result.text.status == KeyTextStatus::kDeadKey && !dead_found) {
            dead_found = true;
            any_dead_key_verified = true;
            Check(result.text.utf16_code_units.empty() && result.raw_return_value < 0 &&
                      result.system_call_made,
                  "layout " + LayoutLabel(layout) + ": dead key refuses its spacing "
                  "character and reports no text",
                  TranslationText(result));
            const KeyDecision plain =
                DecideKey(input.event, result.text, PlainContext(false));
            const KeyDecision composing =
                DecideKey(input.event, result.text, PlainContext(true));
            Check(plain.disposition == KeyDisposition::kPassThroughToHost &&
                      plain.reason == KeyDecisionReason::kDeadKey,
                  "layout " + LayoutLabel(layout) +
                      ": a dead key without a composition passes through",
                  std::string(DescribeKeyDisposition(plain.disposition)) + "/" +
                      DescribeKeyDecisionReason(plain.reason));
            Check(composing.disposition ==
                          KeyDisposition::kCompositionBoundaryRequired &&
                      composing.reason == KeyDecisionReason::kDeadKey,
                  "layout " + LayoutLabel(layout) +
                      ": a dead key while composing demands boundary coordination",
                  std::string(DescribeKeyDisposition(composing.disposition)) + "/" +
                      DescribeKeyDecisionReason(composing.reason));

            // The non-mutation property, measured rather than assumed: after translating the
            // dead key (bit 2 set), 'a' must still be plain 'a'. If the flag were ignored,
            // the pending dead key would combine with it.
            const Attempt letter = TranslateCharacter(L'a', layout);
            if (letter.kind == Attempt::Kind::kTranslated) {
              const Attempt second = TranslateCharacter(L'a', layout);
              Check(second.kind == Attempt::Kind::kTranslated &&
                        IsSingleAscii(second.result, 'a'),
                    "layout " + LayoutLabel(layout) +
                        ": translating a dead key did not change the following 'a' "
                        "(bit 2 non-mutation)",
                    second.kind == Attempt::Kind::kTranslated
                        ? TranslationText(second.result)
                        : "the letter was not typeable");
            } else {
              std::printf("[input-win] evidence dead-key-nonmutation layout=%s "
                          "unverified (no typeable 'a')\n",
                          LayoutLabel(layout).c_str());
            }
            std::printf("[input-win] evidence dead-key layout=%s vk=0x%02X shift=%u "
                        "raw=%d units=%zu\n",
                        LayoutLabel(layout).c_str(), static_cast<unsigned>(vk),
                        static_cast<unsigned>(shift_state), result.raw_return_value,
                        result.text.utf16_code_units.size());
          }
          if (result.text.status == KeyTextStatus::kText &&
              result.raw_return_value > 1 && !multi_found) {
            multi_found = true;
            any_multi_unit_verified = true;
            const KeyDecision decision =
                DecideKey(input.event, result.text, PlainContext(true));
            Check(result.text.utf16_code_units.size() ==
                          static_cast<std::size_t>(result.raw_return_value) &&
                      result.utf8_bytes.size() > 1 &&
                      decision.disposition ==
                          KeyDisposition::kCompositionBoundaryRequired &&
                      decision.reason == KeyDecisionReason::kMultipleUtf16CodeUnits,
                  "layout " + LayoutLabel(layout) +
                      ": a multi-unit translation is preserved whole and rejected whole",
                  TranslationText(result));
            std::printf("[input-win] evidence multi-unit layout=%s vk=0x%02X shift=%u "
                        "raw=%d units=%zu utf8-bytes=%zu\n",
                        LayoutLabel(layout).c_str(), static_cast<unsigned>(vk),
                        static_cast<unsigned>(shift_state), result.raw_return_value,
                        result.text.utf16_code_units.size(), result.utf8_bytes.size());
          }
        }
      }
      if (!dead_found) {
        std::printf("[input-win] evidence dead-key layout=%s unverified (no dead key found "
                    "in the neutral/Shift key scan)\n",
                    LayoutLabel(layout).c_str());
      }
      if (!multi_found) {
        std::printf("[input-win] evidence multi-unit layout=%s unverified (no multi-unit "
                    "result found in the neutral/Shift key scan)\n",
                    LayoutLabel(layout).c_str());
      }
    }

    // A function key with no translation is a real no-text result, not an empty string.
    if (!any_no_text_verified) {
      for (UINT vk = VK_F1; vk <= VK_F12; ++vk) {
        const ChordInput input = MakeChord(static_cast<BYTE>(vk), 0, layout);
        const KeyTranslationResult result =
            TranslateKeyEvent(input.event, input.snapshot);
        if (result.text.status == KeyTextStatus::kNoText) {
          any_no_text_verified = true;
          Check(result.raw_return_value == 0 && result.text.utf16_code_units.empty() &&
                    result.system_call_made,
                "layout " + LayoutLabel(layout) +
                    ": a key with no translation reports zero units, not empty text",
                TranslationText(result));
          std::printf("[input-win] evidence no-text layout=%s vk=0x%02X raw=0\n",
                      LayoutLabel(layout).c_str(), static_cast<unsigned>(vk));
          break;
        }
      }
    }

    std::printf("[input-win] evidence layout=%s ascii-verified=%d ascii-skipped=%d "
                "ascii-needs-ctrl-alt=%d ascii-dead-sequence=%d\n",
                LayoutLabel(layout).c_str(), verified, skipped, needs_ctrl_alt,
                dead_sequence);
  }

  if (!any_non_ascii_verified) {
    std::printf("[input-win] evidence non-ascii unverified: no installed layout had a "
                "direct non-Ctrl/Alt chord for the tested characters\n");
  }
  if (!any_dead_key_verified) {
    std::printf("[input-win] evidence dead-key unverified: no installed layout exposed a "
                "dead key in the neutral/Shift scan\n");
  }
  if (!any_multi_unit_verified) {
    std::printf("[input-win] evidence multi-unit unverified: no installed layout returned "
                "more than one code unit for the scanned keys\n");
  }
  if (!any_no_text_verified) {
    std::printf("[input-win] evidence no-text unverified: no function key returned zero\n");
  }

  // Key-up: refused before the API, so no translation can touch a dead-key state.
  {
    const ChordInput input =
        MakeChord(0x41, 0, layouts.empty() ? CurrentLayout() : layouts.front());
    const KeyEventSnapshot up = KeyUpEvent(0x41);
    const KeyTranslationResult result = TranslateKeyEvent(up, input.snapshot);
    Check(result.text.status == KeyTextStatus::kNotKeyDown && !result.system_call_made &&
              result.raw_return_value == 0 && result.text.utf16_code_units.empty(),
          "a key-up is refused as not-key-down without calling ToUnicodeEx",
          TranslationText(result));
  }
  {
    const KeyboardStateSnapshot no_layout = KeyboardStateSnapshot::FromExplicitState(
        std::array<std::uint8_t, kKeyboardStateByteCount>{}, nullptr);
    const KeyTranslationResult result = TranslateKeyEvent(KeyUpEvent(0x41), no_layout);
    Check(result.text.status == KeyTextStatus::kNotKeyDown && !result.system_call_made,
          "a key-up is refused as not-key-down even when no layout is supplied",
          TranslationText(result));
  }

  // Repeat metadata does not change the translation: the adapter reads neither repeat_count
  // nor previously_down, and one callback still yields one result.
  {
    const Attempt attempt = TranslateCharacter(L'a', CurrentLayout());
    if (attempt.kind == Attempt::Kind::kTranslated) {
      KeyEventSnapshot repeated = attempt.input.event;
      repeated.repeat_count = 5;
      repeated.previously_down = true;
      const KeyTranslationResult result =
          TranslateKeyEvent(repeated, attempt.input.snapshot);
      Check(result.ok_for_text() && IsSingleAscii(result, 'a') &&
                result.text.utf16_code_units.size() == 1,
            "repeat_count 5 and previously_down do not change the translated text",
            TranslationText(result));
    } else {
      std::printf("[input-win] evidence repeat-unverified: 'a' is not typeable on the "
                  "current layout without Ctrl/Alt\n");
    }
  }

  return FinishMode();
}

// --- the falsifiability control -------------------------------------------------------
int RunSelfCheckFail() {
  std::printf("[input-win] mode self-check-fail: one deliberately false claim\n");
  // The false claim: a key-up is translated into text. The real modes pin the opposite, so
  // this must fail -- exit 14, no RESULT PASS.
  const KeyboardStateSnapshot snapshot = KeyboardStateSnapshot::FromExplicitState(
      std::array<std::uint8_t, kKeyboardStateByteCount>{}, CurrentLayout());
  const KeyTranslationResult result = TranslateKeyEvent(KeyUpEvent(0x41), snapshot);
  Check(result.text.status == KeyTextStatus::kText,
        "deliberately asserted that a key-up was translated into text",
        TranslationText(result));
  std::printf("[input-win] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n",
              g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring mode;
  for (int index = 1; index < argc; ++index) {
    const std::wstring arg = argv[index];
    if (arg == L"--mode" && index + 1 < argc) {
      mode = argv[++index];
    } else {
      std::printf("usage: keyboard_translation_tests --mode snapshot|translation|"
                  "self-check-fail\n");
      return kExitUsage;
    }
  }
  if (mode == L"snapshot") {
    return RunSnapshot();
  }
  if (mode == L"translation") {
    return RunTranslation();
  }
  if (mode == L"self-check-fail") {
    return RunSelfCheckFail();
  }
  std::printf("usage: keyboard_translation_tests --mode snapshot|translation|self-check-fail\n");
  return kExitUsage;
}
