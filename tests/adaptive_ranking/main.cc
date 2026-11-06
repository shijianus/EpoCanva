// Adaptive ranking and learning-event semantics tests.
//
// Each case is a separate CTest entry so a regression names the behaviour it
// broke. The --forcefail case proves the harness can report a red run; it is not
// a case that passes unconditionally.
#include "adaptive_ranker.h"
#include "learning_events.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace rk = epocanva::ranking;

int failed = 0;
int checks = 0;

void Check(bool value, const char* label) {
  ++checks;
  if (!value) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}

void CheckNear(double actual, double expected, double tolerance, const char* label) {
  ++checks;
  if (!(std::fabs(actual - expected) <= tolerance)) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << " (actual " << actual << " expected "
              << expected << ")\n";
  }
}

rk::Proposal MakeProposal(const char* text, const char* language, const char* id,
                          double static_weight, double context_logp = 0.0,
                          rk::SourceKind source = rk::SourceKind::kCompletion) {
  rk::Proposal proposal;
  proposal.text_utf8 = text;
  proposal.language = language;
  proposal.candidate_id = id;
  proposal.static_weight = static_weight;
  proposal.context_logp = context_logp;
  proposal.source = source;
  return proposal;
}

rk::LearningEvent MakeEvent(rk::EventKind kind, const char* language, const char* id,
                            std::uint64_t generation, std::uint64_t now,
                            bool attributable = true) {
  rk::LearningEvent event;
  event.kind = kind;
  event.language = language;
  event.candidate_id = id;
  event.generation = generation;
  event.now_ticks = now;
  event.attributable = attributable;
  event.privacy = rk::PrivacyClass::kNormal;
  return event;
}

const char* TextAt(const rk::RankRequest& request, const rk::RankResult& result,
                   std::size_t position) {
  return request.proposals[result.ordered[position].proposal_ordinal].text_utf8.c_str();
}

bool SameText(const rk::RankRequest& request, const rk::RankResult& result,
              std::size_t position, const char* expected) {
  if (position >= result.ordered.size()) return false;
  return request.proposals[result.ordered[position].proposal_ordinal].text_utf8 ==
         expected;
}

void PrefixTests() {
  double quality = 0.0;
  bool exact = false;

  Check(rk::AdaptiveRanker::PrefixQuality("hel", "hello", &quality, &exact),
        "valid ASCII prefix accepted");
  CheckNear(quality, 1.0, 1e-9, "full ASCII prefix has quality 1");
  Check(!exact, "prefix is not the whole candidate");

  Check(rk::AdaptiveRanker::PrefixQuality("Hello", "hello", &quality, &exact),
        "case-insensitive prefix accepted");
  Check(exact && quality == 1.0, "ASCII case-insensitive exact match");

  // UTF-8 prefix quality counts scalars, not bytes. A 3-byte key character must
  // count as one unit; counting bytes would halve the measured quality.
  Check(rk::AdaptiveRanker::PrefixQuality(u8"你", u8"你好", &quality, &exact),
        "CJK prefix accepted");
  CheckNear(quality, 1.0, 1e-9, "one-shared-of-one CJK scalar is a full prefix");
  Check(!exact, "CJK prefix is not exact");

  Check(rk::AdaptiveRanker::PrefixQuality(u8"你好", u8"你", &quality, &exact),
        "shorter candidate accepted");
  CheckNear(quality, 0.5, 1e-9, "one of two CJK scalars is half a prefix");

  Check(rk::AdaptiveRanker::PrefixQuality(u8"hé", u8"hétéro", &quality, &exact),
        "accented prefix accepted");
  CheckNear(quality, 1.0, 1e-9, "accented scalars compare exactly");

  Check(rk::AdaptiveRanker::PrefixQuality(u8"e", u8"é", &quality, &exact),
        "precomposed acute is not ASCII e");
  CheckNear(quality, 0.0, 1e-9, "é and e are distinct scalars");

  Check(rk::AdaptiveRanker::PrefixQuality("nihao", u8"你好", &quality, &exact),
        "romanised key against CJK output accepted");
  CheckNear(quality, 0.0, 1e-9, "romanised key shares no scalar with CJK output");

  // Malformed UTF-8 must be refused, not repaired.
  const std::string lone_continuation("\x80", 1);
  Check(!rk::AdaptiveRanker::PrefixQuality(lone_continuation, "hello", &quality, &exact),
        "lone continuation byte rejected");
  const std::string truncated("\xE4\xBD", 2);
  Check(!rk::AdaptiveRanker::PrefixQuality("a", truncated, &quality, &exact),
        "truncated sequence rejected");
  const std::string surrogate("\xED\xA0\x80", 3);
  Check(!rk::AdaptiveRanker::PrefixQuality("a", surrogate, &quality, &exact),
        "UTF-8 encoded surrogate rejected");
  Check(!rk::AdaptiveRanker::PrefixQuality("a", "b", nullptr, &exact),
        "null output rejected");
}

