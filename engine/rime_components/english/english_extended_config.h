// Private Rime English configuration shared with the actual-engine test harness.
// These constants add no C ABI. Every feature switch defaults to false; the
// existing enable_suggestions switch still controls only legacy one-edit spelling.
#ifndef EPOCANVA_ENGLISH_EXTENDED_CONFIG_H_
#define EPOCANVA_ENGLISH_EXTENDED_CONFIG_H_

#include <cstddef>

namespace epocanva {
namespace english {
namespace extended {

inline constexpr char kEnableMorphology[] = "enable_morphology";
// No POS metadata is supplied. Default true admits dictionary-attested forms or
// core-marked possessives with attested bases only; false deliberately opts into
// unverified generated-rule forms.
inline constexpr char kRequireAttestedMorphology[] = "require_attested_morphology";
inline constexpr char kEnableAbbreviation[] = "enable_abbreviation";
inline constexpr char kEnableTwoEditSpelling[] = "enable_two_edit_spelling";
inline constexpr char kEnableSegmentation[] = "enable_segmentation";
inline constexpr char kFrequencyPath[] = "frequency_path";
inline constexpr char kFrequencySource[] = "frequency_source";
inline constexpr char kFrequencyProvenance[] = "frequency_provenance";
inline constexpr char kTestFixtureSource[] = "test_fixture";
inline constexpr char kProductionCorpusSource[] = "production_corpus";

// Independent frequency file, never the lexicon's attestation/Alt12 weights:
//   epocanva.en.frequency/v1 TAB <frequency_source> LF
//   provenance TAB <frequency_provenance> LF
//   <positive decimal stable lexeme ID> TAB <positive uint64 count> LF
// Source and provenance must match the schema exactly. LF/CRLF accepted; one
// final newline optional. No BOM, comments, blank lines, signs, whitespace around
// fields, leading zeroes or numeric overflow. Core Create additionally validates
// exact known IDs, duplicates and aggregate count overflow, atomically.
// A production_corpus declaration is NOT independent provenance/licence clearance.
inline constexpr char kFrequencyFormat[] = "epocanva.en.frequency/v1";
inline constexpr std::size_t kMaxFrequencyFileUtf8Bytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxProvenanceUtf8Bytes = 256;
inline constexpr std::size_t kMenuCandidateLimit = 8;  // candidates including raw

// Schema-local observability through existing RimeApi::get_property. Values are
// fixed configuration/load-status literals only, set on the first query; never
// input, paths, IDs, counts, provenance text or learned history. These private
// properties let tests distinguish an unavailable feature from a healthy no-match
// without expanding the legacy diagnostics ABI or its counter meanings.
inline constexpr char kSupportStatusProperty[] =
    "_epocanva_english_extended_status";
inline constexpr char kFrequencySourceProperty[] =
    "_epocanva_english_frequency_source";
inline constexpr char kSegmentationStatusProperty[] =
    "_epocanva_english_segmentation_status";

// The public Rime candidate view carries comments rather than candidate types.
// These labels are constants, not descriptions constructed from user input.
inline constexpr char kMorphologyComment[] = "morphology";
inline constexpr char kAbbreviationComment[] = "abbreviation";
inline constexpr char kSegmentationComment[] = "segmentation";
inline constexpr char kSpellingComment[] = "spell2";  // at most two OSA edits
inline constexpr char kContractionComment[] = "contraction";

}  // namespace extended
}  // namespace english
}  // namespace epocanva

#endif  // EPOCANVA_ENGLISH_EXTENDED_CONFIG_H_
