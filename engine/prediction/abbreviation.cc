#include "abbreviation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace epocanva {
namespace prediction {
namespace abbreviation {
namespace {

bool SupportedLanguage(std::string_view language_id) {
  return language_id == "en" || language_id == "fr" || language_id == "zh" ||
         language_id == "ja" || language_id == "ko";
}

bool SpaceJoined(std::string_view language_id) {
  return language_id != "zh" && language_id != "ja";
}

// Only called after IsValidUtf8/NormalizeLexemeUtf8. Offsets and spans returned
// here count UTF-8 code units (bytes); an edge always owns one WHOLE scalar.
std::size_t ScalarUtf8Bytes(unsigned char first_utf8_byte) {
  if (first_utf8_byte < 0x80) return 1;
  if (first_utf8_byte < 0xe0) return 2;
  if (first_utf8_byte < 0xf0) return 3;
  return 4;
}
std::vector<std::string> Scalars(std::string_view valid_utf8) {
  std::vector<std::string> scalars;
  for (std::size_t offset_utf8_byte = 0; offset_utf8_byte < valid_utf8.size();) {
    const auto scalar_utf8_bytes =
        ScalarUtf8Bytes(static_cast<unsigned char>(valid_utf8[offset_utf8_byte]));
    scalars.emplace_back(valid_utf8.substr(offset_utf8_byte, scalar_utf8_bytes));
    offset_utf8_byte += scalar_utf8_bytes;
  }
  return scalars;
}

struct Key {
  std::string normalized_utf8;
  std::vector<std::string> components_utf8;
};
Status ParseKey(std::string_view utf8, Key* key) {
  if (utf8.size() > kMaxShorthandUtf8Bytes) return Status::kLimitExceeded;
  if (!IsValidUtf8(utf8)) return Status::kInvalidUtf8;
  if (utf8.empty()) return Status::kInvalidLexeme;
  std::size_t begin_utf8_byte = 0;
  for (;;) {
    if (key->components_utf8.size() == kMaxOutputLexemes)
      return Status::kLimitExceeded;
    const auto space_utf8_byte = utf8.find(' ', begin_utf8_byte);
    const auto end_utf8_byte = space_utf8_byte == std::string_view::npos ?
        utf8.size() : space_utf8_byte;
    const auto component_utf8 = utf8.substr(begin_utf8_byte,
                                            end_utf8_byte - begin_utf8_byte);
    if (component_utf8.size() > kMaxLexemeUtf8Bytes) return Status::kLimitExceeded;
    std::string normalized;
    if (!NormalizeLexemeUtf8(component_utf8, &normalized))
      return Status::kInvalidLexeme;
    if (!key->normalized_utf8.empty()) key->normalized_utf8.push_back(' ');
    key->normalized_utf8 += normalized;
    key->components_utf8.push_back(std::move(normalized));
    if (space_utf8_byte == std::string_view::npos) return Status::kOk;
    begin_utf8_byte = space_utf8_byte + 1;
  }
}

Status DecimalId(std::string_view bytes, LexemeId* id) {
  *id = 0;
  if (bytes.empty() || (bytes.size() > 1 && bytes.front() == '0'))
    return Status::kInvalidFormat;
  for (const auto byte : bytes) {
    if (byte < '0' || byte > '9') return Status::kInvalidFormat;
    const auto digit = static_cast<LexemeId>(byte - '0');
    if (*id > ((std::numeric_limits<LexemeId>::max)() - digit) / 10)
      return Status::kCountOverflow;
    *id = *id * 10 + digit;
  }
  return Status::kOk;
}

bool AppendToken(const LexemeRecord& row, bool space_joined,
                 std::size_t max_output_lexemes,
                 std::size_t max_output_utf8_bytes, Suggestion* suggestion) {
  if (suggestion->lexeme_ids.size() >= max_output_lexemes) return false;
  const std::size_t separator_utf8_bytes =
      space_joined && !suggestion->lexeme_ids.empty() ? 1 : 0;
  const auto addition_utf8_bytes = row.normalized_utf8.size() + separator_utf8_bytes;
  if (suggestion->text_utf8.size() > max_output_utf8_bytes ||
      addition_utf8_bytes > max_output_utf8_bytes - suggestion->text_utf8.size())
    return false;
  if (separator_utf8_bytes != 0) suggestion->text_utf8.push_back(' ');
  suggestion->text_utf8 += row.normalized_utf8;
  suggestion->tokens_utf8.push_back(row.normalized_utf8);
  suggestion->lexeme_ids.push_back(row.id);
  return true;
}

void AdvanceContext(LexemeId id, Context* context) {
  if (context->size() == 2) context->erase(context->begin());
  context->push_back(id);
}

bool Better(const Suggestion& left, const Suggestion& right) {
  const auto left_score = left.log_probability + left.match_log_score;
  const auto right_score = right.log_probability + right.match_log_score;
  if (left_score != right_score) return left_score > right_score;
  if (left.log_probability != right.log_probability)
    return left.log_probability > right.log_probability;
  return left.lexeme_ids < right.lexeme_ids;
}

bool ValidBuildOptions(const BuildOptions& options) {
  return options.max_source_utf8_bytes <= kMaxSourceUtf8Bytes &&
         options.max_record_utf8_bytes <= kMaxRecordUtf8Bytes &&
         options.max_records <= kMaxResourceRecords &&
         options.max_trie_nodes <= kMaxResourceTrieNodes;
}
bool ValidOptions(const Options& options) {
  return options.max_suggestions <= kMaxResults &&
         options.max_output_lexemes >= 1 && options.max_output_lexemes <= kMaxOutputLexemes &&
         options.max_output_utf8_bytes >= 1 &&
         options.max_output_utf8_bytes <= kMaxOutputUtf8Bytes &&
         options.beam_width >= 1 && options.beam_width < prediction::kMaxSuggestions &&
         options.max_model_queries <= kMaxModelQueries &&
         options.max_index_entries <= kMaxIndexEntries &&
         options.max_trie_nodes <= kMaxResourceTrieNodes &&
         options.max_explicit_expansions <= kMaxResourceRecords;
}

// Each interpretation specifies one full scalar-safe prefix per emitted word.
std::vector<std::vector<std::string>> Interpretations(const Key& key,
                                                     std::size_t max_lexemes) {
  std::vector<std::vector<std::string>> interpretations;
  if (key.components_utf8.size() > 1) {
    if (key.components_utf8.size() > max_lexemes) return interpretations;
    for (std::size_t component_ordinal = 0;
         component_ordinal + 1 < key.components_utf8.size(); ++component_ordinal) {
      if (Scalars(key.components_utf8[component_ordinal]).size() != 1)
        return interpretations;  // An irregular explicit key, not an initialism.
    }
    interpretations.push_back(key.components_utf8);
    return interpretations;
  }
  const auto scalars = Scalars(key.normalized_utf8);
  if (scalars.size() == 1) {
    interpretations.push_back(key.components_utf8);
    return interpretations;
  }
  const auto max_words = (std::min)(scalars.size(), max_lexemes);
  for (std::size_t word_count = 2; word_count <= max_words; ++word_count) {
    std::vector<std::string> prefixes;
    std::size_t suffix_begin_utf8_byte = 0;
    for (std::size_t scalar_ordinal = 0; scalar_ordinal + 1 < word_count;
         ++scalar_ordinal) {
      prefixes.push_back(scalars[scalar_ordinal]);
      suffix_begin_utf8_byte += scalars[scalar_ordinal].size();
    }
    prefixes.push_back(key.normalized_utf8.substr(suffix_begin_utf8_byte));
    interpretations.push_back(std::move(prefixes));
  }
  return interpretations;
}

struct Path {
  Suggestion suggestion;
  Context context;
};

struct Query {
  const Model& model;
  const Options& options;
  const PersonalCounts* personal;
  std::string language_id;
  Result* result;
  std::size_t model_query_limit;  // Absolute call counter ceiling for current phase.
  // Query-local evidence survives intermediate top-k eviction. At most the
  // budgeted number of explicit records can enter this map; nothing is cached.
  std::map<std::vector<LexemeId>, bool> explicit_evidence;  // value = exact key