void OrderingTests() {
  rk::AdaptiveRanker ranker;
  rk::RankRequest request;
  request.user_language = "en";
  request.input_key_utf8 = "hello";
  request.proposals = {
      MakeProposal("hello world", "en", "p_hello_world", 0.9),
      MakeProposal("hello", "en", "p_hello", 0.2),
      MakeProposal("help", "en", "p_help", 0.5),
  };

  const rk::RankResult result = ranker.Rank(request, nullptr);
  Check(result.status == rk::RankStatus::kOk, "ordering rank succeeds");
  Check(result.ordered.size() == 3, "all three proposals ranked");
  Check(SameText(request, result, 0, "hello"), "exact candidate is first");
  Check(result.ordered[0].tier == 0, "exact candidate is tier 0");
  Check(result.ordered[1].tier == 1 && result.ordered[2].tier == 1,
        "other candidates are tier 1");
  Check(result.ordered[0].score < result.ordered[1].score,
        "tier outranks score: the exact candidate wins with a lower score");
  Check(SameText(request, result, 1, "hello world"), "higher static weight second");
  Check(SameText(request, result, 2, "help"), "weaker prefix last");

  // Determinism: identical input yields identical output, including ties.
  rk::RankRequest tie_request;
  tie_request.user_language = "en";
  tie_request.proposals = {
      MakeProposal("alpha", "en", "a", 0.5),
      MakeProposal("beta", "en", "b", 0.5),
      MakeProposal("gamma", "en", "g", 0.5),
  };
  const rk::RankResult first = ranker.Rank(tie_request, nullptr);
  const rk::RankResult second = ranker.Rank(tie_request, nullptr);
  bool same = first.ordered.size() == second.ordered.size();
  for (std::size_t i = 0; same && i < first.ordered.size(); ++i) {
    same = first.ordered[i].proposal_ordinal == second.ordered[i].proposal_ordinal &&
           first.ordered[i].score == second.ordered[i].score;
  }
  Check(same, "repeated ranking is bit-identical");
  Check(SameText(tie_request, first, 0, "alpha") &&
            SameText(tie_request, first, 1, "beta") &&
            SameText(tie_request, first, 2, "gamma"),
        "equal scores keep input order");

  // Deduplication keeps one record for identical display text.
  rk::RankRequest duplicate_request;
  duplicate_request.user_language = "en";
  duplicate_request.input_key_utf8 = "coff";
  duplicate_request.proposals = {
      MakeProposal("coffee", "en", "c1", 0.2),
      MakeProposal("coffee", "en", "c2", 0.8),
      MakeProposal("coffer", "en", "c3", 0.1),
  };
  const rk::RankResult duplicate_result = ranker.Rank(duplicate_request, nullptr);
  Check(duplicate_result.deduplicated == 1, "duplicate display text merged once");
  Check(duplicate_result.ordered.size() == 2, "two distinct texts remain");
  const std::size_t survivor =
      duplicate_result.ordered[0].proposal_ordinal;
  Check(survivor == 1, "survivor is the higher-scoring duplicate");
}

