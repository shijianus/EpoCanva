# Requirement and remaining product gap matrix — 2026-11-25

Requirements remain those in `docs/product/requirements.md`; this audit does not
reduce scope. Language resource priority is read from `LanguagePriority()`.

| Requirement | Actual evidence / implementation | Unfinished acceptance or implementation |
| --- | --- | --- |
| English frequency/context, morphology, spelling, segmentation, abbreviations | Real algorithms and native/controlled-TSF suites retained; no ASCII substitute. | Installed cross-app consistency and persistent authorization lifecycle. |
| Chinese regional and multi-scheme identities, inline behavior | Existing verified pinyin/zhuyin/core and product paths retained. | Remaining required regions/schemes and complete inline behavior; conversion alone is not regional support. |
| French real dictionary | Pinned Hunspell/dictionary, measured cache optimization with exact candidate parity. | Approved corpus/performance thresholds, first-query cost and contention. |
| Japanese composition/partial confirmation and Korean | Existing real decoders/composition/controlled editing retained. | Complete partial-confirm/edit semantics and real-host five-language acceptance. |
| Spanish, German and other permanent languages | Identity registry does not claim unavailable engines. | Real engine/data/UI/test coverage; empty profiles forbidden. |
| Full editor semantics | Independent scalar model and real controlled TSF store; UTF-16 boundaries, undo, invalid states and stale generations. | Noncollapsed selection replacement and installed-host editing/undo. |
| Learning and privacy | Default/Unknown/sensitive persistence disabled; no network feature added. | Explicit grant/revoke transaction from UI through queue/worker/write/clear/restart/multi-user storage. |
| Candidate quality | Tier A has 15 development samples; strict producer/consumer and harness binding implemented. | Real approved Tier B: en/zh/fr/ja at least 1001 each, ko at least 500, total 4504; actual absolute metrics approval and qualification. |
| Reference-model stress | Five fixed seeds ×100000 edits, independent oracle, replay/minimization and resources implemented/measured. | Current candidate formal execution and actual approved resource policy; 500000 edits are not installed-host proof. |
| Windows EXE distribution | Real x64 bootstrapper/MSI and static content/provenance checks. | Authorized snapshot-backed install/reinstall/upgrade/repair/uninstall/rollback, registration and multi-user isolation. |
| Hosts and soak | Controlled tests do not install or register input method. | Notepad/Chromium/Word and other specified hosts, five languages, actual keyboard/candidates, recovery, sensitive fields, at least1800 seconds measured active interaction. |
| Six-layer CI | All layers implemented with runner/cache/plan provenance and negative verification. | Actual same-SHA six-layer success; zero authorized self-hosted runners available. |
| Source/license/supply chain | AGPL-3.0-only retained, source/package and dependency manifests checked; strict source archive producer. | Final rebuilt corresponding source, third-party corpus redistribution decisions, signed public artifacts and timestamps. |
| Android APK | No delivered Android implementation or device evidence. Existing adb/java do not prove build capability; Gradle probe missing. | InputMethodService, device lifecycle/composition/candidate UI, no INTERNET permission and dependency/network verification, real APK/device tests. |
| Systematic UI i18n | Existing UI labels are not full localization evidence. | Complete translation/layout/accessibility and supported-language tests. |

Windows ordinary in-process TSF has an architectural no-network guarantee, not
OS process network isolation. Android can enforce network permission absence;
no Android network test was run here. Internal Alpha word-list authorization is
not public redistribution permission, and AGPL does not relicense third parties.
