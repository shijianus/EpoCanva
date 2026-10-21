// EpoCanva P2.4-recovery -- tests for the pilot file-digest helper itself.
//
// WHY THIS IS A SEPARATE PROGRAM. pilot_sha256.cc is the thing the pilot identity check rests
// on, and until now its own correctness was only ever implied by the pilot modes agreeing with
// the manifest. A digest helper deserves cases that need no engine: this program links nothing
// but the helper and bcrypt, is registered in both configure modes, and fails on its own if the
// digest backend is wrong.
//
// THE EXPECTATIONS ARE NOT PRODUCED HERE. Every literal below was computed with three
// implementations outside this repository -- python hashlib, GNU coreutils sha256sum and
// PowerShell Get-FileHash -- which agree with each other on all of them, and the real word list
// is compared against the digest recorded in
// data/manifests/english-pilot.sources.lock.json. The helper under test generated none of them;
// it is only asked to reproduce them. No second SHA-256 implementation is written here either.
//
// WHAT THE FIXTURES ARE. The bytes come from the two formulas documented at Kind and are written
// into this run's own scratch directory, so nothing under tests/fixtures/ or build/english-pilot/
// is ever opened for writing. Sizes are chosen around the boundaries that matter: SHA-256's
// 64-byte block and its 55/56-byte padding split, and the helper's own 65,536-byte read chunk.
//
// EXIT CODES match the stage codes in recovery_test_support.h so one report reads the same across
// the two programs: 0 the run passed, 2 a usage or configuration problem, 5 a required file is
// missing, 6 the run directory was refused, 14 an assertion failed (and no success marker).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pilot_sha256.h"

#ifndef EPOCANVA_PILOT_EXPECTED_SHA256
// Configured without -DEPOCANVA_ENGLISH_PILOT_LEXICON: no manifest digest is compiled in, so
// mode real-file has nothing to compare against and says so instead of inventing a number.
#define EPOCANVA_SHA256_UNCONFIGURED 1
#define EPOCANVA_PILOT_EXPECTED_SHA256 ""
#define EPOCANVA_PILOT_EXPECTED_BYTES 0
#endif

