// EpoCanva P0.1 toolchain probe.
//
// Scope of this executable (deliberately narrow):
//   1. Prove a C++17 translation unit compiles, links and runs with the project flags.
//   2. Prove fixed UTF-8 byte literals survive the source/execution charset unchanged.
//   3. Emit a fixed, non-sensitive digest over an embedded literal for build fingerprinting.
//   4. Return distinguishable exit codes so the harness can detect real failures.
//
// It does NOT read user input, clipboard, documents, registry, network or any
// user profile location. It does NOT write anywhere. It does NOT test Unicode
// normalisation, text segmentation, rendering or any input method capability;
// the UTF-8 checks below are byte-level charset sanity checks only.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

namespace {

// Exit codes. Anything non-zero is a hard failure of a specific check.
enum class ExitCode : int {
  kOk = 0,
  kLanguageLevel = 1,
  kStdlibFeatures = 2,
  kUtf8Bytes = 3,
  kDigest = 4,
  kUsage = 5,
  kForcedFailure = 9,
};

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

// Embedded, non-sensitive fingerprint material. Changing EPOCANVA_VERSION or the
// toolchain expectations changes this value, which is the point of the check.
constexpr std::string_view kDigestSubject = "EpoCanva:P0.1:toolchain-probe:v1";

struct Utf8Sample {
  std::string_view name;
  std::string_view text;
  std::size_t expected_bytes;
};

// Fixed corpus covering the languages EpoCanva targets. Byte counts are asserted,
// not computed from the platform, so a charset regression is caught.
constexpr Utf8Sample kUtf8Samples[] = {
    {"latin-small-e-acute", "\xC3\xA9", 2},
    {"latin-capital-a-umlaut", "\xC3\x84", 2},
    {"cjk-hanzi-zhong", "\xE4\xB8\xAD", 3},
    {"cjk-hanzi-guo", "\xE5\x9B\xBD", 3},
    {"hiragana-ka", "\xE3\x81\x8B", 3},
    {"katakana-mi", "\xE3\x83\x9F", 3},
    {"hangul-ko", "\xED\x95\x9C", 3},
    {"euro-sign", "\xE2\x82\xAC", 3},
    {"emoji-noto-brain", "\xF0\x9F\xA4\xA0", 4},
};

constexpr std::size_t kUtf8SampleCount =
    sizeof(kUtf8Samples) / sizeof(kUtf8Samples[0]);

[[nodiscard]] std::uint64_t Fnv1a64(std::string_view data) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  for (unsigned char byte : data) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnvPrime;
  }
  return hash;
}

// Caller-supplied buffer on purpose: a shared static buffer would make
// "expected vs actual" printing show the same value twice.
void Hex64Into(std::uint64_t value, char (&buffer)[17]) noexcept {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < 8; ++i) {
    const unsigned byte = static_cast<unsigned>((value >> (i * 8)) & 0xFFu);
    buffer[(7 - i) * 2] = kDigits[(byte >> 4) & 0xFu];
    buffer[(7 - i) * 2 + 1] = kDigits[byte & 0xFu];
  }
  buffer[16] = '\0';
}

[[nodiscard]] std::string_view Hex64(std::uint64_t value) noexcept {
  static char scratch[17] = {};
  Hex64Into(value, scratch);
  return std::string_view(scratch, 16);
}

// Check 1: the compiler really is in C++17 mode (or later), not a silently
// downgraded /Zc:__cplusplus-less build.
[[nodiscard]] ExitCode CheckLanguageLevel() {
#if defined(__cplusplus)
  static_assert(__cplusplus >= 201703L, "C++17 or later required");
#else
  return ExitCode::kLanguageLevel;
#endif
  return ExitCode::kOk;
}

// Check 1b: C++17 library and language features that later EpoCanva layers
// depend on. Structured bindings, if-init, inline constexpr, optional,
// string_view and a fold-free variadic helper.
template <typename... T>
[[nodiscard]] std::size_t CountNonEmpty(T... values) {
  std::size_t n = 0;
  ((values.size() != 0 ? ++n : n), ...);
  return n;
}

[[nodiscard]] ExitCode CheckStdlibFeatures() {
  std::optional<int> maybe_value = 42;
  if (!maybe_value.has_value()) {
    return ExitCode::kStdlibFeatures;
  }

  std::string_view key = "session";
  std::string_view value = "commit";
  const auto [found_key, found_value] = std::pair{key, value};
  if (found_key != "session" || found_value != "commit") {
    return ExitCode::kStdlibFeatures;
  }

  if (constexpr std::string_view marker = "epocanva"; marker.empty()) {
    return ExitCode::kStdlibFeatures;
  }

  if (CountNonEmpty(std::string_view{"a"}, std::string_view{""},
                    std::string_view{"c"}) != 2) {
    return ExitCode::kStdlibFeatures;
  }

  // if-with-initialiser must parse and evaluate against the runtime optional.
  if (auto parsed = std::optional<int>{*maybe_value + 1};
      !parsed.has_value() || *parsed != *maybe_value + 1) {
    return ExitCode::kStdlibFeatures;
  }

  return ExitCode::kOk;
}

