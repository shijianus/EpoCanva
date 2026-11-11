# Windows learning-store persistence verification (historical v2)

> **Historical (schema v2).** The current schema is v3 with profile/language
> identity, decay, negative evidence, manual entries, `.v2` migration backups,
> `.corrupt` quarantine and clear-all semantics. See
> `docs/testing/learning-store-v3.md` and ADR 0011. The commands and results below
> are the v2 evidence for that revision, not the current test count.

## Scope and environment

This adapter persists the actual `engine/data_store/learning_store.{h,cc}` format;
there is no parallel model or fixed candidate stub. Its API is declared in
`engine/data_store/windows_learning_repository.h`. It accepts admitted dictionary
IDs and aggregate counts, not typed text, keystrokes, timestamps, focused-field
names, or application identifiers.

Verified on 2026-11-02 with Windows x64, MSVC 19.44.35209, Windows SDK
10.0.26100.0, CMake 3.31.6-msvc6, Visual Studio 17 2022 generator, Debug.
The installed Visual Studio Professional CMake executable was used because
`command -v cmake`, `command -v ninja`, and `command -v cl` found nothing on the
Git Bash PATH. No tools were installed, no privileges were elevated, and no
system configuration was changed. All build output and disposable fixtures were
under `build/multi-learning-repository/`.

**Result: 10 tests passed; 1 real-symlink capability test skipped.** The final
CTest process returned **0**. The symlink executable invocation separately
returned **77**, reporting `CreateSymbolicLinkW` error **1314**
(`ERROR_PRIVILEGE_NOT_HELD`). This is partial symlink evidence, not a successful
symlink test. Junction rejection and other path cases did execute.

## API and authorization

`WindowsLearningRepository(root)` retains only the caller's path and does no
filesystem I/O. All operations return the core `Status` enum; there are no path,
content, or Win32 diagnostic strings in the runtime API and no runtime logging.

| Operation | Authorization | Persistent effect |
| --- | --- | --- |
| `Record(catalog, selection, privacy)` | Fresh Normal, learning enabled, nonzero matching target/generation, confirmed host edit | Reload current record, update IDs/counts once, increment revision, atomically publish |
| `LoadSnapshot(catalog, privacy, out)` | Fresh Normal, learning enabled, nonzero unchanged target/generation | Read bounded snapshot; no count or revision change |
| `Delete(catalog, explicit_user_action, privacy = {})` | Explicit user action only; callback deliberately not invoked | Delete namespace data and orphan temporary file without reading their contents |
| `Export(catalog, explicit_user_action, privacy, out)` | Explicit user action plus fresh Normal privacy; learning may be disabled, no focused target required | Return bounded versioned ID/count bytes in memory, never choose/write an export destination |

Deletion after disabling learning is intentional. The task coordinator clarified
that explicit user management must not be blocked by the focused-field learning
gate. Consequently the blanket “disabled means no storage access” rule applies
to `Record` and `LoadSnapshot`, **not** an explicit deletion or Normal-authorized
export. Nonexplicit Delete/Export deny before I/O. Unknown, Sensitive, empty, or
throwing callbacks deny learning operations and export before storage access.
Outputs are cleared on denied/failed reads and exports.

Every operation owns a fresh local gate; no permission or aggregate cache is
retained. The gate is queried before filesystem operations, immediately before
writing temporary ID/count bytes, before flushing, and immediately before atomic
publication. Callback exceptions become Unknown, never the previous grant.
Changing target/generation aborts with `kStale`.

The caller must hold its current host permission read-lock throughout Record and
must already have confirmed the host edit. A callback alone cannot atomically
freeze host state. Permission revocation after earlier allowed I/O can leave
previously performed metadata reads or staged writes; the guarantee is **no
publication of the revoked aggregate update**, not retroactive absence of those
allowed actions. Closing handles and removing an uncommitted temporary file are
unconditional cleanup even after revocation.

## Storage, concurrency, and bounded data

The existing caller root contains an adapter-owned `learning-v2` directory.
For a catalog namespace, filenames are `n-<lowercase hex of namespace UTF-8
bytes>.dat`, `.tmp`, and `.lock`. Namespace bytes never become path separators;
even a valid namespace of `..` is a harmless hex leaf. The namespace is the
transaction identity; a different catalog SHA-256 in that namespace is rejected
as `kCatalogMismatch`, not silently merged or reset.