namespace {

using epocanva::recovery::DigestFailure;
using epocanva::recovery::DigestFailureName;
using epocanva::recovery::FileDigest;
using epocanva::recovery::Sha256FileDigest;

constexpr int kUsage = 2;
constexpr int kRequiredFileMissing = 5;
constexpr int kRunDirectoryFailed = 6;
constexpr int kAssertionFailed = 14;

// The same guard the engine harness uses: this program deletes only inside a leaf whose parent
// carries exactly this name, which nothing outside the generated CTest commands produces.
constexpr char kRunRootGuard[] = "rime-english-recovery-run";

// The read chunk pilot_sha256.cc streams with, restated so the chunk-edge fixtures can be named
// in the same unit. It decides no expectation; it only says which size the edges are at.
constexpr std::size_t kHelperChunkBytes = 65536;

// --- the fixtures ---------------------------------------------------------------

enum class Kind {
  kLiteral,  // the exact bytes of `text`
  kPattern,  // byte i = (i * 31 + 7) & 0xFF, i zero-based, so the full 0..255 range recurs
  kRamp,     // byte i = i, i.e. 0x00 through 0xFF once each
};

struct Vector {
  const char* name;
  Kind kind;
  const char* text;   // kLiteral only
  std::size_t bytes;  // kPattern and kRamp only
  const char* expected;
};

// The 64-lowercase-hex-character expectations, as printed by the three external tools named
// above. empty / "abc" / the 56-byte message are FIPS 180-4's own sample digests.
const Vector kVectors[] = {
    {"empty", Kind::kLiteral, "", 0,
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", Kind::kLiteral, "abc", 3,
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"nist_56", Kind::kLiteral,
     "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"pat_1", Kind::kPattern, "", 1,
     "ca358758f6d27e6cf45272937977a748fd88391db679ceda7dc7bf1f005ee879"},
    {"pat_55", Kind::kPattern, "", 55,
     "8aa994584139d128848eeebc4e815639ba5ab6e6e39574195a63ac4f14f7c43b"},
    {"pat_56", Kind::kPattern, "", 56,
     "ad574708f75c044c9b85de64cb568ee7711ff4f36448c6242f053ba8f6cc2b63"},
    {"pat_57", Kind::kPattern, "", 57,
     "5b46e502092be01b1100193e089fdda95638c12e19a1d24f308eb2c3d3ae849d"},
    {"pat_63", Kind::kPattern, "", 63,
     "280ed3e8ff1df845b2e7dfe6ac6cee817bef20e783cc65abc41b818b4d2fe076"},
    {"pat_64", Kind::kPattern, "", 64,
     "c6ab9724ade5b6a7a1edfffb12f3aa9181351355af8fd08c919952ad211339dd"},
    {"pat_65", Kind::kPattern, "", 65,
     "788367c73c7ddf4c53f65e68cc0d943e6227ab55b0e78ba63ace822b1c6301c0"},
    {"pat_200", Kind::kPattern, "", 200,
     "44cae5223d431caed4a9e32271d6abf17c3f2f4abac45fcdb48a99fcc6072a09"},
    // The whole byte range, including 0x00 and 0xFF, which a 7-bit-ASCII word list never gives.
    {"ramp_256", Kind::kRamp, "", 256,
     "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880"},
    // Around the helper's own read chunk: one short, exactly one, one over.
    {"pat_65535", Kind::kPattern, "", kHelperChunkBytes - 1,
     "91d0a03af0efeeb5568bbdcf2bd7fe1c9d932c4d22db3643c7586c1bd1afbeb1"},
    {"pat_65536", Kind::kPattern, "", kHelperChunkBytes,
     "ef4636928161808e87035fa51983821677527ccd9661991c5d0126a778b2268a"},
    {"pat_65537", Kind::kPattern, "", kHelperChunkBytes + 1,
     "74f3260f4a9adaf7271c910ba2d111dbae7a176afb487e1b7f655897eadf1093"},
};

std::size_t FixtureSize(const Vector& v) {
  return v.kind == Kind::kLiteral ? std::string(v.text).size() : v.bytes;
}

std::vector<unsigned char> FixtureBytes(const Vector& v) {
  if (v.kind == Kind::kLiteral) {
    const std::string text(v.text);
    return std::vector<unsigned char>(text.begin(), text.end());
  }
  const std::size_t n = v.bytes;
  std::vector<unsigned char> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned int value =
        (v.kind == Kind::kRamp) ? static_cast<unsigned int>(i % 256u)
                                : static_cast<unsigned int>((i * 31u + 7u) & 0xFFu);
    out.push_back(static_cast<unsigned char>(value));
  }
  return out;
}

const Vector* FindVector(const std::string& name) {
  for (const Vector& v : kVectors) {
    if (name == v.name) {
      return &v;
    }
  }
  return nullptr;
}

// --- the program's own checker ---------------------------------------------------

class Checker {
 public:
  void Check(bool condition, const std::string& what, const std::string& got) {
    ++total_;
    if (condition) {
      return;
    }
    ++failed_;
    std::printf("[sha256] ASSERT FAIL %s\n", what.c_str());
    std::printf("[sha256]            got %s\n", got.c_str());
  }

  void CheckStr(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what, "\"" + got + "\" want \"" + want + "\"");
  }

  void CheckNum(long long got, long long want, const std::string& what) {
    Check(got == want, what, std::to_string(got) + " want " + std::to_string(want));
  }

  // The manifest convention: 64 lowercase hex characters, and nothing else.
  void CheckHex64(const std::string& got, const std::string& what) {
    bool shaped = got.size() == 64;
    for (const char c : got) {
      const bool digit = c >= '0' && c <= '9';
      const bool lower_hex = c >= 'a' && c <= 'f';
      if (!digit && !lower_hex) {
        shaped = false;
      }
    }
    Check(shaped, what, "\"" + got + "\" want 64 lowercase hex characters");
  }

  int total() const { return total_; }
  int failed() const { return failed_; }

 private:
  int total_ = 0;
  int failed_ = 0;
};