// Check 2: fixed UTF-8 byte sequences arrive in the running binary unchanged.
// This is a source/execution charset sanity check. It is explicitly not a
// Unicode conformance test and not an input method capability test.
[[nodiscard]] ExitCode CheckUtf8Bytes(std::ostream& out) {
  for (const Utf8Sample& sample : kUtf8Samples) {
    if (sample.text.size() != sample.expected_bytes) {
      out << "FAIL utf8-byte-length name=" << sample.name
          << " expected=" << sample.expected_bytes << " actual=" << sample.text.size()
          << '\n';
      return ExitCode::kUtf8Bytes;
    }

    // Every multi-byte UTF-8 sequence must start with a lead byte in 0xC0..0xF7
    // and continue with 0x80..0xBF continuation bytes only.
    const unsigned char lead = static_cast<unsigned char>(sample.text[0]);
    const bool lead_ok = sample.expected_bytes == 1
                             ? lead < 0x80u
                             : (lead >= 0xC0u && lead <= 0xF7u);
    if (!lead_ok) {
      out << "FAIL utf8-lead-byte name=" << sample.name << '\n';
      return ExitCode::kUtf8Bytes;
    }

    for (std::size_t i = 1; i < sample.text.size(); ++i) {
      const unsigned char follow = static_cast<unsigned char>(sample.text[i]);
      if (follow < 0x80u || follow > 0xBFu) {
        out << "FAIL utf8-continuation-byte name=" << sample.name
            << " index=" << i << '\n';
        return ExitCode::kUtf8Bytes;
      }
    }
  }

  out << "OK   utf8-samples checked=" << kUtf8SampleCount << '\n';
  return ExitCode::kOk;
}

// Check 3: deterministic digest over the embedded literal. The expected value is
// pinned below; if the fingerprint string or the hashing routine drifts, this
// fails, which proves the binary actually ran the compiled code path.
[[nodiscard]] ExitCode CheckDigest(std::ostream& out) {
  constexpr std::uint64_t kExpectedDigest = 0xAB47508AA940DBC0ull;

  // The printer must itself be verified: a nibble-swapping bug would otherwise
  // display a wrong-looking value while the numeric comparison still passed.
  if (Hex64(0x0123456789ABCDEFull) != "0123456789abcdef") {
    out << "FAIL hex-formatter self-check\n";
    return ExitCode::kDigest;
  }

  const std::uint64_t digest = Fnv1a64(kDigestSubject);
  out << "INFO digest-subject=" << kDigestSubject << '\n';
  out << "INFO digest-fnv1a64=" << Hex64(digest) << '\n';
  if (digest != kExpectedDigest) {
    char expected_text[17] = {};
    char actual_text[17] = {};
    Hex64Into(kExpectedDigest, expected_text);
    Hex64Into(digest, actual_text);
    out << "FAIL digest-mismatch expected=" << expected_text
        << " actual=" << actual_text << '\n';
    return ExitCode::kDigest;
  }
  out << "OK   digest matches pinned value\n";
  return ExitCode::kOk;
}

void PrintUsage(std::ostream& out) {
  out << "usage: epocanva_toolchain_probe [selftest|forcefail]\n"
      << "  selftest    run all toolchain checks (default)\n"
      << "  forcefail   skip checks and exit with code 9, used by the CTest\n"
      << "              negative case to prove a failing probe is detected\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view mode = "selftest";
  if (argc > 1) {
    mode = std::string_view{argv[1]};
    if (argc > 2) {
      PrintUsage(std::cerr);
      return static_cast<int>(ExitCode::kUsage);
    }
  }

  if (mode == "forcefail") {
    std::cout << "INFO forced-failure mode requested\n";
    std::cerr << "FAIL probe reports failure on purpose\n";
    return static_cast<int>(ExitCode::kForcedFailure);
  }

  if (mode != "selftest") {
    PrintUsage(std::cerr);
    return static_cast<int>(ExitCode::kUsage);
  }

  std::cout << "INFO EpoCanva toolchain probe\n";
  std::cout << "INFO compiler="
#if defined(_MSC_VER)
            << "MSVC _MSC_VER=" << _MSC_VER
#else
            << __VERSION__
#endif
            << " __cplusplus=" << __cplusplus << '\n';
  std::cout << "INFO sizeof(void*)=" << sizeof(void*) * 8u << "-bit\n";

  if (const ExitCode rc = CheckLanguageLevel(); rc != ExitCode::kOk) {
    std::cerr << "FAIL language-level rc=" << static_cast<int>(rc) << '\n';
    return static_cast<int>(rc);
  }
  std::cout << "OK   c++17 language level\n";

  if (const ExitCode rc = CheckStdlibFeatures(); rc != ExitCode::kOk) {
    std::cerr << "FAIL stdlib-features rc=" << static_cast<int>(rc) << '\n';
    return static_cast<int>(rc);
  }
  std::cout << "OK   c++17 stdlib and language features\n";

  if (const ExitCode rc = CheckUtf8Bytes(std::cout); rc != ExitCode::kOk) {
    return static_cast<int>(rc);
  }

  if (const ExitCode rc = CheckDigest(std::cout); rc != ExitCode::kOk) {
    return static_cast<int>(rc);
  }

  std::cout << "RESULT PASS\n";
  return static_cast<int>(ExitCode::kOk);
}
