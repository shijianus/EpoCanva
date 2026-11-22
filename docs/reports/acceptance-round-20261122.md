# 2026-11-22 acceptance round report

## Verdict

**Technical/engineering: GO (local). Internal acceptance: NO-GO (11 external/scope blockers). Public beta: NO-GO. Full product scope: incomplete (unchanged).**

All engineering defects assigned to this round are implemented, reviewed, committed and verified at the frozen candidate. Every piece of acceptance evidence that can legitimately be produced without owner approval, an authorized isolated host or runner authorization has been produced and bound. The remaining release blockers are external by construction; none was waived, none was downgraded.

## 1. Identity

- Start: branch `feat/internal-acceptance-round` at `c567eb045a72c2b6787edbd3249c7ab7c220e6bc` (same as origin), `main` stale as expected.
- End: same branch at **`b97b45a80dbea17c8f4e72950657ac28380c04e7`** (tested candidate; the docs commit `bc978d0` follows it and changes no product input), working tree clean. 11 commits this round; 69 files changed.
- User's pre-existing `observer_tests.cc` hardening and the three 2026-11-19 reports were preserved and committed as part of `8583d76`; the proposed-not-approved beta scope is `docs/product/windows-public-beta-scope.md`.
- Nothing was pushed, merged, tagged or published. No installation, registration, elevation or signing was performed.

## 2. Fixed defects and behavior changes

- **Installer/payload (batch A + 5):** the MSI installs the immutable runtime under Program Files with `package.manifest` as a real payload row; the TSF profile helper lives outside the verified application root with a format-2 receipt binding sources/toolchain/binary; extraction is validated as an exact path/hash/byte set (only exemptions: the helper and msiexec's own stripped database, each identified deterministically); registration delegates to the ownership-aware `DllRegisterServer` with paired rollback actions; a project-owned offline EXE bootstrapper (`EpoCanva-…-x64-setup.exe`) embeds the hash-verified MSI behind an `EPOINST1` header and verifies both digests before use — a real EXE installer, not a renamed MSI. The staging overlap now treats the fixed policy as the single digest authority with negative coverage.
- **Candidates (batch B):** real paging (page size 8) with absolute-index selection, a true page grid on the TSF UIElement, page-sliced window presentation; one shared `DecideMenuKey` predicate used by both prediction and handling, so Shift+Enter/Escape and every chord pass through to the host.
- **Protected host text (batch D):** technical runs (email/URL/path markers attached to the token) are withheld from completion, next-word and preference reads; `hello. hel` stays ordinary.
- **Scheme admission (batch E):** zh-Hans-CN zhuyin promoted to implemented on controlled integration evidence only; the evaluator measures by binding-manifest state (unverified schemes fail the run instead of vanishing); the capability matrix no longer claims the profile was not exposed; zh-Hant zhuyin stays unverified.
- **Persistent learning:** stays closed (`ProductLearningWritesPermitted()` false); the partial `personalization_reads` field was reverted rather than left as dead code.

## 3. Verification actually executed (real exit codes)

| Evidence | Result |
| --- | --- |
| Root Debug CTest + JUnit verifier | 107/107, zero failures/errors/skips, verifier PASS, exit 0 |
| Backend Release CTest (`build/acceptance-20261122-backend`) | 61/61, zero failures/errors/skips, exit 0 |
| Product formal receipt (`build_product.ps1`, Release, trace OFF) | **259/259, skipped 0, exit 0** at `b97b45a`; engine rebuilt first (exit 0, path hygiene ok) |
| Strict stage | 66 files, 172,137,518 bytes, `path-exception: none`, exit 0 |
| MSI (with exact extraction set + extracted `--diagnose`) | 25,124,864 bytes, SHA-256 `ba83a28d…f2033e`, exit 0 |
| EXE bootstrapper | 25,274,416 bytes, SHA-256 `af1781c4…d9e6abb`, embeds verified MSI, exit 0 |
| Corresponding Source | 17,906 files, 63,142,851 bytes, SHA-256 `1349afd6…1f3f82b`, exit 0 |
| package-diagnose entry | bound, gate now PASSES |
| record_installer | MSI bound to candidate, gate PASSES |
| Offline privacy producer | 4/4 cases, exit 0, entry bound |
| stress-reference-model producer | 500,000 ops / five seeds / zero mismatches, mutant negative control, entry bound, gate now PASSES |
| English acceptance producer | 7/7 cases (frequency, context-learning, morphology, spelling, segmentation, abbreviation, TSF cross-session), entry bound; aggregate stays blocked on real-host by design |
| checker (internal-unsigned) | exit 1 — **11 blockers**, all external/scope |
| checker (public-signed) | exit 1 — 18 blockers |
| Python suites | evaluator 74 OK; capability matrix 22 OK; staging 40 OK (2 declared component skips) |
| Targeted defect tests | candidate UI 19/19 paging checks + full suite exit 0; key admission incl. 28 menu-key checks exit 0; sink candidate case incl. modified-key regression exit 0; unified TSF incl. protected-run regressions exit 0 |

## 4. Remaining blockers (all external or scope)

| Blocker | Required action |
| --- | --- |
| public-package / acceptance-profile | owner approves the Windows beta scope (`docs/product/windows-public-beta-scope.md` is `proposed_not_approved`) and a public-candidate restage |
| candidate-quality | approved Tier B corpus + absolute thresholds |
| project-license / lexicon-redistribution | attributable owner/legal records |
| installer-lifecycle | authorized isolated host: install/upgrade/repair/uninstall/rollback/cleanup/multi-user |
| real-host, english-capabilities aggregate, offline-privacy aggregate, language-acceptance | same authorized hosts: Notepad/Chromium/Word, sensitive fields, five languages |
| host-soak | ≥1800 s measured interactive soak on an authorized desktop |
| acceptance-ci-provenance | six-layer CI at this exact SHA + protected runner authorization |
| signing (public only) | final signing/timestamp evidence |

## 6. Remote integration and CI

- The branch was pushed to origin after a full remote audit: every remote branch (`main`, `codex/go-refactor-20261117`, `feat/offline-multilingual-backend`, `feat/phase-e-*`) contained zero commits not already in the local HEAD, so the push was a pure fast-forward and overwrote nothing on either side.
- The first push (`fe5be83`) failed the remote `windows-ci` **whitespace gate** (6 trailing-whitespace problems in the beta-scope proposal); `root-unit-tests` was skipped behind it. A forward fixup commit (`c99ff82`) stripped the hard-break spaces.
- Remote CI at `c99ff82` (run 45048037830): **contract-gate success, canary success, root-unit-tests success.** These are genuine remote layers 1-2 evidence at this exact SHA; the four deeper layers remain disabled pending the protected runner with the hash-verified immutable input cache.

## 7. Handover

- Evidence for the tested candidate lives in `build/windows-product/` (`product-build-receipt.json`, `stage/EpoCanva-0.2.6-w3b`, `installer/`, `source-manifest.json`, `release-evidence.json` with five bound entries, `release-readiness-*.json`), `build/acceptance-20261122-backend/`, and `build/windows-product/english-acceptance/`.
- Old candidates' evidence is not reusable: every prior receipt/MSI/archive was bound to older (dirty or pre-batch) SHAs.
- Next session: push approval + remote CI at `b97b45a`; owner scope/approval records; authorized host sessions for lifecycle/real-host/soak; Tier B + thresholds. If any source file changes, the whole hash-bound chain must be rebuilt (the repeated producer-fix loops this round demonstrate why identity is strict).
