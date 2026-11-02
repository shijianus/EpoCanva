#ifndef EPOCANVA_JA_ROMAJI_COMPOSER_H_
#define EPOCANVA_JA_ROMAJI_COMPOSER_H_

#include <cstddef>
#include <string>

namespace epocanva {
namespace ja {

// An input step is one accepted printable ASCII byte, not a grapheme or OS key.
inline constexpr std::size_t kMaxInputSteps = 4096;

enum class InputResult { kAccepted, kRejectedByte, kInputLimit };
enum class KanaMode { kHiragana, kKatakana };

struct Preedit {
  std::string text_utf8;
  // All text offsets and extents are UTF-8 code units (bytes), never UTF-16.
  std::size_t caret_utf8_bytes = 0;
  std::size_t active_begin_utf8_bytes = 0;
  std::size_t active_size_utf8_bytes = 0;
  // Number of accepted ASCII-byte input steps since commit/cancel.
  std::size_t input_step_count = 0;
};

// Independent in-memory Romaji -> Kana composition, not Kana -> Kanji conversion.
// No dictionaries, candidates, platform/Rime dependencies, I/O, learning or logs.
// One instance belongs to one caller-controlled composition; no shared state.
class RomajiComposer {
 public:
  explicit RomajiComposer(KanaMode mode = KanaMode::kHiragana);

  // Accepts only bytes 0x20..0x7e. Rejection leaves every state field unchanged.
  // Romanization is ASCII case-insensitive; unmatched literals retain their case.
  InputResult PushAscii(char ascii_byte);

  // Removes one accepted ASCII-byte input step and reconstructs its predecessor.
  // This can unmake a whole kana or yoon, rather than delete a Unicode character.
  // Returns false (no mutation) when there is no input step to remove.
  bool Backspace();

  // Clears this composition and its transient input history; retains kana mode.
  // No claim of secure erasure of allocator-owned memory is made.
  void Cancel();

  // Returns strict UTF-8, then clears like Cancel(). A bare pending n -> nasal;
  // nn has already emitted one nasal. Other incomplete prefixes stay literal.
  // This does not write to a host application; the caller owns the returned text.
  std::string CommitUtf8();

  // Resolved kana + visible pending ASCII; caret is always at the UTF-8 byte end.
  // The active span is the pending ASCII prefix (possibly empty). After nn its
  // optional reusable n is invisible and the active span is empty.
  Preedit GetPreedit() const;

  // Rerenders all outstanding kana, preserving literal case and input-step count.
  // A mode change is not an input step; Backspace does not undo mode changes.
  void SetMode(KanaMode mode);
  KanaMode GetMode() const;

 private:
  void ProcessAscii(char ascii_byte);
  void ResolvePending();
  void RebuildFromHistory();
  void AppendKana(char32_t unicode_scalar);

  KanaMode mode_;
  std::string history_ascii_;
  std::string resolved_utf8_;
  std::string pending_ascii_;
  // nn emits one nasal immediately; its second n can start an immediately
  // following vowel/y syllable. Apostrophe consumes this optional reuse.
  char reusable_n_ascii_ = '\0';
};

}  // namespace ja
}  // namespace epocanva

#endif  // EPOCANVA_JA_ROMAJI_COMPOSER_H_
