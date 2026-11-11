#ifndef EPOCANVA_COMPOSITION_TEST_SUPPORT_H_
#define EPOCANVA_COMPOSITION_TEST_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace composition_test {

class TestContext {
 public:
  void Check(bool condition, const char* case_id) {
    ++assertion_count_;
    if (condition) return;
    ++failure_count_;
    // Only synthetic case identifiers and assertion counts, never input/output.
    if (failure_count_ <= 20) std::cerr << "CHECK FAIL case=" << case_id << '\n';
    if (failure_count_ == 21) std::cerr << "Further failing case identifiers suppressed\n";
  }

  int Finish(const char* suite_id) const {
    std::cout << (failure_count_ == 0 ? "RESULT PASS" : "RESULT FAIL")
              << " suite=" << suite_id << " assertion_count=" << assertion_count_
              << " failure_count=" << failure_count_ << '\n';
    return failure_count_ == 0 ? 0 : 1;
  }

 private:
  std::size_t assertion_count_ = 0;
  std::size_t failure_count_ = 0;
};

// Independent strict decoder, including four-byte scalars that the two cores do
// not produce. Rejects overlong forms, surrogate scalars, truncation and >10FFFF.
inline bool IsStrictUtf8(std::string_view text_utf8) {
  std::size_t byte_offset = 0;
  while (byte_offset < text_utf8.size()) {
    const auto first_byte = static_cast<unsigned char>(text_utf8[byte_offset++]);
    if (first_byte <= 0x7f) continue;
    std::size_t continuation_byte_count = 0;
    std::uint32_t unicode_scalar = 0;
    std::uint32_t minimum_unicode_scalar = 0;
    if (first_byte >= 0xc2 && first_byte <= 0xdf) {
      continuation_byte_count = 1;
      unicode_scalar = first_byte & 0x1f;
      minimum_unicode_scalar = 0x80;
    } else if (first_byte >= 0xe0 && first_byte <= 0xef) {
      continuation_byte_count = 2;
      unicode_scalar = first_byte & 0x0f;
      minimum_unicode_scalar = 0x800;
    } else if (first_byte >= 0xf0 && first_byte <= 0xf4) {
      continuation_byte_count = 3;
      unicode_scalar = first_byte & 0x07;
      minimum_unicode_scalar = 0x10000;
    } else {
      return false;
    }
    if (continuation_byte_count > text_utf8.size() - byte_offset) return false;
    for (std::size_t byte_count = 0; byte_count < continuation_byte_count; ++byte_count) {
      const auto next_byte = static_cast<unsigned char>(text_utf8[byte_offset++]);
      if ((next_byte & 0xc0) != 0x80) return false;
      unicode_scalar = unicode_scalar * 64 + (next_byte & 0x3f);
    }
    if (unicode_scalar < minimum_unicode_scalar || unicode_scalar > 0x10ffff ||
        (unicode_scalar >= 0xd800 && unicode_scalar <= 0xdfff)) return false;
  }
  return true;
}

inline bool IsUtf8ByteBoundary(std::string_view text_utf8, std::size_t byte_offset) {
  if (byte_offset > text_utf8.size()) return false;
  if (byte_offset == text_utf8.size()) return true;
  return (static_cast<unsigned char>(text_utf8[byte_offset]) & 0xc0) != 0x80;
}

inline std::uint32_t DecodeOneThreeByteScalar(std::string_view text_utf8) {
  if (text_utf8.size() != 3 || !IsStrictUtf8(text_utf8)) return 0xffffffffu;
  const auto first_byte = static_cast<unsigned char>(text_utf8[0]);
  const auto second_byte = static_cast<unsigned char>(text_utf8[1]);
  const auto third_byte = static_cast<unsigned char>(text_utf8[2]);
  if ((first_byte & 0xf0) != 0xe0) return 0xffffffffu;
  return static_cast<std::uint32_t>((first_byte & 0x0f) * 4096 +
                                    (second_byte & 0x3f) * 64 + (third_byte & 0x3f));
}

template <typename Composer>
void Feed(TestContext& test, Composer& composer, std::string_view input_ascii,
          const char* case_id) {
  using Result = decltype(composer.PushAscii(' '));
  for (char ascii_byte : input_ascii) {
    test.Check(composer.PushAscii(ascii_byte) == Result::kAccepted, case_id);
  }
}

template <typename Snapshot>
void CheckSnapshot(TestContext& test, const Snapshot& snapshot,
                   std::size_t expected_input_step_count, const char* case_id) {
  test.Check(IsStrictUtf8(snapshot.text_utf8), case_id);
  test.Check(snapshot.caret_utf8_bytes == snapshot.text_utf8.size(), case_id);
  test.Check(snapshot.active_begin_utf8_bytes <= snapshot.text_utf8.size(), case_id);
  const bool span_fits = snapshot.active_begin_utf8_bytes <= snapshot.text_utf8.size() &&
                        snapshot.active_size_utf8_bytes <=
                            snapshot.text_utf8.size() - snapshot.active_begin_utf8_bytes;
  test.Check(span_fits, case_id);
  test.Check(span_fits && snapshot.active_begin_utf8_bytes + snapshot.active_size_utf8_bytes ==
                             snapshot.text_utf8.size(), case_id);
  test.Check(IsUtf8ByteBoundary(snapshot.text_utf8, snapshot.active_begin_utf8_bytes), case_id);
  test.Check(snapshot.input_step_count == expected_input_step_count, case_id);
  // Each accepted ASCII-byte input step contributes at most three UTF-8 bytes.
  test.Check(snapshot.text_utf8.size() <= 3 * expected_input_step_count, case_id);
}

template <typename Snapshot>
void CheckSameSnapshot(TestContext& test, const Snapshot& actual, const Snapshot& expected,
                       const char* case_id) {
  test.Check(actual.text_utf8 == expected.text_utf8, case_id);
  test.Check(actual.caret_utf8_bytes == expected.caret_utf8_bytes, case_id);
  test.Check(actual.active_begin_utf8_bytes == expected.active_begin_utf8_bytes, case_id);
  test.Check(actual.active_size_utf8_bytes == expected.active_size_utf8_bytes, case_id);
  test.Check(actual.input_step_count == expected.input_step_count, case_id);
}

template <typename Composer>
void ExpectState(TestContext& test, const Composer& composer, std::string_view expected_utf8,
                 std::size_t expected_active_begin_utf8_bytes,
                 std::size_t expected_active_size_utf8_bytes,
                 std::size_t expected_input_step_count, const char* case_id) {
  const auto snapshot = composer.GetPreedit();
  CheckSnapshot(test, snapshot, expected_input_step_count, case_id);
  test.Check(snapshot.text_utf8 == expected_utf8, case_id);
  test.Check(snapshot.active_begin_utf8_bytes == expected_active_begin_utf8_bytes, case_id);
  test.Check(snapshot.active_size_utf8_bytes == expected_active_size_utf8_bytes, case_id);
}

void RunJapaneseTests(TestContext& test);
void RunKoreanTests(TestContext& test);
void RunInvariantTests(TestContext& test);

}  // namespace composition_test

#endif  // EPOCANVA_COMPOSITION_TEST_SUPPORT_H_
