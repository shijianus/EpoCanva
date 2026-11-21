# Windows Public Beta Scope Proposal

**Status:** `proposed_not_approved`  
**Scope ID:** `epocanva-windows-beta-v1`  
**Candidate branch at proposal:** `feat/internal-acceptance-round`  
**Candidate commit at proposal:** `c567eb045a72c2b6787edbd3249c7ab7c220e6bc`

This proposal makes no distribution or owner approval. The project currently authorizes internal unsigned acceptance only. Public beta distribution requires an artifact-bound scope decision, distribution approvals, a signed final installer, and every release-gate item in `docs/product/release-gate.md`.

## Proposed Target

| Dimension | Proposed value |
| --- | --- |
| OS | Windows 11 x64; minimum build is not approved and must be measured/decided before release |
| Architecture | x64 only |
| Install | Per-machine MSI delivered through a real offline EXE bootstrapper; immutable runtime under Program Files, user settings/data under LocalAppData |
| Hosts | Notepad, Chromium, Microsoft Word; every host requires installed-candidate evidence |
| Runtime | Ordinary in-process TSF DLL; offline behavior is architectural and dependency-reviewed, not an OS network sandbox |
| Language priority | The canonical order remains `LanguagePriority()` in `engine/locale/language_tag.cc`; this document does not duplicate that ranking |
| Current build support evidence | Internal backend/resource sets declare `en`, `zh`, `fr`, `ja`, and `ko`; each scheme still requires candidate-bound host and quality evidence before public enablement |

## Interaction and Learning Contract

The existing Windows product owns ordinary host text and offers suggestions; an explicit, currently visible selection may replace the current token. This is not a commitment to complete inline Japanese, Korean, or Chinese composition. Candidate paging, protected technical text, and modifier-key pass-through must be verified before any such workflow is admitted.

Ordinary typing must remain with the host even when the engine is unavailable or privacy permission is absent/unknown. Context enhancement and local personalization are independently permission-gated. Persistent learning writes remain disabled until revocation, queue/worker, clear, and multi-process lifecycle evidence passes; no scope proposal enables them.

## Quality, Performance, and Recovery

Use the current candidate-v1 policy and its exact sample minimums; the current policy needs en/zh/fr/ja 1,001 cases each and ko 500. Tier B provenance/licensing and absolute Top-1/Top-3/MRR and latency thresholds are not approved, so a quality GO cannot be inferred from Tier A or synthetic fixtures. Approved absolute performance limits, hardware, installed-host sampling, and soak criteria remain required decisions/evidence.

Recovery must cover safe retry, uninstall, downgrade/rollback, and explicit preservation or deletion of LocalAppData data. The installer may not place writable settings or compiled engine state inside the integrity-checked application directory. Data clearing must be an explicit user action; uninstall policy must be documented and separately tested.

## Known Limitations and Permanent Product Scope

A Windows-only scope, if approved, is only a product phase. It does not remove Android IME/APK, Spanish/German, Chinese regional identities and all required schemes, full CJK editing, or other languages in `docs/product/requirements.md` from permanent scope. The language list and priorities remain defined by the canonical locale implementation, not repeated here.

At proposal time, Android has no IME, Spanish/German have no backend binding, Chinese traditional/regional schemes are not verified, installed-host/lifecycle/soak evidence is absent for this candidate, public data redistribution decisions are absent, and no final signing record exists. These are open blockers, not waivers.

## Decision Required

An attributable owner must either approve this exact Windows scope and its explicitly listed enabled profile set for a named candidate/artifact, or reject it and retain full-product public-beta scope. Until then, `distribution_authorized=false`, public acceptance remains blocked, and this proposal must not be described as an approved beta plan.