  bool CanCallModel(bool indexed) {
    if (result->model_queries >= model_query_limit ||
        (indexed && result->index_entries_visited >= options.max_index_entries)) {
      result->budget_exhausted = true;
      return false;
    }
    return true;
  }
  bool AcceptStatus(Status status) {
    if (status == Status::kOk || status == Status::kBeamPruned ||
        status == Status::kBudgetExhausted) return true;
    result->status = status;
    result->suggestions.clear();
    return false;
  }
  bool ValidatePersonal(const Context& context) {
    if (!personal) return true;
    if (!CanCallModel(false)) return false;
    PredictOptions validation;
    validation.max_suggestions = 0;
    validation.language_id = language_id;
    // max_suggestions=0 performs ONLY bounded PersonalCounts validation.
    ++result->model_queries;
    const auto checked = model.Predict(std::nullopt, context, validation, personal);
    result->personal_preparations += checked.personal_preparations;
    return AcceptStatus(checked.status);
  }
  bool Predict(std::string_view prefix_utf8, const Context& context,
               PredictResult* predicted) {
    if (!CanCallModel(true)) return false;
    PredictOptions bounded;
    bounded.max_suggestions = options.beam_width + 1;  // Explicit pruning sentinel.
    bounded.max_words = 1;
    bounded.beam_width = options.beam_width;
    bounded.max_index_entries = options.max_index_entries - result->index_entries_visited;
    bounded.language_id = language_id;
    ++result->model_queries;
    *predicted = model.Predict(prefix_utf8, context, bounded, personal);
    result->index_entries_visited += predicted->index_entries_visited;
    result->personal_preparations += predicted->personal_preparations;
    result->budget_exhausted = result->budget_exhausted || predicted->budget_exhausted;
    result->beam_pruned = result->beam_pruned || predicted->beam_pruned;
    return AcceptStatus(predicted->status);
  }
  bool Score(const Context& context, LexemeId id, double* log_probability) {
    if (!CanCallModel(false)) return false;
    ++result->model_queries;
    if (personal) ++result->personal_preparations;
    const auto scored = model.Score(context, id, personal);
    if (!AcceptStatus(scored.status)) return false;
    *log_probability = scored.log_probability;
    return true;
  }
  void Retain(Suggestion candidate) {
    if (candidate.explicit_key) {
      auto& exact_key = explicit_evidence[candidate.lexeme_ids];
      exact_key = exact_key || candidate.exact_key;
    }
    const auto evidence = explicit_evidence.find(candidate.lexeme_ids);
    if (evidence != explicit_evidence.end()) {
      candidate.explicit_key = true;
      candidate.exact_key = evidence->second;
    }
    auto& best = result->suggestions;
    const auto same = std::find_if(best.begin(), best.end(), [&](const auto& other) {
      return candidate.lexeme_ids == other.lexeme_ids;
    });
    if (same != best.end()) {
      const bool explicit_key = candidate.explicit_key || same->explicit_key;
      const bool exact_key = candidate.exact_key || same->exact_key;
      if (Better(candidate, *same)) *same = std::move(candidate);
      same->explicit_key = explicit_key;
      same->exact_key = exact_key;
    } else {
      best.push_back(std::move(candidate));
    }
    std::sort(best.begin(), best.end(), Better);
    if (best.size() > options.max_suggestions) best.resize(options.max_suggestions);
  }
  bool Initialism(const std::vector<std::string>& prefixes_utf8,
                  const Context& previous_ids) {
    std::vector<Path> beam{{{}, previous_ids}};
    for (const auto& prefix_utf8 : prefixes_utf8) {
      std::vector<Path> expanded;
      for (const auto& path : beam) {
        PredictResult predicted;
        if (!Predict(prefix_utf8, path.context, &predicted)) {
          if (result->status != Status::kOk) return false;
          break;  // Other paths could not be searched; keep only full expansions.
        }
        // A full sentinel page can hide further same-prefix IDs, including when
        // punctuation/output-limit filtering removes a returned row. Never claim
        // exhaustive word search just because filtering shortened this page.
        if (predicted.suggestions.size() > options.beam_width)
          result->beam_pruned = true;
        for (const auto& next : predicted.suggestions) {
          if (next.lexeme_ids.size() != 1) continue;
          const auto* row = model.Find(next.lexeme_ids.front());
          if (!row || row->kind != TokenKind::kWord || row->language_id != language_id)
            continue;
          Path child = path;
          if (!AppendToken(*row, SpaceJoined(language_id), options.max_output_lexemes,
                           options.max_output_utf8_bytes, &child.suggestion)) continue;
          child.suggestion.log_probability += next.log_probability;
          AdvanceContext(row->id, &child.context);
          const auto where = std::lower_bound(expanded.begin(), expanded.end(), child,
              [](const Path& left, const Path& right) {
                return Better(left.suggestion, right.suggestion);
              });
          expanded.insert(where, std::move(child));
          if (expanded.size() > options.beam_width) {
            expanded.pop_back();
            result->beam_pruned = true;
          }
        }
      }
      beam = std::move(expanded);
      if (beam.empty()) return true;
    }
    for (auto& path : beam) Retain(std::move(path.suggestion));
    return true;
  }
  bool VisitTrieNode() {
    if (result->trie_nodes_visited >= options.max_trie_nodes) {
      result->budget_exhausted = true;
      return false;
    }
    ++result->trie_nodes_visited;
    return true;
  }
};

}  // namespace

struct Decoder::Impl {
  struct Node {
    // Whole-scalar UTF-8 edges, in deterministic binary byte order.
    std::map<std::string, std::size_t> children;  // node ordinals
    std::vector<std::size_t> expansions;  // expansion record ordinals
  };
  struct Expansion {
    Suggestion suggestion;
    std::size_t key_scalar_count = 0;  // Unicode scalars, not UTF-8 bytes
  };
  std::shared_ptr<const Model> model;
  std::map<std::string, std::size_t> language_roots;  // node ordinals
  std::vector<Node> nodes;
  std::vector<Expansion> expansions;

