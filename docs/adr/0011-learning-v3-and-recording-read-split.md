# ADR 0011 — Learning schema v3, profile-scoped persistence, and the recording/read split

- Status: Accepted
- Date: 2026-11-06
- Phase: D (local personalisation closure)

## Context

`engine/data_store` persisted schema v2: admitted lexeme IDs, word counts and
adjacent-pair counts. It had no profile identity, timestamps, decay metadata,
negative evidence or manual entries. `engine/ranking` held event semantics only in
memory. The Windows product set `learning_enabled = 0` in the unified backend and
supplied no learning root, so durable product learning was never enabled.

The phase D brief requires a v3 model with profile/language identity, stable
candidate identity, positive and negative summaries, a defined decay unit, a
manual flag and schema version; migration from v1/v2; atomic write and corruption
recovery; explicit read/write permission separation; and a product closure only
if a field-permission lock covers the entire worker call chain.

## Decision

1. **Schema v3 is a superset of the existing bounded store.** The header stores
   `namespace`, `catalog-sha256`, `profile-id`, `language` (primary subtag),
   `decay-unit=ms`, `half-life-ms` and `revision`. Legacy `w`/`b` rows remain for
   the lexical ranker. New `p` rows hold:
   - stable `candidate_id` (derived as `lex:<id>[+<id>...]` when the caller does
     not supply one);
   - bounded normalized lexical identity (UTF-8; never a keystroke stream);
   - admitted lexeme IDs;
   - positive/negative values in thousandths of a ranking point;
   - `last_tick_ms`;
   - `manual` flag.

2. **A profile is a storage identity.** The backend builds a per-session catalog
   with `profile_id`, primary `language`, and a namespace
   `backend.<language>.p<profile-id>.v1` (legacy sessions use `.legacy.v1`).
   A full BCP 47 tag is never stored in the short language slot. Two keyboard
   layouts of the same language do not silently share learning.

3. **Decay unit is UTC milliseconds since the Unix epoch.** Positive and negative
   summaries are stored as raw values plus `last_tick_ms`; reads decay to the
   caller's tick without mutating durable state. Writes advance decay to the event
   tick before adding a gain. A tick older than the stored timestamp is a no-op
   (clock rollback cannot increase a preference); a delta beyond the documented
   clamp cannot overflow. Version 1/2 migration uses the caller's tick as the
   baseline instead of decaying from the epoch.

4. **Read and write are different gates.** `Read`/`LoadSnapshot` require a fresh
   Normal field and matching target/generation, but not `learning_enabled`.
   `Record`/`RecordFeedback` additionally require `learning_enabled`. This makes
   "pause recording" stop new writes while existing preferences still affect
   ranking, a behaviour the settings UI states explicitly. Sensitive and Unknown
   fields deny both paths. Explicit clear/export/manual removal use their own
   explicit user action and are not blocked by the recording toggle.

5. **Persistence is repaired, not silently reset.** Version 1/2 files are
   migrated in place to v3; the original bytes are first copied to a private
   `.v2` backup. A malformed v3 file is renamed to `.corrupt` (with a bounded
   numeric suffix) and the caller receives `kInvalidFormat`; a later write starts
   from an empty in-memory store. Wrong catalog/version files are preserved with
   `kCatalogMismatch`/`kUnsupportedVersion`. Every data, temporary, backup and
   quarantine artefact is removed by the explicit namespace clear; `DeleteAll`
   covers every profile. No artefact is auto-restored.

6. **Adaptive ranking remains bounded and uncalibrated.** The backend reads v3
   preferences and installs them into `LearningEventModel` through a persistence
   setter, not a synthetic event. `AdaptiveRanker` re-ranks the real completion
   menu using the backend order as a normalized prior and a small personal term
   (`personal=0.25`, `negative=0.25`, engineering defaults). The raw candidate is
   kept out of the pass and remains first. Next-word/next-unit predictions keep
   their context-model order in this phase. This is not claimed as calibrated
   ranking; phase E must evaluate it offline.

7. **Product writes remain closed.** `ProductLearningWritesPermitted()` returns
   false because the TSF permission verdict is a point-in-time query, not an
   OS-enforced lock held across observation, queueing, worker execution, commit
   acknowledgement and persistence. The product passes its user-data root so
   existing v3 preferences can be read, exposes an unavailable recording switch,
   and exposes a real clear action. Flipping this to true requires evidence that
   does not exist yet.

## Rejected alternatives

- **Separate database/server.** Adds a second storage system, a new attack surface
  and cross-process state without addressing the missing field lock.
- **Persist the event log or raw text/keystrokes.** Violates the privacy boundary
  and the bounded-storage requirement; aggregate summaries are sufficient.
- **Make the existing v2 parser accept optional v3 lines without a version.**
  Ambiguous and not safely migratable. A versioned header and explicit `p` rows
  are cheaper to verify.
- **Enable product writes because the test harness can call the repository.**
  A test double is not a field-permission lock. It would violate AGENTS §2/§3.
- **Add the backend's raw model score to the ranker's features.** The scales are
  not calibrated. The backend order is used as a normalized prior instead.

## Consequences

- `LearningStore` and `WindowsLearningRepository` tests gained migration, decay,
  negative/manual, corruption/quarantine, paused-read and delete-all cases.
- The backend integration test `backend.learning.persistence_ranking` proves a
  persisted preference reorders a real completion menu, survives a backend
  restart, and applies while recording is paused.
- The settings UI reports that recording is unavailable and offers clear without
  pretending that paused recording erases preferences.
- Old `backend.<language>.v1` language-only namespaces are not auto-imported into
  profile namespaces. No shipped product ever wrote them because product learning
  was disabled; repository-level v1/v2 migration is verified under a single
  catalog.
- Remaining blockers: real TSF host field-lock evidence, current-package host
  acceptance, calibrated ranking evaluation, and the full phase E/F/G/H/R work.