void NoModelTests() {
  // The product must produce a useful order with no model installed. context 0
  // is the documented "no evidence" value and must not block ranking.
  rk::AdaptiveRanker ranker;
  rk::RankRequest request;
  request.user_language = "en";
  request.input_key_utf8 = "mor";
  request.proposals = {
      MakeProposal("morning", "en", "m1", 0.8, 0.0),
      MakeProposal("more", "en", "m2", 0.95, 0.0),
      MakeProposal("mornings", "en", "m3", 0.3, 0.0),
  };
  const rk::RankResult result = ranker.Rank(request, nullptr);
  Check(result.status == rk::RankStatus::kOk, "model-free rank succeeds");
  Check(SameText(request, result, 0, "more"), "model-free order follows static weight");
  Check(SameText(request, result, 1, "morning"), "second follows static weight");
  Check(SameText(request, result, 2, "mornings"), "third follows static weight");

  // A context floor must clamp extreme model values instead of letting one
  // pathological probability dominate every other feature.
  rk::RankRequest extreme = request;
  extreme.proposals[0].context_logp = -1000.0;  // nonsense from a broken model
  extreme.proposals[1].context_logp = -1000.0;
  extreme.proposals[2].context_logp = -1000.0;
  const rk::RankResult clamped = ranker.Rank(extreme, nullptr);
  Check(clamped.status == rk::RankStatus::kOk, "extreme context still ranks");
  Check(SameText(extreme, clamped, 0, "more") &&
            SameText(extreme, clamped, 1, "morning") &&
            SameText(extreme, clamped, 2, "mornings"),
        "clamped context preserves the relative order");
  CheckNear(clamped.ordered[0].score,
            result.ordered[0].score - 20.0 * request.weights.context, 1e-9,
            "context is clamped at the documented floor");

  // Missing learning state is not an error.
  Check(result.ordered.size() == 3, "no learning state still returns a full list");
}

void LearningTests() {
  rk::LearningEventModel learning;
  rk::AdaptiveRanker ranker;

  rk::RankRequest request;
  request.user_language = "en";
  request.generation = 11;
  request.proposals = {
      MakeProposal("coffee", "en", "c_coffee", 0.50),
      MakeProposal("tea", "en", "c_tea", 0.40),
  };
  learning.SetGeneration(11);

  const rk::RankResult before = ranker.Rank(request, &learning);
  Check(SameText(request, before, 0, "coffee"), "default order favours static weight");

  // Repeated explicit selection must be able to change the relative order. All
  // three events share one tick so the assertion measures accumulation alone;
  // decay has its own case below.
  for (int i = 0; i < 3; ++i) {
    Check(learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "c_tea", 11, 0)) ==
              rk::EventStatus::kApplied,
          "selection event applied");
  }
  const rk::RankResult after = ranker.Rank(request, &learning);
  Check(SameText(request, after, 0, "tea"),
        "repeated selection promotes the chosen candidate");

  rk::Preference preference;
  Check(learning.Lookup("en", "c_tea", &preference), "selection recorded");
  CheckNear(preference.positive, 1.8, 1e-9, "three selections accumulate 1.8 points");

  // With learning disabled the ranker ignores even stored preferences.
  rk::RankRequest learning_off = request;
  learning_off.learning_enabled = false;
  const rk::RankResult ignored = ranker.Rank(learning_off, &learning);
  Check(SameText(request, ignored, 0, "coffee"),
        "disabling learning restores non-personal order");

  // Disabling writes must not erase what the user already taught.
  learning.SetEnabled(false);
  Check(learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "c_tea", 11, 10)) ==
            rk::EventStatus::kLearningDisabled,
        "disabled model refuses new events");
  Check(learning.Lookup("en", "c_tea", &preference) && preference.positive > 0.0,
        "disabled model keeps existing preference");
  learning.SetEnabled(true);
}

