// EpoCanva P2.4-recovery -- a self-contained SHA-256 for pilot data identity.
//
// WHY THIS EXISTS. The pilot word list is derived data that lives under build/ and is
// pinned by data/manifests/english-pilot.sources.lock.json. "The file the engine is about
// to read is the file the manifest describes" cannot be checked by row count or by file
// size -- a truncated list has both smaller and still parses cleanly -- so the identity
// check needs the digest. A hash is also the one thing that proves no run wrote to the
// original.
//
// SCOPE. This is a test-harness helper, not product code: nothing in engine/ uses it and it
// is never linked into rime.dll. It implements FIPS 180-4 SHA-256 over a file, streaming in
// fixed-size blocks so a 500 KB input costs no more memory than the block.
//
// ITS OWN CORRECTNESS is not taken on faith: the pilot modes compare the value produced
// here against the digest recorded in the manifest, which was produced by an external
// tool, so a broken implementation shows up as an identity rejection rather than as a
// quiet pass.
#ifndef EPOCANVA_RECOVERY_PILOT_SHA256_H_
#define EPOCANVA_RECOVERY_PILOT_SHA256_H_

#include <filesystem>
#include <string>

namespace epocanva {
namespace recovery {

// Lowercase hex digest of the file's bytes, or an empty string when the file could not be
// read. The caller decides whether an empty digest is a missing file or an error; this
// function performs no policy.
std::string Sha256HexOfFile(const std::filesystem::path& file);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_PILOT_SHA256_H_
