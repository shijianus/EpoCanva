# 2026-11-22 acceptance round report

## Verdict

**Technical/engineering: GO (local). Internal acceptance: NO-GO (11 external/scope blockers). Public beta: NO-GO. Full product scope: incomplete (unchanged).**

All engineering defects assigned to this round are implemented, reviewed, committed and verified at the frozen candidate. Every piece of acceptance evidence that can legitimately be produced without owner approval, an authorized isolated host or runner authorization has been produced and bound. The remaining release blockers are external by construction; none was waived, none was downgraded.

## 1. Identity

- Start: branch `feat/internal-acceptance-round` at `c567eb045a72c2b6787edbd3249c7ab7c220e6bc` (same as origin), `main` stale as expected.
- End: same branch at **`25ca53dc7574fbaf93010af8a3b5c8d165416e68`**, working tree clean (`git status --short` empty).
- 9 commits this round: `8583d76` (main defect batch), `cca98ce`, `0015b4c`, `03e5eb3`, `da276fe`, `08fa020`, `8ab6fbe`, `f1920e7`, `25ca53d`. 64 files changed, +4219/−259.
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
| Product formal receipt (`build_product.ps1`, Release, trace OFF) | **259/259, skipped 0, exit 0** at `25ca53d`; engine rebuilt first (exit 0, path hygiene ok) |
| Strict stage | 66 files, 172,137,518 bytes, `path-exception: none`, exit 0 |
| MSI (with exact extraction set + extracted `--diagnose`) | 25,145,344 bytes, SHA-256 `269a31b5…129a7f`, exit 0 |
| EXE bootstrapper | 25,294,896 bytes, SHA-256 `c22477d6…b42659`, embeds verified MSI, exit 0 |
| Corresponding Source | 17,906 files, 63,142,813 bytes, SHA-256 `3ab1877b…af4573e3`, exit 0 |
| package-diagnose entry | bound, gate now PASSES |
| record_installer | MSI bound to candidate, gate PASSES |
| Offline privacy producer | 4/4 cases, exit 0, entry bound |
| stress-reference-model producer | 500,000 ops / five seeds / zero mismatches, mutant negative control, entry bound, gate now PASSES |
| checker (internal-unsigned) | exit 1 — **11 blockers**, all external/scope |
| checker (public-signed) | exit 1 — 18 blockers |
| Python suites | evaluator 70 OK; capability matrix 22 OK; staging 40 OK (2 declared component skips) |
| Targeted defect tests | candidate UI 19/19 paging checks + full suite exit 0; key admission incl. 28 menu-key checks exit 0; sink candidate case incl. modified-key regression exit 0; unified TSF incl. protected-run regressions exit 0 |

## 4. Remaining blockers (all external or scope)

| Blocker | Required action |
| --- | --- |
| public-package / acceptance-profile | owner approves the Windows beta scope (`docs/product/windows-public-beta-scope.md` is `proposed_not_approved`) and a public-candidate restage |
| candidate-quality | approved Tier B corpus + absolute thresholds |
| project-license / lexicon-redistribution | attributable owner/legal records |
| installer-lifecycle | authorized isolated host: install/upgrade/repair/uninstall/rollback/cleanup/multi-user |
| real-host, english-capabilities, offline-privacy aggregate, language-acceptance | same authorized hosts: Notepad/Chromium/Word, sensitive fields, five languages |
| host-soak | ≥1800 s measured interactive soak on an authorized desktop |
| acceptance-ci-provenance | six-layer CI at this exact SHA + protected runner authorization |
| signing (public only) | final signing/timestamp evidence |

## 5. Handover

- Evidence for this candidate lives in `build/windows-product/` (`product-build-receipt.json`, `stage/EpoCanva-0.2.6-w3b`, `installer/`, `source-manifest.json`, `release-evidence.json`, `release-readiness-*.json`) and `build/acceptance-20261122-backend/`.
- Old candidates' evidence is not reusable: every prior receipt/MSI/archive was bound to older (dirty or pre-batch) SHAs.
- Next session: push approval + remote CI at `25ca53d`; owner scope/approval records; authorized host sessions for lifecycle/real-host/soak; Tier B + thresholds. If any source file changes, the whole hash-bound chain must be rebuilt (the six producer-fix loops this round demonstrate why identity is strict).