void IsolationTests() {
  rk::LearningEventModel learning;
  rk::AdaptiveRanker ranker;
  learning.SetGeneration(3);

  learning.Apply(MakeEvent(rk::EventKind::kSelected, "zh", "zh_word", 3, 0));
  learning.Apply(MakeEvent(rk::EventKind::kSelected, "zh", "zh_word", 3, 0));
  learning.Apply(MakeEvent(rk::EventKind::kSelected, "zh", "zh_word", 3, 0));

  rk::Preference preference;
  Check(learning.Lookup("zh", "zh_word", &preference), "Chinese preference stored");
  Check(!learning.Lookup("en", "zh_word", &preference),
        "same id under another language is not the same record");

  rk::RankRequest english;
  english.user_language = "en";
  english.generation = 3;
  english.input_key_utf8 = "coff";
  english.proposals = {
      MakeProposal("coffee", "en", "zh_word", 0.50),
      MakeProposal("coffer", "en", "en_other", 0.45),
  };
  const rk::RankResult result = ranker.Rank(english, &learning);
  Check(SameText(english, result, 0, "coffee"),
        "Chinese learning does not boost an English candidate with the same id");
}

void NegativeTests() {
  rk::LearningEventModel learning;
  rk::AdaptiveRanker ranker;
  learning.SetGeneration(5);

  // Negative feedback is bounded: the third and fiftieth rejection at the same
  // tick must be identical, so a noisy rejection stream cannot keep pushing a
  // word down.
  for (int i = 0; i < 3; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kRejected, "en", "noisy", 5, 0));
  }
  rk::Preference three;
  Check(learning.Lookup("en", "noisy", &three), "rejections recorded");
  for (int i = 3; i < 50; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kRejected, "en", "noisy", 5, 0));
  }
  rk::Preference fifty;
  Check(learning.Lookup("en", "noisy", &fifty), "many rejections recorded");
  CheckNear(three.negative, rk::kMaxNegativePenalty, 1e-12,
            "negative saturates after three rejections");
  CheckNear(fifty.negative, three.negative, 1e-12,
            "negative value does not grow past the saturation bound");

  // Superseded is a weak signal, weaker than an explicit rejection.
  learning.Apply(MakeEvent(rk::EventKind::kSuperseded, "en", "superseded", 5, 0));
  rk::Preference superseded;
  Check(learning.Lookup("en", "superseded", &superseded), "superseded recorded");
  CheckNear(superseded.negative, rk::kSupersededGain, 1e-12,
            "superseded adds only the weak gain");
  Check(superseded.negative < rk::kRejectionGain, "superseded is weaker than rejection");

  // The user's own text cannot be displaced even by unbounded rejection.
  rk::RankRequest original;
  original.user_language = "en";
  original.generation = 5;
  original.input_key_utf8 = "hello";
  original.proposals = {
      MakeProposal("hello", "en", "h_raw", 0.0),
      MakeProposal("hello world", "en", "h_long", 1.0),
  };
  for (int i = 0; i < 100; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kRejected, "en", "h_raw", 5, 100 + i));
  }
  const rk::RankResult result = ranker.Rank(original, &learning);
  Check(SameText(original, result, 0, "hello"),
        "exact input stays first regardless of negative feedback");
}

void DecayTests() {
  rk::LearningEventModel learning;
  learning.SetGeneration(1);
  for (int i = 0; i < 3; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "decaying", 1, 0));
  }
  rk::Preference preference;
  Check(learning.Lookup("en", "decaying", &preference), "preference exists");
  const double peak = preference.positive;
  CheckNear(peak, 1.8, 1e-9, "peak before decay");

  learning.AdvanceTo(rk::kDefaultHalfLifeTicks);
  Check(learning.Lookup("en", "decaying", &preference), "preference survives decay");
  CheckNear(preference.positive, peak / 2.0, 1e-9, "one half-life halves the boost");

  learning.AdvanceTo(2 * rk::kDefaultHalfLifeTicks);
  Check(learning.Lookup("en", "decaying", &preference), "preference survives twice");
  CheckNear(preference.positive, peak / 4.0, 1e-9, "two half-lives quarter the boost");

  // Time never runs backwards inside the model.
  learning.AdvanceTo(1);
  Check(learning.Lookup("en", "decaying", &preference), "preference still present");
  CheckNear(preference.positive, peak / 4.0, 1e-9, "older tick is a no-op");
}