void Report(const std::string& stage, const std::string& detail) {
  std::printf("[sha256] %-22s %s\n", (stage + ":").c_str(), detail.c_str());
}

std::string Describe(const FileDigest& d) {
  return std::string(DigestFailureName(d.failure)) + " | sha256 \"" + d.hex + "\"" +
         (d.detail.empty() ? std::string() : std::string(" | ") + d.detail);
}

// --- command line -----------------------------------------------------------------

struct Options {
  std::filesystem::path run_root;
  std::string mode;
  std::filesystem::path pilot_lexicon;
};

bool ParseArgs(int argc, char** argv, Options* out, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg != "--run-root" && arg != "--mode" && arg != "--pilot-lexicon") {
      *error = "unrecognised argument '" + arg + "'";
      return false;
    }
    if (i + 1 >= argc) {
      *error = "'" + arg + "' needs a value";
      return false;
    }
    const std::string value = argv[++i];
    if (arg == "--run-root") {
      out->run_root = value;
    } else if (arg == "--mode") {
      out->mode = value;
    } else {
      out->pilot_lexicon = value;
    }
  }
  if (out->run_root.empty()) {
    *error = "--run-root is required";
    return false;
  }
  if (out->mode.empty()) {
    *error = "--mode is required";
    return false;
  }
  if (out->mode != "vectors" && out->mode != "identity-negatives" &&
      out->mode != "self-check-fail" && out->mode != "real-file") {
    *error = "unknown mode '" + out->mode +
             "' (vectors|identity-negatives|self-check-fail|real-file)";
    return false;
  }
  if (out->mode == "real-file") {
    if (out->pilot_lexicon.empty()) {
      *error = "mode real-file requires --pilot-lexicon";
      return false;
    }
    if (!out->pilot_lexicon.is_absolute()) {
      *error = "--pilot-lexicon must be absolute, got '" + out->pilot_lexicon.string() + "'";
      return false;
    }
  } else if (!out->pilot_lexicon.empty()) {
    *error = "--pilot-lexicon is accepted only by mode real-file";
    return false;
  }
  return true;
}

// Mirrors the harness rule: recreate this run's leaf only when its parent is the guard name.
bool PrepareRunRoot(const Options& opt, std::string* error) {
  const std::filesystem::path parent = opt.run_root.parent_path();
  if (parent.filename().u8string() != kRunRootGuard) {
    *error = "--run-root " + opt.run_root.string() + " is not a leaf under a directory named '" +
             kRunRootGuard + "'; refusing to delete anything here";
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(opt.run_root, ec)) {
    std::filesystem::remove_all(opt.run_root, ec);
    if (ec) {
      *error = "cannot remove the previous run directory " + opt.run_root.string() + " (" +
               ec.message() + ")";
      return false;
    }
  }
  std::filesystem::create_directories(opt.run_root, ec);
  if (ec) {
    *error = "cannot create " + opt.run_root.string() + " (" + ec.message() + ")";
    return false;
  }
  return true;
}

bool WriteFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes,
               std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot open " + path.string() + " for writing";
    return false;
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  out.close();
  if (!out) {
    *error = "writing " + path.string() + " did not finish";
    return false;
  }
  return true;
}

bool WriteFixture(const std::filesystem::path& dir, const Vector& v,
                  std::filesystem::path* written, std::string* error) {
  const std::filesystem::path path = dir / (std::string(v.name) + ".bin");
  if (!WriteFile(path, FixtureBytes(v), error)) {
    return false;
  }
  *written = path;
  return true;
}

// --- modes ------------------------------------------------------------------------

