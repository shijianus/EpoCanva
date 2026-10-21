// EpoCanva P2.4-recovery -- the pilot trial: the same compiled-in component, driven by a
// REAL word list at data scale.
//
// WHAT MAKES THIS A TEST AND NOT A DEMO. Every expected candidate list comes from
// pilot_reference.{h,cc}: an independent full-scan implementation that never calls the
// product's query core. The engine's own menu is then compared against that answer, item by
// item, on data with 42,356 rows where ties, truncation at the budget and delimiter-bearing
// words all actually occur.
//
// DATA IDENTITY IS CHECKED, NOT ASSUMED. Before the engine reads anything, this file verifies
// the file it was handed: it exists, its byte count equals the manifest's, its SHA-256
// equals the manifest's, and every row is inside the documented grammar with strictly
// ascending unique words. That is the point of the digest: a truncated list has fewer rows
// and a smaller size, still parses cleanly, and the engine would load it and report "ok" --
// only the digest separates "the pinned subset" from "some prefix of it". The data is
// alt12dicts/2of12full.txt at one pinned commit of the English Speller Database repository:
// a single-file Alt12Dicts subset, NOT an ESDB/SCOWL 35/50/60 size-variant export, and its
// weights are a coarse attestation tier across compiled dictionaries (2..12 here), NOT
// corpus frequencies.
//
// THREE REFUSAL KINDS ARE KEPT APART, because "the pilot did not pass" would otherwise be
// one blob:
//   exit 22  pre-validation rejected the data  (before the engine touched any file: the
//              modes assert lexicon_load_attempts is still 0 when they return)
//   exit 14  pre-validation passed, the engine loaded the list, and an expectation disagreed
//   exit 20  the loaded engine has no epocanva_english module at all, which is a different
//              question from whether its word list is good
// A crash, a bad argument or a missing DLL cannot stand in for any of them: CTest compares
// the exact code and the marker policy.
//
// WHAT IS NOT HERE: no A/B performance trial (that mode was never completed before the loss
// and stays out of scope), no index rewrite, no timing claim of product quality -- this file
// prints a descriptive per-key distribution and judges nothing on it.
//
// UNITS (AGENTS.md section 5): candidate indices are zero-based menu positions; preedit and
// composition lengths are UTF-8 code-unit (byte) offsets; every counter is an event count;
// data offsets quoted in identity messages are bytes from the start of the file.
#ifndef EPOCANVA_RECOVERY_PILOT_TESTS_H_
#define EPOCANVA_RECOVERY_PILOT_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// The positive trial: identity-verified data, real engine, reference-compared menus,
// commits, spaces, case shapes, delimiters, protected text, two sessions and one load.
ExitCode RunPilotLexiconMode(Harness& h);

// Pre-validation must refuse a truncated copy of the pilot list even though that copy
// parses cleanly, and must do it before the engine reads anything. Exits 22 by design.
ExitCode RunPilotTruncatedRejectedMode(Harness& h);

// Pre-validation must refuse an absent file with its own named reason (not a crash, not a
// silent skip). Exits 22 by design.
ExitCode RunPilotMissingRejectedMode(Harness& h);

// The group's falsification control: identity verification passes, the engine loads the real
// list, the healthy menus are asserted and pass -- and one expectation is deliberately wrong
// about the ORDER of two rows that the data really distinguishes. Must exit 14 without the
// success marker, which is what proves the comparison has teeth on real data rather than
// merely running green because everything was relaxed.
ExitCode RunPilotAssertionSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_PILOT_TESTS_H_
