# AGENTS.md — rules for anyone (human or automated) working on EpoCanva

Read this before changing anything. When a rule here conflicts with a task brief,
follow the task brief and flag the conflict in your report.

## 1. Language priority (fixed, do not re-order)

```
English > Chinese > French > Japanese > Korean > generic i18n
```

- English is the product's differentiating capability. It is **never** acceptable to
  implement it as an ASCII pass-through, or as "one dictionary-backed Translator".
  English support must cover word selection with frequency/context learning,
  morphological variation (plural, -s/-ed/-ing, possessive, contraction),
  spelling correction, auto-segmentation of run-together input, abbreviation
  expansion, and cross-app consistency.
- Do not silently downgrade English behaviour to satisfy another language's flow.

## 2. Privacy and offline rules (non-negotiable)

- Runtime code performs **no network I/O** — no telemetry, no crash reporting,
  no update pings, no dictionary sync, no cloud lookups.
- Never log raw keystrokes, raw composition buffers, committed text, clipboard
  content, focused-field names, or app identifiers that reveal user content.
- Password fields, one-time-code fields and other privacy-classified fields are
  excluded from learning and from any persistence layer.
- Do not claim OS-enforced guarantees that do not exist. A normal (non-packaged,
  non-containerised) Windows TSF DLL loaded into an arbitrary host process **cannot**
  be sandboxed away from the network by the OS; the guarantee there is architectural
  (our code and dependencies make no socket calls), verified by review and build
  policy. On Android the same guarantee *is* platform-enforced by omitting
  `android.permission.INTERNET`. Phrase these two cases separately in every document.
- Never weaken, delay-batch, or "encrypt then exfiltrate" the above to ship a feature.

## 3. Honesty of results

- Report what you actually ran. Never present an unexecuted step as passing, and never
  paraphrase an exit code you did not observe.
- If a tool is missing, say which one, show the probe command, and mark the phase as
  partially verified rather than green.
- A test that cannot fail is not a test. Do not add scripts that unconditionally exit 0.
- Do not stub an input method with fixed candidate lists or empty classes and call it
  an implementation.

## 4. Phase discipline

- Work only within the assigned phase. The current phase, its scope and its deliverables
  are defined by the **task brief you have been given** and by the phase report in
  `docs/reports/` — not by a phase number hardcoded in this file. This section deliberately
  carries no phase-specific prohibitions because they rot: history shows a stale
  "do not download librime" rule here survived two phases that did exactly that, and every
  phase report then had to open by declaring a conflict with it.
- Permanent, phase-independent constraints: the language priority of §1, the privacy and
  offline rules of §2, honesty in §3, unit discipline in §5, verify-against-real-sources in
  §6, byte-clean third parties in §7, undecided licensing in §8, git safety in §9, build
  hygiene in §10. These bind in every phase.
- Anything beyond the assigned phase goes into "open questions" in the phase report, not
  into code. If a task brief asks for work that a permanent rule forbids, the brief wins
  and you flag the conflict — but only permanent rules are in this file, so that should now
  mean rules like §2 or §7, never a phase-number rule.

## 5. Precise units and offsets

- Every offset, length and index in code, docs, ABIs and tests must state its unit:
  **UTF-8 code unit (byte)**, **UTF-16 code unit**, **grapheme cluster**, or
  **caret/screen-cell column**. Mixed units are the classic source of preedit corruption.
- Cursor/anchor arithmetic must be defined at the C ABI boundary before any platform
  adapter is written; Windows TSF speaks UTF-16 and librime speaks UTF-8.
- Never write "position" or "length" without a unit.

## 6. Verify against real sources, not memory

- Before asserting how anything behaves (librime, TSF, Android IME APIs, a shipped
  dependency), open the actual source/headers on disk or the pinned upstream revision
  and cite the file and line in your reasoning.
- Assumptions drawn from blog posts, model memory, or a previous project's layout are
  hypotheses until checked. State which you verified.

## 7. Third-party code

- Never edit a file under `third_party/` in place. Upstream checkouts stay byte-clean.
- All modifications live in `third_party/patches/*.patch`, applied reproducibly by the
  build, with a note in `docs/build/bootstrap.md` explaining why each patch exists.
- Every dependency needs: pinned commit/tag, verified hash, licence file, and an entry
  in the third-party manifest. See `third_party/README.md`.
- No FetchContent / ExternalProject network fetches in this repository.

## 8. Licensing

- The EpoCanva licence is **undecided**. Do not add licence headers, LICENSE files,
  SPDX identifiers, or contributor licence text on your own initiative.

## 9. Git and workspace safety

- Never `git commit`, `git push`, create tags, or open PRs unless explicitly asked.
- Never run destructive git operations (`reset --hard`, `clean -fd`, `checkout --`,
  branch deletion) in a shared or unknown worktree without explicit approval.
- Do not initialise nested repositories; check `git rev-parse --is-inside-work-tree`
  first.
- Do not modify system configuration, install packages, or elevate privileges as a
  shortcut for a missing tool. Report the blocker with the exact command needed.

## 10. Build hygiene

- All artefacts stay under `build/<preset>/`. Never write to the source tree, the user
  profile, or Program Files during configure/build/test.
- Builds use C++17 with extensions disabled (`CMAKE_CXX_EXTENSIONS OFF`, MSVC `/permissive-`).
- Do not hardcode machine-specific absolute compiler paths in committed files;
  machine-local facts belong in `docs/reports/environment.local.md`, which is git-ignored.

## 11. Reporting format when you finish a phase

1. Sanitized environment summary.
2. Files added/modified (list every path).
3. Key design choices and their rejected alternatives.
4. Exact commands run, with real exit codes.
5. Test results — or the precise reason nothing was executed.
6. Unfinished items and risks.
7. Blockers that must be resolved before the next phase.