// Every fixture pattern hashed and compared with an expectation this program did not compute:
// the empty file, FIPS 180-4's two other sample messages, the 55/56/57 padding edges, the
// 63/64/65 block edges, a full 0x00..0xFF ramp, and the helper's chunk edges.
int RunVectors(Checker* checks, const std::filesystem::path& scratch) {
  for (const Vector& v : kVectors) {
    std::filesystem::path path;
    std::string error;
    if (!WriteFixture(scratch, v, &path, &error)) {
      checks->Check(false, std::string("fixture ") + v.name + " could not be written", error);
      continue;
    }
    std::error_code ec;
    const long long on_disk =
        static_cast<long long>(std::filesystem::file_size(path, ec));
    checks->CheckNum(on_disk, static_cast<long long>(FixtureSize(v)),
                     std::string(v.name) + ": the bytes written are the bytes on disk");

    const FileDigest d = Sha256FileDigest(path);
    checks->Check(d.failure == DigestFailure::kNone,
                  std::string(v.name) + ": the helper digested the file, with reason " +
                      Describe(d),
                  Describe(d));
    checks->CheckHex64(d.hex, std::string(v.name) + ": the digest is in the manifest's format");
    checks->CheckStr(d.hex, v.expected, std::string(v.name) + " (external expectation)");
    Report(std::string("vector ") + v.name, Describe(d));
  }
  return 0;
}

// The four ways a file can fail to be the pinned one, kept apart. Every case here is an
// independent copy or a path that was never written; no fixture and no generated word list is
// opened for writing.
int RunIdentityNegatives(Checker* checks, const std::filesystem::path& scratch) {
  std::string error;

  // 1. A path that is not there.
  const std::filesystem::path absent = scratch / "never_written.bin";
  checks->Check(!std::filesystem::exists(absent), "the absence control starts absent",
                absent.string());
  const FileDigest missing = Sha256FileDigest(absent);
  checks->Check(missing.failure == DigestFailure::kOpenFailed,
                "an absent file is refused as open-failed, with reason " + Describe(missing),
                Describe(missing));
  checks->Check(missing.hex.empty(),
                "and yields NO digest rather than a zero-filled or partial one",
                "\"" + missing.hex + "\"");
  Report("absent", Describe(missing));

  // 2. A directory where a file was expected: still an open failure, still no digest.
  const FileDigest dir = Sha256FileDigest(scratch);
  checks->Check(dir.failure == DigestFailure::kOpenFailed,
                "a directory is refused as open-failed, with reason " + Describe(dir),
                Describe(dir));
  checks->Check(dir.hex.empty(), "a directory yields no digest", "\"" + dir.hex + "\"");
  Report("directory", Describe(dir));

  // 3. Truncation. The first kHelperChunkBytes bytes of pat_65537 are, by the fixture formula,
  //    exactly pat_65536 -- so the truncated copy has a VALID digest: the other file's. That is
  //    the point: a read that stops early is an identity finding, not an I/O failure, and no
  //    error status is involved anywhere on this path.
  const Vector* longer = FindVector("pat_65537");
  const Vector* shorter = FindVector("pat_65536");
  checks->Check(longer != nullptr && shorter != nullptr, "the truncation pair exists",
                longer == nullptr ? "pat_65537 missing" : "pat_65536 missing");
  if (longer != nullptr && shorter != nullptr) {
    std::vector<unsigned char> bytes = FixtureBytes(*longer);
    const std::size_t keep = bytes.size() - 1;
    bytes.resize(keep);
    const std::filesystem::path cut = scratch / "truncated_copy.bin";
    if (WriteFile(cut, bytes, &error)) {
      const FileDigest d = Sha256FileDigest(cut);
      checks->Check(d.failure == DigestFailure::kNone,
                    "a truncated file is still readable, so it is not an I/O failure: " +
                        Describe(d),
                    Describe(d));
      checks->CheckStr(d.hex, shorter->expected,
                       "the truncated copy hashes to the shorter file's digest");
      checks->Check(d.hex != longer->expected,
                    "and that is exactly how it differs from the pinned file", d.hex);
      Report("truncated", Describe(d));
    } else {
      checks->Check(false, "the truncated copy could not be written", error);
    }
  }

  // 4. Same length, one byte changed. Size and row count are identical here; only the digest
  //    separates the two, which is the whole reason the identity check uses one.
  const Vector* ramp = FindVector("ramp_256");
  checks->Check(ramp != nullptr, "the tamper control fixture exists", "ramp_256 missing");
  if (ramp != nullptr) {
    std::vector<unsigned char> bytes = FixtureBytes(*ramp);
    const std::size_t at = 128;  // the 0x80 in the middle of the ramp
    bytes[at] = static_cast<unsigned char>(bytes[at] ^ 0x01u);
    const std::filesystem::path tampered = scratch / "tampered_copy.bin";
    if (WriteFile(tampered, bytes, &error)) {
      std::error_code ec;
      checks->CheckNum(static_cast<long long>(std::filesystem::file_size(tampered, ec)),
                       static_cast<long long>(ramp->bytes),
                       "the tampered copy has the SAME byte count as the original");
      const FileDigest d = Sha256FileDigest(tampered);
      checks->Check(d.failure == DigestFailure::kNone,
                    "the tampered copy is readable, so this is an identity finding: " +
                        Describe(d),
                    Describe(d));
      checks->CheckHex64(d.hex, "the tampered copy still digests in the manifest's format");
      checks->Check(d.hex != ramp->expected,
                    "and one flipped byte moves the digest off the pinned value", d.hex);
      Report("tampered", Describe(d));
    } else {
      checks->Check(false, "the tampered copy could not be written", error);
    }
  }

  // 5. The failure kinds are named distinctly, so a caller can never fold an I/O or CNG problem
  //    into "digest-mismatch" -- the reason pilot_tests.cc turns them into digest-open-failed /
  //    digest-read-failed / digest-hash-failed instead.
  const std::string names[4] = {DigestFailureName(DigestFailure::kNone),
                                DigestFailureName(DigestFailure::kOpenFailed),
                                DigestFailureName(DigestFailure::kReadFailed),
                                DigestFailureName(DigestFailure::kHashFailed)};
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      checks->Check(names[i] != names[j],
                    "failure kinds are distinguishable: " + names[i] + " vs " + names[j],
                    names[i] + " == " + names[j]);
    }
  }
  checks->CheckStr(names[0], "none", "the success kind is named, not a bare bool");