A separate, empty `.lock` is opened without delete sharing and locked exclusively
with `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)`. The lock
covers loading the current file, validating it through the real core, recording,
serializing, writing/flushing the private temporary file, and replacement.
Contention returns `kBusy` without sleeping or unbounded retries. Reads, exports,
and deletion use the same exclusive lock. The lock file is never renamed or
removed by the adapter, including after deletion, avoiding split-lock/lost-update
races.

The existing file's byte count is checked against `kMaxStoreBytes` (262,144 UTF-8
bytes) before allocating/reading its content. Serialization is also bounded
before write. The core enforces admitted IDs, catalog identity, record limits,
count limits, migration, and revision exhaustion. Invalid/oversize stores are
not silently overwritten. Read-only files and insecure permissions are errors.

The temporary file is created with `CREATE_NEW`, exclusive sharing, and its own
private DACL. It is written and flushed, then renamed by its already-validated
handle using `FileRenameInfoEx` with
`FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS`.
No `IGNORE_READONLY` behavior is requested. This Windows 10-era API permits an
existing reader to retain the old file while later opens observe the new one.
There is no unsafe delete-then-create gap and no fallback to path-resolved source
replacement. Unsupported filesystems/API behavior fail closed with status.

The first concurrency trial exposed legacy `FileRenameInfo` returning Win32 5
at replacement despite a writable target and an available DELETE-capable handle.
The final implementation uses the documented open-target replacement semantics,
not a blanket retry of access-denied errors. A deterministic held-reader test
proves the old handle reads the complete old snapshot and new opens read exactly
one later revision. Eight repeated four-process trials then passed; each trial
checks 160 concurrent successful commits plus one baseline commit equals
revision/count **161**, including both word and pair counts.

Atomic publication and a flushed staged file do not constitute a proven
power-loss durability guarantee for every filesystem, drive cache, or cloud
filter. Power-loss/process-kill injection was not performed. A crash before
publication can leave an orphan private `.tmp`; the next admitted write or
explicit Delete removes that exact file. A successful Delete starts a new
revision epoch; subsequent fresh storage begins at revision zero.

## Paths and DACLs

Before opening storage, pure syntax validation rejects UNC, caller-supplied
extended/device paths, relative/drive-relative paths, alternate streams,
traversal, reserved DOS device components, invalid UTF-16, and trailing-dot/space
aliases. `QueryDosDeviceW` is used only on a local drive letter and must resolve
to a physical `\\Device\\HarddiskVolume<decimal>` device. Network mappings,
SUBST mappings, and unexpected device types are rejected before following them.
Subsequent opens use that resolved physical volume, not a mutable drive-letter
mapping. The volume must support persistent ACLs.

All ancestors, the caller root, and the private directory are opened without
following the final reparse point and retained without write/delete sharing for
the transaction. The adapter rejects name-surrogate/path-redirection tags
(including junctions and symlinks), unknown reparse tags, non-disk objects, and
multiply linked data/lock files. Leaves are opened with `FILE_FLAG_OPEN_NO_RECALL`;
offline/recall leaves are rejected rather than read/hydrated.

The verification workspace has OneDrive **cloud** reparse tags, which are not
path-redirection tags. Those documented cloud tags are distinguished from
junctions/symlinks. This does **not** claim that external synchronization is
blocked: production callers should choose a non-synced local root. An external
sync provider, filesystem filter, administrator, or another process acting as
the same user is outside the adapter's isolation boundary.

A fresh effective thread `TokenUser` (falling back to process token only for
`ERROR_NO_TOKEN`) supplies the SID. The adapter builds a protected DACL containing
exactly one explicit `FILE_ALL_ACCESS` allow ACE for that user, with no inherited
or world ACEs. Directories include child inheritance flags; each created file
also receives its own explicit protected descriptor. Existing adapter-owned
objects must match the current user owner and that DACL; they are rejected, not
silently repaired. Caller-root ACLs are never modified.

DACLs are access controls, not encryption, anonymization, or a same-user sandbox.
Admitted IDs/counts are still personal data.

## Deletion and export limits