void RejectionTests() {
  // These are the structural guarantees: events that are not attributable to a
  // specific candidate in the current menu take no effect at all.
  rk::LearningEventModel learning;
  learning.SetGeneration(9);
  rk::Preference preference;

  Check(learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "x", 8, 0)) ==
            rk::EventStatus::kIgnoredStale,
        "stale generation ignored");
  Check(!learning.Lookup("en", "x", &preference), "stale event recorded nothing");

  Check(learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "x", 9, 0, false)) ==
            rk::EventStatus::kIgnoredUnattributable,
        "unattributable event ignored");
  Check(!learning.Lookup("en", "x", &preference), "unattributable event recorded nothing");

  rk::LearningEvent sensitive =
      MakeEvent(rk::EventKind::kSelected, "en", "x", 9, 0);
  sensitive.privacy = rk::PrivacyClass::kSensitive;
  Check(learning.Apply(sensitive) == rk::EventStatus::kDeniedSensitive,
        "sensitive field denied");

  rk::LearningEvent unknown = MakeEvent(rk::EventKind::kSelected, "en", "x", 9, 0);
  unknown.privacy = rk::PrivacyClass::kUnknown;
  Check(learning.Apply(unknown) == rk::EventStatus::kDeniedUnknownPrivacy,
        "unclassified field denied");
  Check(!learning.Lookup("en", "x", &preference), "denied events recorded nothing");

  rk::LearningEvent bad_language = MakeEvent(rk::EventKind::kSelected, "EN", "x", 9, 0);
  Check(learning.Apply(bad_language) == rk::EventStatus::kInvalidArgument,
        "non-canonical language rejected");

  rk::LearningEvent empty_id = MakeEvent(rk::EventKind::kSelected, "en", "", 9, 0);
  Check(learning.Apply(empty_id) == rk::EventStatus::kInvalidArgument,
        "empty candidate id rejected");

  Check(learning.Apply(MakeEvent(rk::EventKind::kManualWordRemoved, "en", "missing", 9,
                                 0)) == rk::EventStatus::kNotTracked,
        "removing an untracked manual word reports not tracked");

  // A manual word is a direct user instruction and needs no menu attribution.
  Check(learning.Apply(MakeEvent(rk::EventKind::kManualWordAdded, "en", "mine", 9, 0,
                                 false)) == rk::EventStatus::kApplied,
        "manual word accepted without menu attribution");
  Check(learning.Lookup("en", "mine", &preference) && preference.manual,
        "manual word flagged as user-owned");
  CheckNear(preference.positive, rk::kMaxPersonalBoost, 1e-12,
            "manual word starts at the saturation bound");
  Check(learning.Apply(MakeEvent(rk::EventKind::kManualWordRemoved, "en", "mine", 9,
                                 0)) == rk::EventStatus::kApplied,
        "manual word removal applied");
  Check(!learning.Lookup("en", "mine", &preference), "manual word removed");
}

void PersistenceTests() {
  // The persistence adapter installs a stored summary without inventing an event or a
  // gain. It must still clamp values and preserve user-authored entries under pressure.
  rk::LearningEventModel learning;
  rk::Preference imported;
  imported.positive = 1.8;
  imported.negative = 0.2;
  imported.last_tick = 100;
  Check(learning.SetPreference("en", "c_tea", imported), "stored preference imported");
  rk::AdaptiveRanker ranker;
  rk::RankRequest request;
  request.user_language = "en";
  request.proposals = {
      MakeProposal("coffee", "en", "c_coffee", 0.50),
      MakeProposal("tea", "en", "c_tea", 0.40),
  };
  Check(SameText(request, ranker.Rank(request, &learning), 0, "tea"),
        "imported preference changes the order without an event");
  rk::Preference clamped;
  clamped.positive = 99.0;
  clamped.negative = 99.0;
  Check(learning.SetPreference("en", "c_tea", clamped), "out-of-range import clipped");
  rk::Preference stored;
  Check(learning.Lookup("en", "c_tea", &stored) &&
        stored.positive == rk::kMaxPersonalBoost &&
        stored.negative == rk::kMaxNegativePenalty, "import uses the same bounds");
  rk::Preference bad = clamped;
  bad.positive = std::nan("");
  Check(!learning.SetPreference("en", "c_bad", bad), "non-finite import rejected");
  Check(!learning.SetPreference("EN", "c_bad", clamped), "invalid language import rejected");
  Check(!learning.SetPreference("en", "", clamped), "empty candidate import rejected");

  rk::LearningEventModel full;
  for (std::size_t i = 0; i < rk::kMaxLearnedEntries; ++i) {
    rk::Preference manual;
    manual.positive = rk::kMaxPersonalBoost;
    manual.manual = true;
    const std::string id = "m" + std::to_string(i);
    Check(full.SetPreference("en", id, manual), "manual entry fills the bound");
  }
  rk::Preference inferred;
  inferred.positive = 1.0;
  Check(!full.SetPreference("en", "new-inferred", inferred),
        "inferred import cannot evict a manual entry");
}

