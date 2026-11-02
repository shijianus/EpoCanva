#ifndef EPOCANVA_PREDICTION_ABBREVIATION_H_
#define EPOCANVA_PREDICTION_ABBREVIATION_H_

#include "model.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace prediction {
namespace abbreviation {

constexpr std::size_t kMaxResults = 8;
constexpr std::size_t kMaxOutputLexemes = 8;
constexpr std::size_t kMaxOutputUtf8Bytes = 4096;
constexpr std::size_t kMaxShorthandUtf8Bytes = 512;
constexpr std::size_t kMaxSourceUtf8Bytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxRecordUtf8Bytes = 4096;
constexpr std::size_t kMaxResourceRecords = 16384;
constexpr std::size_t kMaxResourceTrieNodes = 131072;
constexpr std::size_t kMaxModelQueries = 4096;
constexpr std::size_t kMaxIndexEntries = 1024u * 1024u;

struct BuildOptions {
  // Inclusive limits; these may be lowered, never raised above the constants.
  // Zero permits no resource bytes/records/nodes in the corresponding category.
  std::size_t max_source_utf8_bytes = kMaxSourceUtf8Bytes;
  std::size_t max_record_utf8_bytes = kMaxRecordUtf8Bytes;
  std::size_t max_records = kMaxResourceRecords;
  std::size_t max_trie_nodes = kMaxResourceTrieNodes;
};
struct BuildInfo {
  Status status = Status::kOk;
  std::size_t line_ordinal = 0;  // 1-based TSV record line; 0 if not applicable
  std::size_t explicit_expansions = 0;  // accepted records, not distinct keys
  std::size_t trie_nodes = 0;
};

struct Options {
  std::size_t max_suggestions = kMaxResults;  // 0..8 result records
  std::size_t max_output_lexemes = kMaxOutputLexemes;  // 1..8 per result
  std::size_t max_output_utf8_bytes = kMaxOutputUtf8Bytes;  // 1..4096 per result
  // 1..63 active paths per interpretation; one additional child probes pruning.
  std::size_t beam_width = 16;
  // Shared across ALL interpretations and explicit expansions in one Decode.
  // Unlike Model::Predict, ZERO MEANS NO WORK, never an exhaustive index scan.
  std::size_t max_model_queries = 256;  // Predict + Score calls; <=4096
  std::size_t max_index_entries = 32768;  // ranked entries visited; <=1048576
  std::size_t max_trie_nodes = 1024;  // prefix walk + subtree nodes; <=131072
  std::size_t max_explicit_expansions = 128;  // inspected records; <=16384
  bool enable_initialism = true;
  bool enable_explicit = true;
};

struct Suggestion {
  // Normalized model tokens; en/fr/ko joined with U+0020, zh/ja concatenated.
  // Hosts may render tokens differently; IDs/tokens remain authoritative.
  std::string text_utf8;
  std::vector<std::string> tokens_utf8;
  std::vector<LexemeId> lexeme_ids;
  double log_probability = 0.0;  // sum of conditional natural log probabilities
  // Model initial/prefix matches and exact explicit keys: 0. Partial explicit
  // keys: -ln(2) per untyped Unicode scalar (including any key separators).
  double match_log_score = 0.0;
  // Provenance from the validated resource, independent of numeric rank. ORed
  // across duplicate ID paths. exact_key implies explicit_key and means a full
  // normalized query==resource key match (not merely an initialism match).
  bool explicit_key = false;
  bool exact_key = false;
};
struct Result {
  Status status = Status::kModelUnavailable;
  std::vector<Suggestion> suggestions;
  // True only if top-k is exact within the documented grammar/output limits:
  // every interpretation/resource match searched without work or beam pruning.
  // Normal final top-k truncation is not beam pruning. Errors are never complete.
  bool complete = false;
  bool budget_exhausted = false;
  bool beam_pruned = false;
  std::size_t model_queries = 0;
  std::size_t index_entries_visited = 0;
  std::size_t trie_nodes_visited = 0;
  std::size_t explicit_expansions_visited = 0;
  // Model's current public API prepares once per Predict/Score call, NOT once
  // per Decode. This records that actual cost; no shared personal state/cache.
  std::size_t personal_preparations = 0;
};

class Decoder {
 public:
  // Atomic, immutable construction. Receives SOURCE TEXT, never a path. Caller
  // must admit the local resource and verify its pinned digest before calling.
  // This parser validates syntax/model membership, NOT resource authenticity.
  // Empty text is valid and enables model-only decoding (no built-in table).
  //
  // Nonempty source: strict scalar UTF-8, LF endings including final LF; no BOM,
  // CR, NUL, blank lines, comments, extra columns or duplicate (language,key,IDs).
  // Header: EPOCANVA_ABBREVIATIONS_TSV<TAB>1<LF>
  // Rows:   language<TAB>key<TAB>decimal-ID[ SPACE decimal-ID ...]<LF>
  // Languages are EXACTLY en/fr/zh/ja/ko. Canonical keys use the model's ASCII
  // lowercase rule only; non-ASCII scalars are neither folded nor normalized.
  // Keys contain 1..8 nonempty space-separated components (each <=256 UTF-8
  // bytes), with no leading/trailing/repeated spaces; total <=512 UTF-8 bytes.
  // IDs are canonical unsigned decimal without leading zeros, 1..8 per row.
  // Each must Find() an actual same-language kWord in this exact Model; EOS,
  // punctuation, OOV and foreign-language IDs are rejected, not re-hashed.
  static std::shared_ptr<const Decoder> Build(
      std::shared_ptr<const Model> model,
      std::string_view verified_expansion_source_utf8 = {},
      const BuildOptions& options = {}, BuildInfo* info = nullptr);
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // Context is <=2 admitted, non-EOS, same-language model IDs, oldest->newest.
  // Unknown or foreign IDs are rejected rather than silently backing off.
  // PersonalCounts is caller-owned/ephemeral and must stay unchanged during the
  // entire call. Caller gates consent/sensitive fields/focus as in model.h.
  // No files, network, logging, persistence, or shared mutable query state.
  //
  // Query keys obey the resource key grammar, but ASCII uppercase is accepted.
  // Initialism decoding calls Model::Predict with one token and a scalar-safe
  // prefix at each step. Separated input: all nonfinal components are single
  // Unicode scalar initials; final component is a whole next-word prefix
  // ("g m", "g mor"). Compact input: first 1..7 scalars are initials and the
  // remaining complete scalar suffix is the final prefix ("gm", "gmor").
  // Compact interpretations are tried in ascending output-token count (2..8);
  // a lone scalar is a one-token prefix. No untyped extra words are invented.
  // Noninitial irregular keys still participate in explicit-trie decoding.
  // Explicit keys are scored first so initialisms cannot starve vetted paths.
  // If both modes are enabled, explicit work gets half the remaining model
  // calls (rounded up), or enough for one 8-token path if available. Initialism
  // search then uses the remaining global budget. Hitting either phase's work
  // allowance is budget_exhausted, even if another phase leaves calls unused.
  //
  // Rank: descending (log_probability + match_log_score), then descending
  // log_probability, then ascending lexeme-ID vector. Identical ID sequences
  // are merged, retaining their best match score. No byte-truncated outputs.
  Result Decode(std::string_view shorthand_utf8, const Context& previous_ids,
                std::string_view language_id, const Options& options = {},
                const PersonalCounts* personal = nullptr) const;

  std::size_t explicit_expansion_count() const;

 private:
  struct Impl;
  explicit Decoder(std::unique_ptr<const Impl> impl);
  std::unique_ptr<const Impl> impl_;
};

}  // namespace abbreviation
}  // namespace prediction
}  // namespace epocanva
#endif  // EPOCANVA_PREDICTION_ABBREVIATION_H_