Explicit deletion does not read or parse the data file, so malformed/oversize
content can still be removed. It deletes the current namespace's data and exact
orphan temporary leaf by validated handles. It retains the empty lock and private
directory; it does not recursively delete unrelated files or other namespaces.
Insecure ACLs, reparse redirections, read-only state, or busy locks can prevent
removal, and a failure does not promise a multi-file rollback.

Deletion is not secure erasure and cannot remove backups, filesystem journals,
volume snapshots, previous exports, copied files, previously held handles, or
storage remanence. After deletion, a later permitted transaction can create new
counts; disable learning first when that is unwanted. Exports contain the same
bounded dictionary-ID/count data and remain the caller's responsibility once
returned. Neither API logs exported content or sends it elsewhere.

## Offline guarantee

The Windows adapter and its dependencies in this target make no socket, HTTP,
telemetry, update, dictionary-sync, or cloud-lookup calls. This is an
**architectural** guarantee supported by source review and the explicit local
path policy, not an OS-enforced network sandbox for a DLL loaded in an arbitrary
Windows host process. External filesystem/sync providers remain outside it.
On Android, omitting `android.permission.INTERNET` can provide platform-enforced
network denial; this Windows-only task implements and verifies no Android
adapter and does not conflate the two guarantees.

## Verification matrix

| Test | Actual result | Important assertions |
| --- | --- | --- |
| `learning.repository.persistence` | Pass | Constructor/missing read no creation; reopened IDs/counts/revision; actual v1 migration; bounded file; held old-reader atomic replacement |
| `learning.repository.privacy` | Pass | Unknown/Sensitive/off/throw deny; process I/O counters unchanged for initial denial; empty callback; invalid/stale selection; no cached grant; cleared output |
| `learning.repository.revocation` | Pass | Revocation at every callback in an actual transaction, including final publish; byte-identical durable baseline; no surviving stage; final callback throw/generation change |
| `learning.repository.errors` | Pass | Oversize/corrupt/duplicate/catalog mismatch; revision exhaustion; read-only attributes/DACL; bounded lock contention and valid retry |
| `learning.repository.management` | Pass | Explicit-action requirement; export while disabled; actual core re-import; deletion while field callback throws without invoking it; orphan removal; stable lock identity; corrupt/oversize deletion |
| `learning.repository.acl` | Pass | Protected exact current-user-only ACL on directory/lock/data; exclusive stage; renamed stage ACL; broad-world negative controls rejected |
| `learning.repository.paths` | Pass | UNC/device/traversal/ADS/device-name syntax refusal; invalid catalog paths; harmless hex namespace; real junction refusal; hard-link refusal |
| `learning.repository.symlinks` | **Skipped, exit 77** | Actual fixture creation denied with Win32 1314; no elevation/configuration attempted |
| `learning.repository.concurrency` | Pass | Independent-process lock denial; deletion serializes; 4 independent writers × 40 commits, no missing word/pair counts or revision increments |
| `learning.repository.force-failure` | Pass (negative control) | Harness requires exact child exit 1 and intentional assertion text; no success marker |
| `learning.repository.invalid-mode` | Pass (negative control) | Harness requires exact child exit 2; no success marker |

The negative controls use `assert_exit.cmake`, not `WILL_FAIL`; a missing/crashed
executable cannot masquerade as the expected assertion failure. Symlink skip is
limited to the specific missing privilege error; other setup failures fail.

## Commands and observed exits

Paths in this section are sanitized. `$repo` denotes the absolute checkout path;
`$cmake` and `$ctest` denote the existing Visual Studio Professional
`Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{cmake,ctest}.exe` files.
No machine-specific compiler path is stored in a build script. These are the
actual command arguments used, with local path prefixes replaced by variables.

```sh
"$cmake" -S "$repo/tests/learning_repository" \
  -B "$repo/build/multi-learning-repository" \
  -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0
"$cmake" --build "$repo/build/multi-learning-repository" --config Debug --parallel 4
"$ctest" --test-dir "$repo/build/multi-learning-repository" -C Debug --output-on-failure
"$ctest" --test-dir "$repo/build/multi-learning-repository" -C Debug \
  --output-on-failure -R 'learning.repository.(errors|concurrency)'
"$ctest" --test-dir "$repo/build/multi-learning-repository" -C Debug \
  --output-on-failure -R 'learning.repository.concurrency'
"$ctest" --test-dir "$repo/build/multi-learning-repository" -C Debug \
  --output-on-failure -R 'learning.repository.concurrency' --repeat until-fail:8
"$repo/build/multi-learning-repository/Debug/epocanva_learning_repository_tests.exe" \
  symlinks "$repo/build/multi-learning-repository/run"
git -C "$repo" diff --check -- engine/data_store/windows_learning_repository.h \
  engine/data_store/windows_learning_repository.cc tests/learning_repository
```