void ClearTests() {
  rk::LearningEventModel learning;
  rk::AdaptiveRanker ranker;
  learning.SetGeneration(2);

  rk::RankRequest request;
  request.user_language = "en";
  request.generation = 2;
  request.proposals = {
      MakeProposal("coffee", "en", "c", 0.50),
      MakeProposal("tea", "en", "t", 0.40),
  };

  for (int i = 0; i < 3; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "t", 2,
                             static_cast<std::uint64_t>(i)));
  }
  Check(SameText(request, ranker.Rank(request, &learning), 0, "tea"),
        "learning changed the order");

  learning.Clear();
  Check(learning.size() == 0, "clear removes every entry");
  Check(SameText(request, ranker.Rank(request, &learning), 0, "coffee"),
        "clear restores default ordering");
}

void MixedLanguageTests() {
  rk::AdaptiveRanker ranker(3);  // three confirmations before latching
  rk::RankRequest request;
  request.user_language = "en";
  request.input_key_utf8 = "hao";
  request.proposals = {
      MakeProposal(u8"好", "zh", "zh_hao", 0.90),
      MakeProposal("hold", "en", "en_hold", 0.30),
  };

  // Without a latched detection the foreign proposal is not offered at all.
  rk::RankResult result = ranker.Rank(request, nullptr);
  Check(result.status == rk::RankStatus::kOk, "mixed-language rank succeeds");
  Check(result.dropped_foreign_language == 1, "unrelated language dropped");
  Check(result.ordered.size() == 1, "only the selected language remains");

  // A single detection observation must not reorder anything.
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language().empty(),
        "one observation does not latch");
  result = ranker.Rank(request, nullptr);
  Check(result.dropped_foreign_language == 1, "one observation changes nothing");

  // Flapping: alternate languages and confirm the latch never moves.
  for (int i = 0; i < 20; ++i) {
    ranker.ObserveDetection(i % 2 == 0 ? "zh" : "fr");
    ranker.ObserveDetection("en");
    Check(ranker.effective_detected_language().empty(),
          "alternating detections never latch");
  }

  // Three consistent observations latch the language.
  ranker.ObserveDetection("zh");
  ranker.ObserveDetection("zh");
  ranker.ObserveDetection("zh");
  Check(ranker.effective_detected_language() == "zh", "three observations latch zh");

  result = ranker.Rank(request, nullptr);
  Check(result.dropped_foreign_language == 0, "latched language is admitted");
  Check(result.ordered.size() == 2, "both proposals ranked");
  Check(SameText(request, result, 0, "hold"),
        "explicitly selected language wins at equal strength");

  // Even a much stronger foreign candidate may not outrank the user's own
  // language when the gap is smaller than the language weight.
  rk::RankRequest close;
  close.user_language = "en";
  close.proposals = {
      MakeProposal(u8"好", "zh", "zh_hao", 0.60),
      MakeProposal("hold", "en", "en_hold", 0.55),
  };
  const rk::RankResult tied = ranker.Rank(close, nullptr);
  Check(SameText(close, tied, 0, "hold"),
        "small foreign advantage is neutralised by the language term");

  // A substantially better foreign candidate can still surface, because the
  // language term is bounded rather than an absolute veto.
  rk::RankRequest strong;
  strong.user_language = "en";
  strong.input_key_utf8 = "zzz";
  strong.proposals = {
      MakeProposal(u8"好", "zh", "zh_hao", 1.00),
      MakeProposal("hold", "en", "en_hold", 0.00),
  };
  const rk::RankResult surfaced = ranker.Rank(strong, nullptr);
  Check(SameText(strong, surfaced, 0, u8"好"),
        "much better foreign candidate still surfaces");

  ranker.ResetDetection();
  Check(ranker.effective_detected_language().empty(), "reset clears the latch");
}

