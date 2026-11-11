#ifndef EPOCANVA_KO_TWO_SET_COMPOSER_H_
#define EPOCANVA_KO_TWO_SET_COMPOSER_H_

#include <cstddef>
#include <string>

namespace epocanva {
namespace ko {

// An input step is one accepted printable ASCII byte, not a grapheme or OS key.
inline constexpr std::size_t kMaxInputSteps = 4096;

enum class InputResult { kAccepted, kRejectedByte, kInputLimit };

struct Preedit {
  std::string text_utf8;
  // All text offsets and extents are UTF-8 code units (bytes), never UTF-16.
  std::size_t caret_utf8_bytes = 0;
  std::size_t active_begin_utf8_bytes = 0;
  std::size_t active_size_utf8_bytes = 0;
  // Number of accepted ASCII-byte input steps since commit/cancel.
  std::size_t input_step_count = 0;
};

// Independent in-memory standard two-set-key -> modern Hangul composition.
// No dictionaries, candidates, platform/Rime dependencies, I/O, learning or logs.
// One instance belongs to one caller-controlled composition; no shared state.
class TwoSetComposer {
 public:
  // Accepts only bytes 0x20..0x7e. Rejection leaves every state field unchanged.
  // R/E/Q/T/W are tense consonants; O/P are shifted vowels. Other uppercase
  // letters equal their lowercase keys. Nonletters are literal boundaries.
  InputResult PushAscii(char ascii_byte);

  // Undoes one accepted ASCII-byte input step, including a final-cluster split
  // or a literal boundary. Does not delete arbitrary UTF-8 bytes or graphemes.
  bool Backspace();

  // Clears this composition and its transient input history. No secure-erasure
  // guarantee for allocator-owned memory is made.
  void Cancel();

  // Returns strict UTF-8, then clears like Cancel(). An incomplete initial or
  // standalone vowel is emitted as a compatibility Jamo; no implicit ㅇ.
  // This does not write to a host application; the caller owns the returned text.
  std::string CommitUtf8();

  // The active span is one syllable/compatibility Jamo, or empty after a literal.
  // Caret is always at the UTF-8 byte end; earlier text is still uncommitted.
  Preedit GetPreedit() const;

 private:
  void ProcessAscii(char ascii_byte);
  void ProcessConsonant(int initial_ordinal, int final_ordinal);
  void ProcessVowel(int medial_ordinal);
  void FlushActive();
  void AppendActive(std::string* output_utf8) const;
  void RebuildFromHistory();

  std::string history_ascii_;
  std::string resolved_utf8_;
  // These are Hangul alphabet ordinals, NOT text offsets: L in [0,18],
  // V in [0,20], T in [0,27]. -1 means absent L/V; T=0 means no final.
  int initial_ordinal_ = -1;
  int medial_ordinal_ = -1;
  int final_ordinal_ = 0;
};

}  // namespace ko
}  // namespace epocanva

#endif  // EPOCANVA_KO_TWO_SET_COMPOSER_H_
