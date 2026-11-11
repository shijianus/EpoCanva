# Learning store v3 contract and verification

Status: implemented, unit/integration tested; Windows real-host product writes remain
closed. This document replaces the v2 descriptions in `learning-store.md` and
`learning-store-windows.md` for current behaviour; those files keep their historical
v2 evidence and are marked as historical.

## 1. Units and identity

| Value | Unit / bound |
| --- | --- |
| Lexeme ID | stable admitted dictionary/model row ID, nonzero `uint64` |
| Candidate ID | ASCII machine key, 1..192 UTF-8 bytes, `lex:<id>[+<id>...]` when derived |
| Normalized identity | UTF-8 lexical surface, 0..96 bytes, ASCII case folded by the session; never a keystroke stream or context |
| Positive / negative summary | milli-ranking-points (`1000` = one ranking point), `[0,2000]` / `[0,1500]` |
| Decay tick | UTC milliseconds since the Unix epoch (one unit = 1 ms) |
| Half-life | milliseconds, default 30 days, `[1, 10 years]` |
| Revision | monotonic `uint64`, refuses to wrap |
| Store file | `<= 262,144` UTF-8 bytes |
| Word / pair / preference records | 2048 / 2048 / 448 |

`Catalog` carries `name_space`, dictionary `sha256`, admitted IDs, `profile_id`,
primary `language` subtag and `half_life_ms`. A full BCP 47 tag is not stored in the
language slot. The backend uses a per-profile namespace
`backend.<language>.p<profile-id>.v1`; legacy C-ABI sessions use `.legacy.v1`.

## 2. Schema v3

The file is ASCII/UTF-8 tab-separated lines with a trailing newline:

```text
epocanva-learning	3
namespace	<name_space>
catalog-sha256	<64 lowercase hex>
profile-id	<profile>
language	<primary subtag>
decay-unit	ms
half-life-ms	<milliseconds>
revision	<n>
w	<lexeme-id>	<count>
b	<previous-id>	<lexeme-id>	<count>
p	<candidate-id>	<normalized-identity>	<id[,id...]>	<positive>	<negative>	<last-tick>	<manual 0|1>
```

Every field is validated; tab/newline/control characters are rejected in the string
fields. Candidate rows are keyed by the `(profile-id in the header, candidate-id
column)` pair, not by concatenating unvalidated delimiters. `lexeme_ids` are parsed as
decimal CSV values, must be unique, admitted, and at most eight per row. Duplicate
candidate/word/pair records, out-of-range values, malformed UTF-8, unknown IDs and
truncated files are rejected atomically.

Legacy `w`/`b` rows remain the source for the existing lexical ranker. `p` rows are
the adaptive-ranking summaries. A v3 file may contain both.

## 3. Write, read, pause and privacy

`Record` and `RecordFeedback` require a fresh Normal field, `learning_enabled`, a
nonzero target and generation matching the selection. `Record` additionally requires
`host_write_confirmed`. `Read`/`LoadSnapshot` require a fresh Normal field and matching
target/generation but do **not** require `learning_enabled`: pausing recording stops new
writes while existing preferences remain available to ranking.

| Condition | Record | Read/LoadSnapshot | Clear/Export/Manual removal |
| --- | --- | --- | --- |
| Sensitive | denied | denied | explicit action only |
| Unknown | denied | denied | explicit action only |
| Normal, learning enabled | allowed | allowed | allowed |
| Normal, learning paused | denied | allowed | allowed |
| Target/generation mismatch | stale | stale | clear independent |

The change is deliberate: the settings UI says existing local preferences still affect
candidate order while recording is paused. Sensitive/Unknown fields never enter the
read or write path. The product TSF path still does not enable durable writes because
the field-permission verdict is not held as an OS lock across the worker call chain;
see §7.

## 4. Decay, negative evidence and manual entries

- A selection adds `+600` milli-points, saturating at `2000`.
- A rejection adds `+500` negative milli-points; a superseded choice adds `+150`,
  saturating at `1500`.
- `FeedbackKind::kSelected/kRejected/kSuperseded` require attribution and the current
  candidate-menu generation. Unattributable or stale feedback is rejected.
- Manual add sets `manual=true`, positive to the saturation bound and negative to zero;
  it requires an explicit user action and a Normal, recording-enabled field.
- Manual remove requires an explicit user action and a Normal field; it is allowed while
  recording is paused. It removes the manual row, not the word counts.
