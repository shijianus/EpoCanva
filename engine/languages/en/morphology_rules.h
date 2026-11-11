// Project-authored, bounded English orthographic rules (rule set en-morph-v1).
// No POS tagger, syllable/stress model, Unicode folding or complete inflection
// dictionary is claimed. Input is a canonical ASCII-letter lemma, <=64 UTF-8
// bytes. A caller with POS metadata can select forms with the bit mask below.
// The LanguageSupport adapter has no POS metadata in EnglishLexicon: regular
// rows are possible stems, NOT assertions that every generated form is a word.
//
// Provenance/scope: the small irregular relationships and regular rules were
// authored for this project, not extracted from AGID or a third-party inflector.
// Spellings of the irregular noun/verb examples were checked offline against
// the already-pinned public-domain Alt12Dicts 2of12full.txt at ESDB commit
// 7bbe293425d48a1210bf17aaf156b28ade5c2de1. That list attests SPELLING only, not
// grammatical relationships or frequency. See the exact audit references and
// limitations in docs/testing/english-completeness.md. No AGID data is used.
//
// Unmarked regular rules: s/es/consonant-y plurals/3sg; e/consonant-y past;
// silent-e/ie progressive; simple one-vowel-group CVC doubling, plus an explicit
// final-stress allowlist; final -ic adds k. No general f->ves/o->oes, no inferred
// stress, no general British consonant doubling. Known irregular noun rows use
// noun forms only; known irregular verb rows use verb forms only. Possessives
// are ASCII apostrophes (singular +'s, plural ending s +', otherwise +'s).
#ifndef EPOCANVA_EN_MORPHOLOGY_RULES_H_
#define EPOCANVA_EN_MORPHOLOGY_RULES_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace en {

enum MorphologyForm : std::uint32_t {
  kPlural = 1u << 0,
  kThirdPersonSingular = 1u << 1,
  kPast = 1u << 2,
  kPastParticiple = 1u << 3,
  kProgressive = 1u << 4,
  kSingularPossessive = 1u << 5,
  kPluralPossessive = 1u << 6,
  kIrregularPresent = 1u << 7,
  kAllMorphologyForms = (1u << 8) - 1u,
};
struct GeneratedWordForm {
  std::string canonical;
  std::uint32_t forms = 0;  // merged if, for example, plural == third-person -s
};

// Pure generation; no dictionary access. A bad/non-ASCII/delimited lemma returns
// no forms. Function words, auxiliaries without supported forms and standalone
// known irregular non-lemmas are conservatively excluded. Results are unique,
// sorted by canonical UTF-8 bytes and limited to 64 UTF-8 bytes per form.
// Unchanged invariant forms (sheep/read/run) can be returned with their masks;
// the candidate layer removes text identical to the raw input.
std::vector<GeneratedWordForm> GenerateWordForms(
    std::string_view canonical_lemma,
    std::uint32_t requested_forms = kAllMorphologyForms);

const char* MorphologyRuleSetVersion();

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_MORPHOLOGY_RULES_H_