  bool Explicit(const std::vector<std::string>& prefix_scalars,
                const Context& previous_ids, Query* query) const {
    const auto root = language_roots.find(query->language_id);
    if (root == language_roots.end()) return true;
    if (!query->VisitTrieNode()) return true;
    auto node_ordinal = root->second;
    for (const auto& scalar_utf8 : prefix_scalars) {
      const auto child = nodes[node_ordinal].children.find(scalar_utf8);
      if (child == nodes[node_ordinal].children.end()) return true;
      if (!query->VisitTrieNode()) return true;
      node_ordinal = child->second;
    }
    const auto inspect = [&](std::size_t current_node_ordinal) {
      for (const auto expansion_ordinal : nodes[current_node_ordinal].expansions) {
        if (query->result->explicit_expansions_visited >=
            query->options.max_explicit_expansions) {
          query->result->budget_exhausted = true;
          return false;
        }
        ++query->result->explicit_expansions_visited;
        const auto& expansion = expansions[expansion_ordinal];
        if (expansion.suggestion.lexeme_ids.size() > query->options.max_output_lexemes ||
            expansion.suggestion.text_utf8.size() > query->options.max_output_utf8_bytes)
          continue;  // Outside this query's output domain, not search truncation.
        Suggestion candidate = expansion.suggestion;
        Context context = previous_ids;
        for (const auto id : candidate.lexeme_ids) {
          double log_probability = 0.0;
          if (!query->Score(context, id, &log_probability)) return false;
          candidate.log_probability += log_probability;
          AdvanceContext(id, &context);
        }
        const auto untyped_scalar_count = expansion.key_scalar_count - prefix_scalars.size();
        candidate.match_log_score = -std::log(2.0) * static_cast<double>(untyped_scalar_count);
        candidate.explicit_key = true;
        candidate.exact_key = untyped_scalar_count == 0;
        query->Retain(std::move(candidate));
      }
      return true;
    };
    if (!inspect(node_ordinal)) return query->result->status == Status::kOk;
    struct Frame {
      std::size_t node_ordinal;
      std::map<std::string, std::size_t>::const_iterator next_child;
    };
    // An iterator stack visits only budgeted nodes; it never materializes a wide
    // unvisited subtree. Stack depth is bounded by the key's scalar count.
    std::vector<Frame> stack{{node_ordinal, nodes[node_ordinal].children.begin()}};
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_child == nodes[frame.node_ordinal].children.end()) {
        stack.pop_back();
        continue;
      }
      const auto child_ordinal = (frame.next_child++)->second;
      if (!query->VisitTrieNode()) return true;
      if (!inspect(child_ordinal)) return query->result->status == Status::kOk;
      stack.push_back({child_ordinal, nodes[child_ordinal].children.begin()});
    }
    return true;
  }
};