- Migration from v1/v2 uses the caller's current tick as the baseline; a missing
  timestamp is not decayed from the Unix epoch.
- A backwards tick is a no-op. A forward delta above `kMaxDecayDeltaMilliseconds`
  (100 years) is clamped; a fully decayed value becomes zero, not an underflow.
- Ordinary backspace/delete/commit paths do not exist as feedback kinds. They cannot
  lower a preference structurally.
- Capacity eviction never removes a manual row for an inferred row. If the table is
  full of manual rows, an inferred add is refused with `kLimitExceeded`.
- Counts and preference units saturate; revision refuses to wrap; serialized size is
  checked before publication.

## 5. Persistence, migration, corruption and clear

`WindowsLearningRepository` keeps the existing `learning-v2` directory name (historical)
but writes schema v3. It retains the per-namespace stable lock and adds an empty
directory-wide `all.lock`: normal transactions hold it shared, `DeleteAll` holds it
exclusively.

- **v1/v2 → v3:** `Load` parses the legacy file, synthesizes bounded preferences from
  word counts (highest counts first), and establishes the caller's current tick as the
  baseline. The repository publishes v3 and first writes the original bytes to a private
  `.v2` backup. A failed migration leaves the original data path in place.
- **Corrupt v3:** `kInvalidFormat`; the original is renamed to `.corrupt` (bounded
  suffix) and the caller sees the parse error. A subsequent write starts from an empty
  store. The quarantine is still sensitive user data and is removed by clear.
- **Wrong version/catalog:** `kUnsupportedVersion` / `kCatalogMismatch`; the file is
  preserved, not quarantined or overwritten.
- **Oversized file:** `kLimitExceeded`, no allocation/read of the body; not silently
  overwritten.
- **Atomic write:** private temporary file, `FlushFileBuffers`, handle-based
  `FileRenameInfoEx` with POSIX replacement. A pre-existing reader keeps the old bytes;
  new opens observe one new revision. Orphan `.tmp` is removed by the next transaction
  or clear.
- **Clear (namespace):** removes `.dat`, `.tmp`, `.v2`, `.v2.tmp`, and `.corrupt*` without
  reading their contents; keeps the empty locks and directory.
- **DeleteAll:** removes every `n-*` data/temp/backup/quarantine artefact for every
  profile under the directory; keeps only empty locks. It is an explicit user action and
  does not require a focused field.
- **No resurrection:** no path reads `.v2`/`.tmp`/`.corrupt` automatically, so an old
  stage cannot restore cleared preferences.

Deletion is not secure erasure; backups, filesystem journals, volume snapshots,
previously exported copies and content already accepted by another application are
outside this API. DACLs are access control, not encryption or anonymisation.

## 6. Ranking bridge

`Personal()` in `engine/backend/backend_api.cc` loads the profile's repository snapshot
even when recording is paused. Legacy word/pair counts feed the existing lexical
ranker. v3 preference rows are installed into `LearningEventModel` through
`SetPreference` (a persistence setter, not a synthetic event) and `AdaptiveRanker`
re-ranks the real completion menu:

- The backend order is the prior, normalized to `[0,1]` by position. Raw model
  log-probabilities are not added to the ranker's features.
- `prefix=0`, `lexical=1`, `context=0`, `personal=0.25`, `negative=0.25`,
  `source=0`, `language=0`. These are engineering defaults, not corpus-calibrated.
- The raw candidate is kept out of the pass and remains first. If a future source puts a
  raw candidate elsewhere, the pass is skipped rather than risking displacement.
- The pass applies to `COMPLETION` menus. Next-word/next-unit predictions keep their
  context-model order in phase D.
- Deduplication/foreign-language drops that change the candidate count abort the pass
  and preserve the backend order. A malformed preference/unit input is ignored.

`backend.learning.persistence_ranking` writes a v3 file through the real repository,
queries a real English completion menu from the fixture model, observes the seeded
candidate move above the displaced one, closes and reopens the backend, observes the
same order, then opens a session with `learning_enabled=0` and observes the same order
without a new write. It then calls `DeleteAll(true)` and observes the same menu return
to its default order with no durable preference left.

## 7. Product wiring and honest limits

- `ProductSettings` has `learning_recording_enabled` (default false), serialized in
  `settings.ini`.
- `ProductLearningWritesPermitted()` returns **false**. The TSF permission verdict is a
  point-in-time query; the queued worker path has no OS-enforced field lock covering
  observation, queueing, execution, acknowledgement and persistence. The product
  therefore keeps `request.learning_enabled=0` and cannot write.