void BoundsTests() {
  rk::AdaptiveRanker ranker;

  rk::RankRequest too_many;
  too_many.user_language = "en";
  for (std::size_t i = 0; i <= rk::kMaxProposals; ++i) {
    too_many.proposals.push_back(
        MakeProposal(("w" + std::to_string(i)).c_str(), "en", "id", 0.1));
  }
  Check(ranker.Rank(too_many, nullptr).status == rk::RankStatus::kTooManyProposals,
        "oversized proposal list refused instead of truncated");

  rk::RankRequest long_key;
  long_key.user_language = "en";
  long_key.input_key_utf8.assign(rk::kMaxInputKeyUtf8Bytes + 1, 'a');
  long_key.proposals = {MakeProposal("alpha", "en", "a", 0.1)};
  Check(ranker.Rank(long_key, nullptr).status == rk::RankStatus::kInputTooLong,
        "oversized input key refused");

  rk::RankRequest bad_weights;
  bad_weights.user_language = "en";
  bad_weights.weights.context = 999.0;
  bad_weights.proposals = {MakeProposal("alpha", "en", "a", 0.1)};
  Check(ranker.Rank(bad_weights, nullptr).status == rk::RankStatus::kInvalidWeights,
        "out-of-range weight refused");

  rk::RankRequest nan_weights;
  nan_weights.user_language = "en";
  nan_weights.weights.prefix = std::nan("");
  nan_weights.proposals = {MakeProposal("alpha", "en", "a", 0.1)};
  Check(ranker.Rank(nan_weights, nullptr).status == rk::RankStatus::kInvalidWeights,
        "NaN weight refused");

  rk::RankRequest bad_language;
  bad_language.user_language = "English";
  bad_language.proposals = {MakeProposal("alpha", "en", "a", 0.1)};
  Check(ranker.Rank(bad_language, nullptr).status == rk::RankStatus::kInvalidInput,
        "non-subtag language refused");

  const std::string invalid_utf8("\xC0\xAF", 2);
  rk::RankRequest bad_utf8;
  bad_utf8.user_language = "en";
  bad_utf8.proposals.push_back(MakeProposal("alpha", "en", "a", 0.1));
  bad_utf8.proposals[0].text_utf8 = invalid_utf8;
  const rk::RankResult utf8_result = ranker.Rank(bad_utf8, nullptr);
  Check(utf8_result.status == rk::RankStatus::kInvalidUtf8, "overlong UTF-8 refused");
  Check(utf8_result.ordered.empty(), "refused input produces no partial order");

  // The learning table is bounded. Filling it past the limit must not grow
  // without limit and must keep the most recent manual word.
  rk::LearningEventModel learning;
  learning.SetGeneration(1);
  for (std::size_t i = 0; i < rk::kMaxLearnedEntries + 50; ++i) {
    const std::string id = "id" + std::to_string(i);
    learning.Apply(MakeEvent(rk::EventKind::kManualWordAdded, "en", id.c_str(), 1,
                             static_cast<std::uint64_t>(i)));
  }
  Check(learning.size() <= rk::kMaxLearnedEntries, "learning table respects its bound");
  rk::Preference newest;
  const std::string newest_id =
      "id" + std::to_string(rk::kMaxLearnedEntries + 49);
  Check(learning.Lookup("en", newest_id, &newest), "newest manual word retained");
}