- Configure and every observed Debug build completed with exit **0**.
- Initial full suite: CTest **8**, 7 pass / 3 fail. Failures were a test DACL
  restoration asking for removed WRITE_OWNER rights, actual symlink capability,
  and legacy open-target replacement under concurrent writers.
- Targeted diagnostics: CTest **8** until the actual rename error was isolated;
  a single trial also passed **0**, demonstrating why repetition was necessary.
  The diagnostic code was removed from runtime source before final verification.
- Final repeated concurrency command: exit **0**, eight consecutive trials pass.
- Final full Debug suite: exit **0**, **10 pass / 1 skip / 0 fail**, 11 registered.
- Direct symlink probe: exit **77**, explicit Win32 **1314**.
- Assigned-file `git diff --check`: exit **0**. No commit, push, or other git
  mutation was performed.

CTest details are in the build tree's `Testing/Temporary/LastTest.log` and
`CTestTestfile.cmake`. No root/product integration build, actual TSF host test,
Release build, power-cut test, mapped remote-drive fixture, or privileged symlink
test was run by this task. Main owns root integration and its separate evidence.

## Source/SDK references checked

The implementation was checked against the installed SDK, not inferred from a
previous project's behavior:

- SDK `um/securitybaseapi.h`: `GetTokenInformation` around 928,
  `AddAccessAllowedAceEx` around 200, `InitializeAcl` / security descriptor setup
  around 988, protected DACL/owner setters around 1300–1332.
- SDK `um/processthreadsapi.h`: `OpenProcessToken` / `OpenThreadToken` around
  538–558; SDK `um/AclAPI.h`: `GetSecurityInfo` around 194–203.
- SDK `um/fileapi.h`: `LockFileEx` around 955–962,
  `GetVolumeInformationByHandleW` around 889–898, `QueryDosDeviceW` around 973,
  `SetFileInformationByHandle` around 1079–1084.
- SDK `um/WinBase.h`: replacement flags and `FILE_RENAME_INFO` around 9104–9124,
  attribute tag and disposition layouts around 9151–9158;
  SDK `um/minwinbase.h`: `FileRenameInfoEx` around 204.
- SDK `um/winnt.h`: `IsReparseTagNameSurrogate` around 15739, mount-point/symlink
  and cloud tag definitions around 15753–15785.
- Existing `platforms/windows/product/product_io.cc` around 115–205 and
  `product_engine_config.cc` around 229–248 were read. Their generic path-based
  writer/default ACL and bounded-wait behavior were not reused because this
  adapter requires a different privacy/path/lock contract.
- Existing `tools/windows/tsf_lab/lab_self_test.cc` around 433–514 supplied the
  established user-mode junction fixture layout; the SDK declares its tags and
  FSCTL even though the full buffer is WDK-only.
- Microsoft reference pages inspected for handle replacement semantics:
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info>
  and
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information>.

## Owned changes and handoff

Only these task-owned source/document paths were added:

- `engine/data_store/windows_learning_repository.h`
- `engine/data_store/windows_learning_repository.cc`
- `tests/learning_repository/CMakeLists.txt`
- `tests/learning_repository/main.cc`
- `tests/learning_repository/assert_exit.cmake`
- `docs/testing/learning-store-windows.md`

The standalone test CMake links the actual `epocanva_learning_core`, creates
`epocanva_windows_learning_repository` if main has not already defined it, and
links `advapi32`. C++17 extensions are disabled and MSVC uses `/permissive-`.
There were no edits to the core, core tests, root CMake, TSF, settings, third-party
sources, licensing, or system settings by this task.

Implementation and tests were frozen after the final full Debug run. Remaining
verification blocker: execute the explicit symlink case in an already-authorized
Windows environment where fixture creation is permitted; do not silently turn
its current skip into a pass. Host permission locking, root integration,
production non-synced-root selection, crash/power-loss testing, and product
release readiness remain separate caller/integration responsibilities.