Decoder::Decoder(std::unique_ptr<const Impl> impl) : impl_(std::move(impl)) {}
Decoder::~Decoder() = default;
std::size_t Decoder::explicit_expansion_count() const { return impl_->expansions.size(); }

std::shared_ptr<const Decoder> Decoder::Build(
    std::shared_ptr<const Model> model, std::string_view verified_expansion_source_utf8,
    const BuildOptions& options, BuildInfo* info) {
  if (info) *info = {};
  std::size_t line_ordinal = 0;
  const auto fail = [&](Status status) -> std::shared_ptr<const Decoder> {
    if (info) { info->status = status; info->line_ordinal = line_ordinal; }
    return nullptr;
  };
  try {
    if (!model || !model->available()) return fail(Status::kModelUnavailable);
    if (!ValidBuildOptions(options)) return fail(Status::kInvalidOptions);
    const auto source = verified_expansion_source_utf8;
    if (source.size() > options.max_source_utf8_bytes) return fail(Status::kLimitExceeded);
    auto impl = std::make_unique<Impl>();
    impl->model = std::move(model);
    if (source.empty()) return std::shared_ptr<const Decoder>(new Decoder(std::move(impl)));
    if (!IsValidUtf8(source)) return fail(Status::kInvalidUtf8);
    if (source.back() != '\n' || source.find('\r') != std::string_view::npos ||
        source.find('\0') != std::string_view::npos) return fail(Status::kInvalidFormat);
    struct Row {
      std::string language_id;
      std::string key_utf8;
      Suggestion suggestion;
      std::size_t line_ordinal;
    };
    std::vector<Row> rows;
    std::size_t begin_utf8_byte = 0;
    while (begin_utf8_byte < source.size()) {
      const auto end_utf8_byte = source.find('\n', begin_utf8_byte);
      const auto line = source.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte);
      begin_utf8_byte = end_utf8_byte + 1;
      ++line_ordinal;
      if (line.size() > options.max_record_utf8_bytes) return fail(Status::kLimitExceeded);
      if (line_ordinal == 1) {
        const auto tab_utf8_byte = line.find('\t');
        if (tab_utf8_byte == std::string_view::npos ||
            line.substr(0, tab_utf8_byte) != "EPOCANVA_ABBREVIATIONS_TSV")
          return fail(Status::kInvalidFormat);
        if (line.substr(tab_utf8_byte + 1) != "1") return fail(Status::kUnsupportedVersion);
        continue;
      }
      if (rows.size() >= options.max_records) return fail(Status::kLimitExceeded);
      const auto first_tab_utf8_byte = line.find('\t');
      if (first_tab_utf8_byte == std::string_view::npos) return fail(Status::kInvalidFormat);
      const auto second_tab_utf8_byte = line.find('\t', first_tab_utf8_byte + 1);
      if (second_tab_utf8_byte == std::string_view::npos ||
          line.find('\t', second_tab_utf8_byte + 1) != std::string_view::npos)
        return fail(Status::kInvalidFormat);
      const auto language_id = line.substr(0, first_tab_utf8_byte);
      if (!SupportedLanguage(language_id)) return fail(Status::kInvalidFormat);
      const auto key_utf8 = line.substr(first_tab_utf8_byte + 1,
                                        second_tab_utf8_byte - first_tab_utf8_byte - 1);
      Key key;
      const auto key_status = ParseKey(key_utf8, &key);
      if (key_status != Status::kOk) return fail(key_status);
      if (key.normalized_utf8 != key_utf8) return fail(Status::kInvalidLexeme);
      Row row{std::string(language_id), std::move(key.normalized_utf8), {}, line_ordinal};
      const auto ids = line.substr(second_tab_utf8_byte + 1);
      std::size_t id_begin_utf8_byte = 0;
      for (;;) {
        if (row.suggestion.lexeme_ids.size() >= kMaxOutputLexemes)
          return fail(Status::kLimitExceeded);
        const auto space_utf8_byte = ids.find(' ', id_begin_utf8_byte);
        const auto id_end_utf8_byte = space_utf8_byte == std::string_view::npos ?
            ids.size() : space_utf8_byte;
        LexemeId id = 0;
        const auto id_status = DecimalId(ids.substr(id_begin_utf8_byte,
                                                    id_end_utf8_byte - id_begin_utf8_byte), &id);
        if (id_status != Status::kOk) return fail(id_status);
        const auto* lexeme = impl->model->Find(id);
        if (!lexeme) return fail(Status::kUnknownId);
        if (lexeme->language_id != language_id || lexeme->kind != TokenKind::kWord)
          return fail(Status::kInvalidLexeme);
        if (!AppendToken(*lexeme, SpaceJoined(language_id), kMaxOutputLexemes,
                         kMaxOutputUtf8Bytes, &row.suggestion)) return fail(Status::kLimitExceeded);
        if (space_utf8_byte == std::string_view::npos) break;
        id_begin_utf8_byte = space_utf8_byte + 1;
      }
      rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
      return std::tie(left.language_id, left.key_utf8, left.suggestion.lexeme_ids) <
             std::tie(right.language_id, right.key_utf8, right.suggestion.lexeme_ids);
    });
    for (std::size_t row_ordinal = 0; row_ordinal < rows.size(); ++row_ordinal) {
      auto& row = rows[row_ordinal];
      line_ordinal = row.line_ordinal;
      if (row_ordinal != 0) {
        const auto& previous = rows[row_ordinal - 1];
        if (row.language_id == previous.language_id && row.key_utf8 == previous.key_utf8 &&
            row.suggestion.lexeme_ids == previous.suggestion.lexeme_ids) {
          line_ordinal = (std::max)(row.line_ordinal, previous.line_ordinal);
          return fail(Status::kDuplicateRecord);
        }
      }
    }
    for (auto& row : rows) {
      line_ordinal = row.line_ordinal;
      auto root = impl->language_roots.find(row.language_id);
      if (root == impl->language_roots.end()) {
        if (impl->nodes.size() >= options.max_trie_nodes) return fail(Status::kLimitExceeded);
        const auto root_ordinal = impl->nodes.size();
        impl->nodes.emplace_back();
        root = impl->language_roots.emplace(row.language_id, root_ordinal).first;
      }
      auto node_ordinal = root->second;
      const auto scalars = Scalars(row.key_utf8);
      for (const auto& scalar_utf8 : scalars) {
        const auto child = impl->nodes[node_ordinal].children.find(scalar_utf8);
        if (child != impl->nodes[node_ordinal].children.end()) {
          node_ordinal = child->second;
          continue;
        }
        if (impl->nodes.size() >= options.max_trie_nodes) return fail(Status::kLimitExceeded);
        const auto child_ordinal = impl->nodes.size();
        impl->nodes.emplace_back();
        impl->nodes[node_ordinal].children.emplace(scalar_utf8, child_ordinal);
        node_ordinal = child_ordinal;
      }
      impl->nodes[node_ordinal].expansions.push_back(impl->expansions.size());
      impl->expansions.push_back({std::move(row.suggestion), scalars.size()});
    }
    if (info) {
      info->explicit_expansions = impl->expansions.size();
      info->trie_nodes = impl->nodes.size();
      info->line_ordinal = 0;
    }
    return std::shared_ptr<const Decoder>(new Decoder(std::move(impl)));
  } catch (const std::bad_alloc&) { return fail(Status::kLimitExceeded); }
    catch (const std::length_error&) { return fail(Status::kLimitExceeded); }
}