#ifndef EPOCANVA_SHA256_UNCONFIGURED
  // The bar this build is held to: the compiled-in expectation is a real digest, so an empty
  // hex from a failure path cannot match it.
  checks->CheckHex64(EPOCANVA_PILOT_EXPECTED_SHA256,
                     "the manifest digest compiled into this build is well-formed");
  checks->Check(missing.hex.empty() && dir.hex.empty(),
                "an error path produces no digest at all, so it cannot match it",
                "\"" + missing.hex + "\" / \"" + dir.hex + "\"");
#endif
  return 0;
}

// The control that keeps this program honest: the same verification path run against a
// deliberately corrupted expectation. Must record a failure and print no success marker;
// a vector case that cannot fail would not be a test (AGENTS.md section 3).
int RunSelfCheckFail(Checker* checks, const std::filesystem::path& scratch) {
  const Vector* abc = FindVector("abc");
  checks->Check(abc != nullptr, "the control fixture exists", "abc missing");
  if (abc == nullptr) {
    return 0;
  }
  std::filesystem::path path;
  std::string write_error;
  if (!WriteFixture(scratch, *abc, &path, &write_error)) {
    // A different code, so a scratch failure can never impersonate the designed exit 14.
    std::printf("[sha256] FAIL scratch: %s\n", write_error.c_str());
    return kRunDirectoryFailed;
  }
  const std::string wrong =
      std::string(abc->expected, 63) +
      (abc->expected[63] == 'd' ? 'e' : 'd');  // one character off the real "abc" digest
  checks->CheckStr(Sha256FileDigest(path).hex, wrong,
                   "deliberately mis-stated expectation for \"abc\"");
  return 0;
}

