// EpoCanva P2.4-recovery -- the file digest the pilot data identity check is built on.
//
// WHY THIS EXISTS. The pilot word list is derived data that lives under build/ and is pinned
// by data/manifests/english-pilot.sources.lock.json. "The file the engine is about to read is
// the file the manifest describes" cannot be checked by row count or by file size -- a
// truncated list has both smaller and still parses cleanly -- so the identity check needs the
// digest. A digest is also the one thing that proves no run wrote to the original.
//
// WHERE THE ALGORITHM LIVES. The digest comes from Windows CNG: BCryptOpenAlgorithmProvider
// with BCRYPT_SHA256_ALGORITHM, then BCryptCreateHash / BCryptHashData / BCryptFinishHash.
// There is no SHA-256 implementation in this repository. An earlier revision of this file
// hand-rolled the compression function and derived the round constants from the roots of the
// first 64 primes; the first version of that was wrong in two places, which is recorded in
// the recovery status report rather than repeated here. Which primitive is used is a build
// fact; what the pinned file hashes to stays a manifest fact.
//
// SCOPE. Test-harness helper, not product code: nothing in engine/ uses it, it is never
// linked into rime.dll, and bcrypt.lib is linked only into the test executables that compile
// this file. The interface is Windows-only and this round makes no cross-platform claim.
//
// ITS OWN CORRECTNESS is not taken on faith. recovery.pilot_sha256.known_vectors hashes
// fixture bytes of known length against digests computed OUTSIDE this helper (python hashlib,
// GNU coreutils sha256sum and PowerShell Get-FileHash, which agree with each other), and the
// pilot modes compare the real word list against the digest recorded in the manifest -- which
// the configure-time CMake file(SHA256) check verifies again with a third implementation. A
// broken digest therefore shows up as an identity rejection or a red vector case, never as a
// quiet pass.
//
// UNITS (AGENTS.md section 5): every length here is in bytes (UTF-8 code units on disk). The
// chunk size bounds one read; the digest is 32 bytes rendered as 64 lowercase hex characters,
// the same form the manifest records.
#ifndef EPOCANVA_RECOVERY_PILOT_SHA256_H_
#define EPOCANVA_RECOVERY_PILOT_SHA256_H_

#include <filesystem>
#include <string>

namespace epocanva {
namespace recovery {

// Which step failed. The three error kinds are kept apart on purpose: "this file could not be
// opened", "it could not be read to the end" and "it is not the pinned file" are different
// facts, and collapsing the first two into the third would let an I/O problem look like a
// data-identity finding.
enum class DigestFailure {
  kNone,        // the digest was computed
  kOpenFailed,  // CreateFileW failed: absent, not a regular file, or not readable
  kReadFailed,  // ReadFile failed part-way through
  kHashFailed,  // a CNG call returned an error status, or reported an unexpected size
};

// The name that goes into an identity-rejection reason. Never empty.
const char* DigestFailureName(DigestFailure failure);

struct FileDigest {
  // Defaulted to an error so a struct that was never filled in cannot read as a pass.
  DigestFailure failure = DigestFailure::kHashFailed;
  std::string hex;     // 64 lowercase hex characters when kNone; EMPTY for every failure
  std::string detail;  // the win32 error or NTSTATUS that produced the failure
};

// Reads `file` in binary and streams it into CNG SHA-256 in fixed-size blocks, so a 500 KB
// input costs no more memory than one block. No digest is returned for any failure: the hex
// field stays empty rather than carrying a zero-filled or partial value, so an error can never
// accidentally equal an expected digest.
FileDigest Sha256FileDigest(const std::filesystem::path& file);

// The same result as a bare digest string: empty for every failure kind. Callers that only
// compare digests use this; callers that report WHY must use Sha256FileDigest.
std::string Sha256HexOfFile(const std::filesystem::path& file);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_PILOT_SHA256_H_