Result Decoder::Decode(std::string_view shorthand_utf8, const Context& previous_ids,
                       std::string_view language_id, const Options& options,
                       const PersonalCounts* personal) const {
  Result result;
  const auto fail = [&](Status status) {
    result.status = status;
    result.suggestions.clear();
    result.complete = false;
    return result;
  };
  try {
    if (!ValidOptions(options) || !SupportedLanguage(language_id))
      return fail(Status::kInvalidOptions);
    if (previous_ids.size() > 2) return fail(Status::kInvalidContext);
    for (const auto id : previous_ids) {
      const auto* row = impl_->model->Find(id);
      if (!row || row->language_id != language_id) return fail(Status::kInvalidContext);
    }
    Key key;
    const auto key_status = ParseKey(shorthand_utf8, &key);
    if (key_status != Status::kOk) return fail(key_status);
    result.status = Status::kOk;
    Query query{*impl_->model, options, personal, std::string(language_id), &result,
                options.max_model_queries, {}};
    if (!query.ValidatePersonal(previous_ids)) {
      if (result.status != Status::kOk) return result;
      result.status = Status::kBudgetExhausted;
      return result;
    }
    if (options.max_suggestions == 0) { result.complete = true; return result; }
    if (options.enable_explicit) {
      // A broad initialism must not consume the calls needed to score a vetted
      // shortcut. Conversely, a large resource must not consume every model
      // call: give explicit matches half the remaining calls (rounded up), but
      // at least one maximum-sized path if the global allowance permits it.
      // Hitting this work-share ceiling reports budget_exhausted, not exactness.
      if (options.enable_initialism) {
        const auto remaining_calls = options.max_model_queries - result.model_queries;
        const auto explicit_calls = (std::min)(remaining_calls,
            (std::max)(kMaxOutputLexemes, (remaining_calls + 1) / 2));
        query.model_query_limit = result.model_queries + explicit_calls;
      }
      if (!impl_->Explicit(Scalars(key.normalized_utf8), previous_ids, &query)) return result;
      query.model_query_limit = options.max_model_queries;
    }
    if (options.enable_initialism) {
      for (const auto& prefixes_utf8 : Interpretations(key, options.max_output_lexemes)) {
        if (!query.Initialism(prefixes_utf8, previous_ids)) return result;
      }
    }
    result.complete = !result.budget_exhausted && !result.beam_pruned;
    result.status = result.budget_exhausted ? Status::kBudgetExhausted :
                    result.beam_pruned ? Status::kBeamPruned : Status::kOk;
    return result;
  } catch (const std::bad_alloc&) { return fail(Status::kLimitExceeded); }
    catch (const std::length_error&) { return fail(Status::kLimitExceeded); }
}

}  // namespace abbreviation
}  // namespace prediction
}  // namespace epocanva
