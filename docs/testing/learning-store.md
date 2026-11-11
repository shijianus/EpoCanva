# Local learning store contract (historical v2)

> **Historical (schema v2).** The current schema is v3 with profile/language
> identity, decay, negative evidence, manual entries and read-while-paused
> semantics. See `docs/testing/learning-store-v3.md` and ADR 0011. This file keeps
> the v2 contract for audit history; do not use it as the current specification.

The core in `engine/data_store/learning_store.h` accepts admitted dictionary IDs and aggregate counts. It accepts no keystroke sequence, preedit, committed text, clipboard content, field name, application identity or timestamp. IDs are reversible against the admitted dictionary and remain personal data; they are not anonymisation or encryption.

A catalog binds one language/schema namespace, a SHA-256 dictionary identity and a unique nonzero set of admitted IDs. The caller resolves those IDs from dictionary rows. Arbitrarily hashing user input does not admit it. A language or dictionary identity mismatch rejects the entire file. English IDs use the independently versioned `StableLexemeId` contract from the English core.

`Record` requires a freshly obtained Normal field classification, enabled learning, nonzero context target and generation, and a confirmed successful host edit. Selection target and generation must match the current access decision. Sensitive and Unknown classifications fail before mutation. The caller must hold the host permission read-lock during the operation. An earlier successful engine call, candidate query or settings save is not permission to persist later. Candidate publication and engine commit production do not establish that the host accepted the text.

A sample contains at most eight admitted dictionary words. Context contains only the preceding admitted dictionary ID plus adjacent pairs within the explicitly selected words. The core retains no sequence between calls; the caller must clear its preceding ID on focus change, external edits, privacy denial, language switch and cancellation. At most 2,048 word records and 2,048 pair records are retained. Counts saturate at 1,000,000,000. Capacity eviction removes the lowest count, with stable numeric-ID tie breaking. The monotonically increasing transaction revision refuses to wrap.

The version 2 encoding is an ASCII UTF-8 byte stream with a format header, namespace, catalog SHA-256, revision, word records and pair records. It is bounded to 262,144 bytes. Parsing rejects duplicate records, invalid or unknown IDs, count overflow, zero counts, unsupported versions, truncated final rows and wrong catalog identities. Parsing is atomic: a malformed file leaves live counts unchanged. Version 1 word-only files migrate to version 2 without inventing pair counts.

Serialization and ranking snapshots require a fresh normal-field decision. Explicit export and deletion are user data-management operations. They must remain available after learning has been disabled; they do not authorize passive reads or future learning. Exported records can reveal a personal vocabulary. Deletion removes local records written by EpoCanva; it does not remove separately exported files, system backups, unreclaimed filesystem blocks or content already accepted by a host application. It does not promise secure erasure.

The core is not internally concurrent. The Windows repository adapter must hold one stable cross-process transaction lock across load, record and atomic replacement, recheck permission immediately before writing, and use a current-user-only protected DACL. A callback alone cannot hold a TSF field stable; the host adapter still owns the permission-lock lifetime. Product wiring must not enable learning before successful-host-write and current-permission evidence reaches this boundary.

The standalone test project is `tests/learning_core`. Its seven behavior groups check denied access, stale edits, ID/catalog isolation, aggregate counts, atomic migration, malformed input, bounds and explicit data management. Its eighth test checks exact intentional exit code 1 with the existing `assert_exit.cmake` wrapper, so a missing executable is not accepted as the negative control. Windows file/ACL/concurrency tests are separate from this pure C++17 core.

These tests validate the storage contract. They do not establish integrated English learning, cross-application rank consistency or five-language product readiness. Runtime code performs no network I/O. A normal Windows TSF DLL in a host process has no OS network sandbox; the offline guarantee is architectural and requires review and build policy. A future Android artifact can enforce offline operation by omitting `android.permission.INTERNET`; there is no Android artifact in this phase.