// The artefact itself: the generated pilot word list, hashed by this helper and compared with
// the full digest the manifest records. Registered only in a pilot-configured build.
int RunRealFile(Checker* checks, const Options& opt) {
#ifdef EPOCANVA_SHA256_UNCONFIGURED
  (void)checks;
  (void)opt;
  std::printf("[sha256] FAIL configuration: this build directory was configured without "
              "-DEPOCANVA_ENGLISH_PILOT_LEXICON, so mode real-file has no manifest digest "
              "compiled in to compare against\n");
  return kUsage;
#else
  std::error_code ec;
  if (!std::filesystem::exists(opt.pilot_lexicon, ec)) {
    std::printf("[sha256] FAIL required file: %s does not exist\n",
                opt.pilot_lexicon.string().c_str());
    return kRequiredFileMissing;
  }
  const long long bytes =
      static_cast<long long>(std::filesystem::file_size(opt.pilot_lexicon, ec));
  checks->CheckHex64(EPOCANVA_PILOT_EXPECTED_SHA256,
                     "the compiled-in manifest expectation is well-formed");
  checks->CheckNum(bytes, static_cast<long long>(EPOCANVA_PILOT_EXPECTED_BYTES),
                   "the pilot word list byte count matches the manifest");
  const FileDigest d = Sha256FileDigest(opt.pilot_lexicon);
  checks->Check(d.failure == DigestFailure::kNone,
                "the pilot word list was digested, with reason " + Describe(d), Describe(d));
  checks->CheckStr(d.hex, EPOCANVA_PILOT_EXPECTED_SHA256,
                   "the pilot word list digest matches the manifest");
  Report("pilot file", opt.pilot_lexicon.string());
  Report("pilot digest", Describe(d));
  return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string error;
  if (!ParseArgs(argc, argv, &opt, &error)) {
    std::printf(
        "usage: rime_english_recovery_sha256 --run-root <a leaf under "
        "rime-english-recovery-run> --mode "
        "<vectors|identity-negatives|self-check-fail|real-file> [--pilot-lexicon <absolute "
        "path>, accepted only by mode real-file]\n");
    std::printf("[sha256] FAIL arguments: %s\n", error.c_str());
    return kUsage;
  }
  if (!PrepareRunRoot(opt, &error)) {
    std::printf("[sha256] FAIL run-root: %s\n", error.c_str());
    return kRunDirectoryFailed;
  }
  std::printf("[sha256] run-root:              %s (recreated empty)\n",
              opt.run_root.string().c_str());
  std::printf("[sha256] mode:                  %s\n", opt.mode.c_str());
  std::printf("[sha256] backend:               Windows CNG SHA256 (bcrypt); chunk %zu bytes\n",
              kHelperChunkBytes);

  Checker checks;
  int rc = 0;
  if (opt.mode == "real-file") {
    rc = RunRealFile(&checks, opt);
  } else {
    const std::filesystem::path scratch = opt.run_root / "scratch";
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    if (ec) {
      std::printf("[sha256] FAIL run-root: cannot create %s (%s)\n", scratch.string().c_str(),
                  ec.message().c_str());
      return kRunDirectoryFailed;
    }
    if (opt.mode == "vectors") {
      rc = RunVectors(&checks, scratch);
    } else if (opt.mode == "identity-negatives") {
      rc = RunIdentityNegatives(&checks, scratch);
    } else {
      rc = RunSelfCheckFail(&checks, scratch);
    }
    std::printf("[sha256] scratch:               %s\n", scratch.string().c_str());
  }

  std::printf("[sha256] assertions:            %d checked / %d failed\n", checks.total(),
              checks.failed());
  if (rc != 0) {
    return rc;
  }
  if (checks.failed() > 0) {
    std::printf("[sha256] RESULT FAIL\n");
    return kAssertionFailed;
  }
  std::printf("RESULT PASS\n");
  return 0;
}