- `UnifiedBackendConfig.learning_root` is still supplied so existing v3 preferences can
  be read for ranking. The setting has no effect on basic input.
- The settings window disables the recording checkbox, explains why, shows the
  `learning-v2` data path, and provides a confirmed "Clear local learning data" action.
  The action calls `DeleteAll(true)` and reports failure; it is distinct from pausing.
- Pausing recording does not erase existing preferences; the UI says so. Clearing does
  erase the local store; neither action affects basic typing.

## 8. Tests and observed results (2026-11-06)

Root suite: `build/vs2022-x64-debug`, Debug, **78 registered; CTest exit 0 with
`100% tests passed, 0 failed out of 78`, of which 77 executed and 1 explicit skip**
(`learning.repository.symlinks`, Win32 1314). Backend suite:
`build/backend-service`, Release, **58/58 passed** (including the real five-language,
real-profile, disk migration/reopen and `backend.learning.persistence_ranking` tests).
Product suite: `build/windows-product`, Release, see the phase report for the final
count; the product parameter/TSF tests ran in the same tree.

Key cases:

| Case | Evidence |
| --- | --- |
| `learning.core.privacy` | Unknown/Sensitive deny write/read/load/serialize; paused read allowed; paused write denied |
| `learning.core.identity` | invalid profile/language/half-life, target/generation, unconfirmed edit, bad IDs, whitespace/control/UTF-8 identity |
| `learning.core.counts` | v3 round trip, candidate preference, profile/language/half-life isolation, deterministic read |
| `learning.core.migration` | v1/v2 counts/pairs, synthesized preferences, v3 header fields, future-version refusal, atomic failure |
| `learning.core.malformed` | duplicate keys, out-of-range units, bad identity, unknown IDs, truncation, oversize, revision exhaustion |
| `learning.core.bounded` | 3000 commits, 2048 words/pairs, 448 preferences, serialized size bound |
| `learning.core.feedback` | selection/rejection/superseded saturation, unattributable/stale/paused refusal |
| `learning.core.decay` | half-life, rollback no-op, clamped jump to zero, migration baseline |
| `learning.core.manual` | explicit add/remove, manual flag, negative on manual, no eviction by inferred, paused removal |
| `learning.repository.persistence` | reopen, v1 migration, `.v2` backup, v3 header, held-reader replacement |
| `learning.repository.corruption` | read-time v3 migration, quarantine, clear of data/backup/quarantine, DeleteAll across namespaces, no resurrection |
| `learning.repository.feedback` | manual preference persists, paused read, paused rejection denied, paused removal allowed |
| `backend.learning.persistence_ranking` | real completion reorder, restart persistence, paused read, raw first, no durable change, clear restores default order |
| `product.unit.extended_settings` | recording intent round trip, malformed/duplicate keys, `ProductLearningWritesPermitted=false`, real repository clear |
| `product.integration.deployment_snapshot` | engine boundary receives `learning_writes_trusted=false` and `learning_recording_enabled=false` |

## 9. Negative control

The integration invariant "a persisted preference reorders the real completion menu"
was deliberately disabled by an early `return` in `ApplyPersonalRanking`. Rebuilding
`epocanva_backend_learning_integration` and running
`backend.learning.persistence_ranking` produced:

```text
FAIL persisted preference reorders the real completion menu
FAIL preferences survive a backend restart and apply while recording is paused
0% tests passed, 1 tests failed out of 1
```

The `return` was removed, the target rebuilt, and the same test passed:

```text
100% tests passed, 0 tests failed out of 2
```

## 10. Remaining blockers

- **Product durable writes are off** until a real field-permission lock covers the
  complete worker call chain and is evidenced in a real TSF host. The storage/bridge is
  verified; the product consent switch is intentionally unavailable.
- **Real-host acceptance** of the current package remains `BLOCKED` (UAC/session
  authorization). No mock/replacement result is reported as host verification.
- **Old language-only namespaces** (`backend.<lang>.v1`) are not auto-imported into
  profile namespaces. The product never wrote them because learning was disabled;
  repository-level v1/v2 migration is verified. A future importer needs an explicit
  one-profile-at-a-time decision.
- **Ranking weights are not calibrated**; phase E must evaluate them offline and add
  mixed-language/next-word coverage.
- No CI, Android, installer/signing, model catalogue or neural product integration.