void SpellingTests() {
  rk::AdaptiveRanker ranker;
  rk::RankRequest request;
  request.user_language = "en";
  request.input_key_utf8 = "recieve";
  rk::Proposal exact_form = MakeProposal("recieve", "en", "a", 0.1);
  rk::Proposal correct = MakeProposal("receive", "en", "b", 0.9,
                                      0.0, rk::SourceKind::kSpelling);
  correct.edit_distance = 2;
  rk::Proposal close = MakeProposal("recieved", "en", "c", 0.9,
                                    0.0, rk::SourceKind::kSpelling);
  close.edit_distance = 1;
  request.proposals = {exact_form, correct, close};

  const rk::RankResult result = ranker.Rank(request, nullptr);
  Check(SameText(request, result, 0, "recieve"), "typed text stays first");
  Check(SameText(request, result, 1, "recieved"),
        "smaller edit distance ranks first among spelling proposals");
  Check(SameText(request, result, 2, "receive"),
        "larger edit distance ranks last");
}

// Readable evidence for a human reviewer: the same production objects, the same
// arithmetic, printed before and after explicit user selection. This is not a
// separate simulated path.
void Demo() {
  rk::LearningEventModel learning;
  rk::AdaptiveRanker ranker;
  learning.SetGeneration(1);

  rk::RankRequest request;
  request.user_language = "en";
  request.input_key_utf8 = "coff";
  request.generation = 1;
  request.proposals = {
      MakeProposal("coffee", "en", "c_coffee", 0.50),
      MakeProposal("coffer", "en", "c_coffer", 0.45),
      MakeProposal("coff", "en", "c_coff", 0.10),
  };

  auto print = [&](const char* label, const rk::RankResult& result) {
    std::cout << label << '\n';
    for (std::size_t i = 0; i < result.ordered.size(); ++i) {
      const rk::RankedCandidate& item = result.ordered[i];
      const rk::Proposal& proposal = request.proposals[item.proposal_ordinal];
      std::cout << "  " << (i + 1) << ". tier=" << item.tier << " score="
                << item.score << " text=" << proposal.text_utf8
                << " source=" << rk::SourceKindName(proposal.source) << '\n';
    }
  };

  print("before any explicit selection", ranker.Rank(request, &learning));
  for (int i = 0; i < 3; ++i) {
    learning.Apply(MakeEvent(rk::EventKind::kSelected, "en", "c_coffer", 1,
                             static_cast<std::uint64_t>(i)));
  }
  print("after selecting coffer three times", ranker.Rank(request, &learning));
  learning.AdvanceTo(4 * rk::kDefaultHalfLifeTicks);
  print("after four half-lives of decay", ranker.Rank(request, &learning));
  learning.Clear();
  print("after clearing learned state", ranker.Rank(request, &learning));
}

struct Case {
  const char* name;
  void (*run)();
};

const Case kCases[] = {
    {"prefix", PrefixTests},
    {"ordering", OrderingTests},
    {"no_model", NoModelTests},
    {"learning", LearningTests},
    {"persistence", PersistenceTests},
    {"isolation", IsolationTests},
    {"negative", NegativeTests},
    {"decay", DecayTests},
    {"rejected_events", RejectionTests},
    {"clear", ClearTests},
    {"mixed_language", MixedLanguageTests},
    {"bounds", BoundsTests},
    {"spelling", SpellingTests},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "--forcefail") {
    Check(false, "intentional falsifiability control");
  } else if (mode == "--all") {
    for (const Case& item : kCases) item.run();
  } else if (mode == "--demo") {
    Demo();
    return 0;
  } else if (mode == "--list") {
    for (const Case& item : kCases) std::cout << item.name << '\n';
    return 0;
  } else {
    bool matched = false;
    const std::string wanted = mode.rfind("--", 0) == 0 ? mode.substr(2) : mode;
    for (const Case& item : kCases) {
      if (wanted == item.name) {
        item.run();
        matched = true;
        break;
      }
    }
    if (!matched) return 2;
  }
  if (failed != 0) return 1;
  std::cout << "RESULT PASS " << checks << " checks\n";
  return 0;
}
