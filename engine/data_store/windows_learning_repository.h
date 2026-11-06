#ifndef EPOCANVA_DATA_STORE_WINDOWS_LEARNING_REPOSITORY_H_
#define EPOCANVA_DATA_STORE_WINDOWS_LEARNING_REPOSITORY_H_

#include "learning_store.h"

#include <filesystem>
#include <functional>
#include <string>

namespace epocanva {
namespace learning {

// Queried synchronously, never retained. An empty/throwing callback is Unknown.
// Record's caller must hold the current host permission read-lock for the entire
// call and must already have confirmed the host edit. A callback is not that lock.
using PrivacyCallback = std::function<Access()>;

// Windows 10 (1709+) persistence of admitted dictionary IDs and aggregate counts. No
// text, keystrokes, host identifiers, telemetry, or network APIs are accepted.
// The ordinary Windows host process is NOT network-sandboxed by this adapter.
//
// root must be an existing, absolute local-drive directory, with no redirection
// reparse points in its ancestors. This adapter owns root/learning-v2 only. Its
// directory, stable lock, temporary file, and data file have a protected DACL
// granting only the current effective Windows user FILE_ALL_ACCESS. Insecure
// existing objects are rejected, not silently repaired. It never modifies root's
// permissions. Constructor performs NO filesystem I/O.
//
// Calls are independent transactions; neither counts nor permission are cached.
// Nonblocking cross-process exclusive locks return kBusy, never wait for a host
// edit thread. A caller may retry a kBusy result with a still-current context.
// File names encode the catalog namespace, not arbitrary schema paths. Catalog
// identity is also checked inside the bounded (kMaxStoreBytes UTF-8 bytes) file.
class WindowsLearningRepository {
 public:
  explicit WindowsLearningRepository(std::filesystem::path root);

  Status Record(const Catalog& catalog, const Selection& selection,
                const PrivacyCallback& privacy) const noexcept;
  Status RecordFeedback(const Catalog& catalog, FeedbackKind kind,
                        std::string_view candidate_id,
                        std::string_view normalized_identity,
                        const std::vector<LexemeId>& lexeme_ids, bool attributable,
                        std::uint64_t generation, bool explicit_user_action,
                        const PrivacyCallback& privacy) const noexcept;
  Status LoadSnapshot(const Catalog& catalog, const PrivacyCallback& privacy,
                      Snapshot* out) const noexcept;

  // Explicit user management is distinct from focused-field learning permission.
  // Delete works even with learning OFF or no field context; it does not invoke
  // privacy. A false explicit_user_action is denied before any filesystem I/O.
  // Removes the namespace's data and orphan temporary file without reading their
  // contents. Keeps the empty stable lock and directory to avoid split locks.
  // Deletion is not secure erasure: backups, snapshots, previously exported
  // copies, filesystem journals, and storage remanence are outside this API.
  // Revision restarts at zero after deletion. Concurrent new learning may create
  // a later record; callers should disable learning first when that is unwanted.
  Status Delete(const Catalog& catalog, bool explicit_user_action,
                const PrivacyCallback& privacy = {}) const noexcept;

  // Explicit "clear all local learning" control. Removes every namespace data file,
  // temporary stage, pre-v3 backup and .corrupt quarantine under this adapter's learning
  // directory. It keeps the directory and the empty locks. It never reads file contents,
  // so a corrupt or oversized file cannot block deletion. A writer that starts after this
  // call can create new data; callers should pause recording first when that matters.
  Status DeleteAll(bool explicit_user_action) const noexcept;

  // Explicit export returns bounded ID/count bytes to the caller, NOT a file.
  // A fresh Normal callback is required, but learning_enabled may be false and
  // no active host target is required. Unknown/Sensitive/failed callbacks deny
  // before I/O. This never changes aggregate counts or their revision.
  Status Export(const Catalog& catalog, bool explicit_user_action,
                const PrivacyCallback& privacy, std::string* out) const noexcept;

 private:
  std::filesystem::path root_;
};

}  // namespace learning
}  // namespace epocanva
#endif
