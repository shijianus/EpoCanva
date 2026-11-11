#include "model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace epocanva {
namespace prediction {
namespace {

using Count = std::uint64_t;
using Pair = std::pair<LexemeId, LexemeId>;

bool NextScalar(std::string_view text, std::size_t* utf8_byte,
                std::uint32_t* scalar) {
  if (*utf8_byte >= text.size()) return false;
  const auto first = static_cast<unsigned char>(text[(*utf8_byte)++]);
  if (first < 0x80) { *scalar = first; return true; }
  unsigned continuation_count = 0;
  std::uint32_t minimum = 0;
  if (first >= 0xc2 && first <= 0xdf) {
    continuation_count = 1; *scalar = first & 0x1fu; minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    continuation_count = 2; *scalar = first & 0x0fu; minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    continuation_count = 3; *scalar = first & 0x07u; minimum = 0x10000;
  } else { return false; }
  if (text.size() - *utf8_byte < continuation_count) return false;
  for (unsigned ordinal = 0; ordinal < continuation_count; ++ordinal) {
    const auto byte = static_cast<unsigned char>(text[(*utf8_byte)++]);
    if ((byte & 0xc0u) != 0x80u) return false;
    *scalar = (*scalar << 6u) | (byte & 0x3fu);
  }
  return *scalar >= minimum && *scalar <= 0x10ffff &&
         !(*scalar >= 0xd800 && *scalar <= 0xdfff);
}

bool ForbiddenScalar(std::uint32_t scalar) {
  return scalar <= 0x20 || (scalar >= 0x7f && scalar <= 0x9f) ||
         scalar == 0xa0 || scalar == 0x1680 ||
         (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 ||
         scalar == 0x2029 || scalar == 0x202f || scalar == 0x205f ||
         scalar == 0x3000;
}

bool AddChecked(Count count, Count* total) {
  if (count > (std::numeric_limits<Count>::max)() - *total) return false;
  *total += count;
  return true;
}

bool IsFingerprint(std::string_view value) {
  if (value.size() != 64) return false;
  for (char byte : value) {
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return false;
  }
  return true;
}

// These ordinals count lexicon/ranked RECORDS, never text offsets. A compact
// uint32 index is sufficient for the bounded on-disk model; FromRecords checks
// the same upper bound before constructing the indexes.
using RecordOrdinal = std::uint32_t;
constexpr RecordOrdinal kNoRecord = (std::numeric_limits<RecordOrdinal>::max)();
using PrefixOrdinals = std::unordered_map<LexemeId, RecordOrdinal>;
struct RankedCount { LexemeId id; Count count; };

struct PrefixRow {
  RecordOrdinal lexicon_ordinal;  // lexical byte order (or language/lexical order)
  RecordOrdinal ranked_ordinal;  // lower ordinal = greater count, then lower ID
};
struct PrefixIndex {
  std::vector<PrefixRow> rows;
  // Iterative range-min tree: payload is the rows[] ordinal whose ranked_ordinal
  // is smallest. A lexical prefix is a half-open interval, so one range maximum
  // plus split intervals enumerates its counts without unrelated lexical scans.
  std::vector<RecordOrdinal> minimum;

  RecordOrdinal Better(RecordOrdinal left, RecordOrdinal right) const {
    if (left == kNoRecord) return right;
    if (right == kNoRecord) return left;
    return rows[left].ranked_ordinal < rows[right].ranked_ordinal ? left : right;
  }
  void Build(const PrefixOrdinals& ordinals, const std::vector<RankedCount>& ranked);
  std::size_t Lower(RecordOrdinal lexicon_ordinal) const {
    return static_cast<std::size_t>(std::lower_bound(rows.begin(), rows.end(), lexicon_ordinal,
        [](const PrefixRow& row, RecordOrdinal key) { return row.lexicon_ordinal < key; }) - rows.begin());
  }
  RecordOrdinal Best(std::size_t begin_record, std::size_t end_record) const {
    RecordOrdinal best = kNoRecord;
    for (begin_record += rows.size(), end_record += rows.size(); begin_record < end_record;
         begin_record /= 2, end_record /= 2) {
      if (begin_record % 2 != 0) best = Better(best, minimum[begin_record++]);
      if (end_record % 2 != 0) best = Better(best, minimum[--end_record]);
    }
    return best;
  }
};
void PrefixIndex::Build(const PrefixOrdinals& ordinals, const std::vector<RankedCount>& ranked) {
  rows.reserve(ranked.size());
  for (std::size_t ranked_record = 0; ranked_record < ranked.size(); ++ranked_record) {
    if (ranked[ranked_record].id == kEosId) continue;  // Prefixes never emit EOS.
    rows.push_back({ordinals.at(ranked[ranked_record].id),
                    static_cast<RecordOrdinal>(ranked_record)});
  }
  std::sort(rows.begin(), rows.end(), [](const PrefixRow& left, const PrefixRow& right) {
    return left.lexicon_ordinal < right.lexicon_ordinal;
  });
  minimum.assign(rows.size() * 2, kNoRecord);
  for (std::size_t record = 0; record < rows.size(); ++record)
    minimum[rows.size() + record] = static_cast<RecordOrdinal>(record);
  for (std::size_t node = rows.size(); node > 1;) {
    --node;
    minimum[node] = Better(minimum[node * 2], minimum[node * 2 + 1]);
  }
}
struct PrefixCursor {
  struct Span {
    std::size_t begin_record;
    std::size_t end_record;
    RecordOrdinal best_record;
    RecordOrdinal ranked_record;
  };
  const PrefixIndex* index;
  std::vector<Span> heap;
  static bool Worse(const Span& left, const Span& right) {
    return left.ranked_record > right.ranked_record;
  }
  PrefixCursor(const PrefixIndex& prefix, RecordOrdinal begin_lexicon_record,
               RecordOrdinal end_lexicon_record) : index(&prefix) {
    Add(prefix.Lower(begin_lexicon_record), prefix.Lower(end_lexicon_record));
  }
  void Add(std::size_t begin_record, std::size_t end_record) {
    if (begin_record == end_record) return;
    const auto best = index->Best(begin_record, end_record);
    heap.push_back({begin_record, end_record, best, index->rows[best].ranked_ordinal});
    std::push_heap(heap.begin(), heap.end(), Worse);
  }
  bool empty() const { return heap.empty(); }
  RecordOrdinal Front() const { return heap.front().ranked_record; }
  void Pop() {
    std::pop_heap(heap.begin(), heap.end(), Worse);
    const auto span = heap.back();
    heap.pop_back();
    Add(span.begin_record, span.best_record);
    Add(static_cast<std::size_t>(span.best_record) + 1, span.end_record);
  }
};

struct Distribution {
  std::map<LexemeId, Count> counts;
  std::vector<RankedCount> ranked;
  std::unique_ptr<const PrefixIndex> prefix_global;
  // Omitted on monolingual models; global lexical ordering is identical there.
  std::unique_ptr<const PrefixIndex> prefix_language;
  Count total = 0;
  Count Get(LexemeId id) const {
    const auto found = counts.find(id);
    return found == counts.end() ? 0 : found->second;
  }
  void Rank() {
    ranked.clear();
    ranked.reserve(counts.size());
    for (const auto& item : counts) ranked.push_back({item.first, item.second});
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      return a.count != b.count ? a.count > b.count : a.id < b.id;
    });
  }
  void IndexPrefixes(const PrefixOrdinals& global, const PrefixOrdinals* language) {
    if (ranked.empty()) return;
    auto global_index = std::make_unique<PrefixIndex>();
    global_index->Build(global, ranked);
    prefix_global = std::move(global_index);
    if (language) {
      auto language_index = std::make_unique<PrefixIndex>();
      language_index->Build(*language, ranked);
      prefix_language = std::move(language_index);
    }
  }
  const PrefixIndex& Prefix(bool language_filtered) const {
    return language_filtered && prefix_language ? *prefix_language : *prefix_global;
  }
};
struct CountIndexes {
  Distribution unigram;
  std::map<LexemeId, Distribution> bigram;
  std::map<Pair, Distribution> trigram;
  Count bigram_total = 0;
  Count trigram_total = 0;
  const Distribution* Bigram(LexemeId previous) const {
    const auto found = bigram.find(previous);
    return found == bigram.end() ? nullptr : &found->second;
  }
  const Distribution* Trigram(LexemeId older, LexemeId newer) const {
    const auto found = trigram.find({older, newer});
    return found == trigram.end() ? nullptr : &found->second;
  }
  Count PairCount(LexemeId previous, LexemeId id) const {
    const auto* found = Bigram(previous);
    return found ? found->Get(id) : 0;
  }
  void Rank() {
    unigram.Rank();
    for (auto& item : bigram) item.second.Rank();
    for (auto& item : trigram) item.second.Rank();
  }
  void IndexPrefixes(const PrefixOrdinals& global, const PrefixOrdinals* language) {
    unigram.IndexPrefixes(global, language);
    for (auto& item : bigram) item.second.IndexPrefixes(global, language);
    for (auto& item : trigram) item.second.IndexPrefixes(global, language);
  }
};

Status InsertCount(Distribution* into, LexemeId id, Count count) {
  if (count == 0) return Status::kInvalidFormat;
  if (!into->counts.emplace(id, count).second) return Status::kDuplicateRecord;
  if (!AddChecked(count, &into->total)) return Status::kCountOverflow;
  return Status::kOk;
}

Status BuildCounts(const std::set<LexemeId>& ids,
                   const std::vector<UnigramRecord>& unigrams,
                   const std::vector<BigramRecord>& bigrams,
                   const std::vector<TrigramRecord>& trigrams,
                   CountIndexes* out) {
  const auto known = [&](LexemeId id) { return id == 0 || ids.count(id) != 0; };
  for (const auto& row : unigrams) {
    if (!known(row.id)) return Status::kUnknownId;
    const auto status = InsertCount(&out->unigram, row.id, row.count);
    if (status != Status::kOk) return status;
  }
  for (const auto& row : bigrams) {
    if (!known(row.id) || row.previous_id == 0 || !known(row.previous_id))
      return Status::kUnknownId;
    const auto status = InsertCount(&out->bigram[row.previous_id], row.id, row.count);
    if (status != Status::kOk) return status;
    if (!AddChecked(row.count, &out->bigram_total)) return Status::kCountOverflow;
  }
  for (const auto& row : trigrams) {
    if (!known(row.id) || row.previous2_id == 0 || row.previous1_id == 0 ||
        !known(row.previous2_id) || !known(row.previous1_id)) return Status::kUnknownId;
    const auto status = InsertCount(
        &out->trigram[{row.previous2_id, row.previous1_id}], row.id, row.count);
    if (status != Status::kOk) return status;
    if (!AddChecked(row.count, &out->trigram_total)) return Status::kCountOverflow;
  }
  return Status::kOk;
}

Status CheckMarginals(const CountIndexes& counts) {
  std::map<LexemeId, Count> incoming;
  for (const auto& history : counts.bigram) {
    if (history.second.total > counts.unigram.Get(history.first))
      return Status::kInconsistentTotals;
    for (const auto& row : history.second.counts) {
      if (!AddChecked(row.second, &incoming[row.first])) return Status::kCountOverflow;
    }
  }
  for (const auto& row : incoming) {
    if (row.second > counts.unigram.Get(row.first)) return Status::kInconsistentTotals;
  }
  std::map<Pair, Count> suffixes;
  for (const auto& history : counts.trigram) {
    if (history.second.total > counts.PairCount(history.first.first, history.first.second))
      return Status::kInconsistentTotals;
    for (const auto& row : history.second.counts) {
      if (!AddChecked(row.second, &suffixes[{history.first.second, row.first}]))
        return Status::kCountOverflow;
    }
  }
  for (const auto& row : suffixes) {
    if (row.second > counts.PairCount(row.first.first, row.first.second))
      return Status::kInconsistentTotals;
  }
  return Status::kOk;
}

struct Term { const Distribution* distribution; long double coefficient; };
struct LinearScore {
  long double constant = 0.0L;
  std::vector<Term> terms;
  void Interpolate(const Distribution* distribution, long double prior) {
    if (!distribution || distribution->total == 0) return;
    const long double denominator = static_cast<long double>(distribution->total) + prior;
    const long double scale = prior / denominator;
    constant *= scale;
    for (auto& term : terms) term.coefficient *= scale;
    terms.push_back({distribution, 1.0L / denominator});
  }
  double Probability(LexemeId id) const {
    long double value = constant;
    for (const auto& term : terms)
      value += term.coefficient * static_cast<long double>(term.distribution->Get(id));
    return static_cast<double>(value);
  }
};

// Small self-contained SHA-256 implementation. It verifies model payload bytes;
// it is not a signature, source-authorisation check, or personal-data hash.
std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}
std::string Sha256(std::string_view bytes) {
  constexpr std::array<std::uint32_t, 64> k = {{
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
  std::array<std::uint32_t, 8> h = {{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
  const auto block = [&](const unsigned char* data) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t word = 0; word < 16; ++word) {
      const std::size_t byte = word * 4;
      w[word] = (std::uint32_t(data[byte]) << 24u) |
                (std::uint32_t(data[byte + 1]) << 16u) |
                (std::uint32_t(data[byte + 2]) << 8u) | data[byte + 3];
    }
    for (std::size_t word = 16; word < 64; ++word) {
      const auto x = w[word - 15], y = w[word - 2];
      const auto s0 = RotateRight(x, 7) ^ RotateRight(x, 18) ^ (x >> 3u);
      const auto s1 = RotateRight(y, 17) ^ RotateRight(y, 19) ^ (y >> 10u);
      w[word] = w[word - 16] + s0 + w[word - 7] + s1;
    }
    auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], z=h[7];
    for (std::size_t round = 0; round < 64; ++round) {
      const auto s1 = RotateRight(e,6) ^ RotateRight(e,11) ^ RotateRight(e,25);
      const auto choose = (e & f) ^ (~e & g);
      const auto t1 = z + s1 + choose + k[round] + w[round];
      const auto s0 = RotateRight(a,2) ^ RotateRight(a,13) ^ RotateRight(a,22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto t2 = s0 + majority;
      z=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=z;
  };
  std::size_t byte = 0;
  while (bytes.size() - byte >= 64) {
    block(reinterpret_cast<const unsigned char*>(bytes.data() + byte));
    byte += 64;
  }
  std::array<unsigned char, 128> final{};
  const auto remainder_bytes = bytes.size() - byte;
  for (std::size_t offset_utf8_byte = 0; offset_utf8_byte < remainder_bytes; ++offset_utf8_byte)
    final[offset_utf8_byte] = static_cast<unsigned char>(bytes[byte + offset_utf8_byte]);
  final[remainder_bytes] = 0x80;
  const std::size_t padded_bytes = remainder_bytes < 56 ? 64 : 128;
  const std::uint64_t bit_count = static_cast<std::uint64_t>(bytes.size()) * 8u;
  for (unsigned ordinal = 0; ordinal < 8; ++ordinal)
    final[padded_bytes - 1 - ordinal] = static_cast<unsigned char>(bit_count >> (ordinal * 8u));
  block(final.data());
  if (padded_bytes == 128) block(final.data() + 64);
  constexpr char hex[] = "0123456789abcdef";
  std::string digest;
  digest.reserve(64);
  for (const auto word : h) {
    for (int nibble = 7; nibble >= 0; --nibble)
      digest.push_back(hex[(word >> (static_cast<unsigned>(nibble) * 4u)) & 15u]);
  }
  return digest;
}

struct Lines {
  std::string_view text;
  std::size_t utf8_byte = 0;
  std::size_t ordinal = 0;
  bool Next(std::string_view* line) {
    if (utf8_byte == text.size()) return false;
    const auto end_utf8_byte = text.find('\n', utf8_byte);
    if (end_utf8_byte == std::string_view::npos) return false;
    *line = text.substr(utf8_byte, end_utf8_byte - utf8_byte);
    utf8_byte = end_utf8_byte + 1;
    ++ordinal;
    return true;
  }
};
std::vector<std::string_view> Columns(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t begin_utf8_byte = 0;
  for (;;) {
    const auto end_utf8_byte = line.find('\t', begin_utf8_byte);
    if (end_utf8_byte == std::string_view::npos) {
      out.push_back(line.substr(begin_utf8_byte));
      return out;
    }
    out.push_back(line.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte));
    begin_utf8_byte = end_utf8_byte + 1;
  }
}
Status Decimal(std::string_view bytes, Count* value) {
  *value = 0;
  if (bytes.empty() || (bytes.size() > 1 && bytes.front() == '0'))
    return Status::kInvalidFormat;
  for (char byte : bytes) {
    if (byte < '0' || byte > '9') return Status::kInvalidFormat;
    const auto digit = static_cast<Count>(byte - '0');
    if (*value > ((std::numeric_limits<Count>::max)() - digit) / 10)
      return Status::kCountOverflow;
    *value = *value * 10 + digit;
  }
  return Status::kOk;
}

}  // namespace

bool IsValidUtf8(std::string_view utf8) {
  std::size_t byte = 0;
  std::uint32_t scalar = 0;
  while (byte < utf8.size()) if (!NextScalar(utf8, &byte, &scalar)) return false;
  return true;
}
bool IsValidLanguageId(std::string_view id) {
  if (id.size() < 2 || id.size() > kMaxLanguageIdBytes) return false;
  std::size_t begin_byte = 0;
  bool first = true;
  for (;;) {
    const auto dash_byte = id.find('-', begin_byte);
    const auto end_byte = dash_byte == std::string_view::npos ? id.size() : dash_byte;
    const auto segment_bytes = end_byte - begin_byte;
    if (segment_bytes < (first ? 2u : 1u) || segment_bytes > 8) return false;
    for (std::size_t byte = begin_byte; byte < end_byte; ++byte) {
      if (id[byte] >= 'a' && id[byte] <= 'z') continue;
      if (!first && id[byte] >= '0' && id[byte] <= '9') continue;
      return false;
    }
    if (dash_byte == std::string_view::npos) return true;
    begin_byte = dash_byte + 1;
    first = false;
  }
}
bool NormalizeLexemeUtf8(std::string_view utf8, std::string* out) {
  if (!out) return false;
  std::string normalized;
  if (utf8.empty() || utf8.size() > kMaxLexemeUtf8Bytes) { out->clear(); return false; }
  std::size_t byte = 0;
  std::uint32_t scalar = 0;
  while (byte < utf8.size()) {
    if (!NextScalar(utf8, &byte, &scalar) || ForbiddenScalar(scalar)) {
      out->clear(); return false;
    }
  }
  normalized.assign(utf8.data(), utf8.size());
  for (char& item : normalized) if (item >= 'A' && item <= 'Z') item += 'a' - 'A';
  *out = std::move(normalized);
  return true;
}
LexemeId StableLexemeId(std::string_view language, std::string_view normalized) {
  std::string checked;
  if (!IsValidLanguageId(language) || !NormalizeLexemeUtf8(normalized, &checked) ||
      checked != normalized) return 0;
  std::uint64_t hash = UINT64_C(14695981039346656037);
  const auto append = [&](std::string_view bytes) {
    for (const unsigned char byte : bytes) { hash ^= byte; hash *= UINT64_C(1099511628211); }
  };
  append("epocanva.prediction.lexeme/v1/"); append(language); append("/"); append(normalized);
  return hash == 0 ? 1 : hash;
}
const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kModelUnavailable: return "model-unavailable";
    case Status::kIoError: return "io-error";
    case Status::kInvalidFormat: return "invalid-format";
    case Status::kUnsupportedVersion: return "unsupported-version";
    case Status::kInvalidUtf8: return "invalid-utf8";
    case Status::kInvalidLexeme: return "invalid-lexeme";
    case Status::kDuplicateRecord: return "duplicate-record";
    case Status::kUnknownId: return "unknown-id";
    case Status::kCountOverflow: return "count-overflow";
    case Status::kInconsistentTotals: return "inconsistent-totals";
    case Status::kFingerprintMismatch: return "fingerprint-mismatch";
    case Status::kLimitExceeded: return "limit-exceeded";
    case Status::kTestFixtureDenied: return "test-fixture-denied";
    case Status::kInvalidContext: return "invalid-context";
    case Status::kInvalidOptions: return "invalid-options";
    case Status::kInvalidPersonalCounts: return "invalid-personal-counts";
    case Status::kBudgetExhausted: return "budget-exhausted";
    case Status::kBeamPruned: return "beam-pruned";
  }
  return "unknown-status";
}

struct Model::Impl {
  ModelMetadata metadata;
  std::vector<LexemeRecord> lexicon;
  std::set<LexemeId> ids;
  CountIndexes counts;
  std::vector<const LexemeRecord*> prefix_lexicon;
  std::vector<const LexemeRecord*> prefix_lexicon_by_language;
  PrefixOrdinals prefix_ordinals;
  PrefixOrdinals prefix_ordinals_by_language;

  void IndexPrefixes() {
    prefix_lexicon.reserve(lexicon.size());
    for (const auto& row : lexicon) prefix_lexicon.push_back(&row);
    std::sort(prefix_lexicon.begin(), prefix_lexicon.end(), [](const auto* left, const auto* right) {
      if (left->normalized_utf8 != right->normalized_utf8)
        return left->normalized_utf8 < right->normalized_utf8;
      return left->id < right->id;
    });
    prefix_ordinals.reserve(lexicon.size());
    for (std::size_t record = 0; record < prefix_lexicon.size(); ++record)
      prefix_ordinals.emplace(prefix_lexicon[record]->id, static_cast<RecordOrdinal>(record));
    const bool multilingual = std::any_of(lexicon.begin(), lexicon.end(), [&](const auto& row) {
      return row.language_id != lexicon.front().language_id;
    });
    if (multilingual) {
      prefix_lexicon_by_language = prefix_lexicon;
      std::sort(prefix_lexicon_by_language.begin(), prefix_lexicon_by_language.end(),
          [](const auto* left, const auto* right) {
            return std::tie(left->language_id, left->normalized_utf8, left->id) <
                   std::tie(right->language_id, right->normalized_utf8, right->id);
          });
      prefix_ordinals_by_language.reserve(lexicon.size());
      for (std::size_t record = 0; record < prefix_lexicon_by_language.size(); ++record)
        prefix_ordinals_by_language.emplace(prefix_lexicon_by_language[record]->id,
                                            static_cast<RecordOrdinal>(record));
    }
    counts.IndexPrefixes(prefix_ordinals, multilingual ? &prefix_ordinals_by_language : nullptr);
  }
  struct PrefixRange {
    RecordOrdinal begin_record = 0;
    RecordOrdinal end_record = 0;
    bool language_filtered = false;
  };
  PrefixRange ResolvePrefix(std::string_view prefix, std::string_view language) const {
    const bool language_filtered = !language.empty();
    const auto& ordered = language_filtered && !prefix_lexicon_by_language.empty() ?
        prefix_lexicon_by_language : prefix_lexicon;
    auto begin = ordered.begin(), end = ordered.end();
    if (language_filtered) {
      if (prefix_lexicon_by_language.empty()) {
        if (lexicon.front().language_id != language) return {0, 0, true};
      } else {
        begin = std::lower_bound(begin, end, language, [](const auto* row, std::string_view key) {
          return row->language_id < key;
        });
        end = std::upper_bound(begin, end, language, [](std::string_view key, const auto* row) {
          return key < row->language_id;
        });
      }
    }
    // Compare whole valid UTF-8 prefixes; no fabricated byte-successor string or
    // truncated scalar. Both interval bounds take logarithmic lexical lookups.
    begin = std::lower_bound(begin, end, prefix, [](const auto* row, std::string_view key) {
      return row->normalized_utf8.compare(0, key.size(), key) < 0;
    });
    end = std::upper_bound(begin, end, prefix, [](std::string_view key, const auto* row) {
      return row->normalized_utf8.compare(0, key.size(), key) > 0;
    });
    return {static_cast<RecordOrdinal>(begin - ordered.begin()),
            static_cast<RecordOrdinal>(end - ordered.begin()), language_filtered};
  }

  const LexemeRecord* Find(LexemeId id) const {
    const auto found = std::lower_bound(lexicon.begin(), lexicon.end(), id,
        [](const LexemeRecord& row, LexemeId key) { return row.id < key; });
    return found != lexicon.end() && found->id == id ? &*found : nullptr;
  }
  Context PrepareContext(const Context& input, bool* backed_off) const {
    Context out;
    for (const auto id : input) {
      if (id == 0 || ids.count(id) == 0) { out.clear(); *backed_off = true; }
      else out.push_back(id);
    }
    if (!out.empty() && !counts.Bigram(out.back())) *backed_off = true;
    if (out.size() == 2 && !counts.Trigram(out[0], out[1])) *backed_off = true;
    return out;
  }
  LinearScore StaticScore(const Context& context) const {
    LinearScore score;
    const long double denominator = static_cast<long double>(counts.unigram.total) +
                                    static_cast<long double>(lexicon.size()) + 1.0L;
    score.constant = 1.0L / denominator;
    score.terms.push_back({&counts.unigram, 1.0L / denominator});
    if (!context.empty()) score.Interpolate(counts.Bigram(context.back()), kBigramPrior);
    if (context.size() == 2)
      score.Interpolate(counts.Trigram(context[0], context[1]), kTrigramPrior);
    return score;
  }
  LinearScore WithPersonal(LinearScore score, const Context& context,
                           const CountIndexes* personal) const {
    if (!personal) return score;
    score.Interpolate(&personal->unigram, kPersonalPrior);
    if (!context.empty()) score.Interpolate(personal->Bigram(context.back()), kPersonalPrior);
    if (context.size() == 2)
      score.Interpolate(personal->Trigram(context[0], context[1]), kPersonalPrior);
    return score;
  }
  Status PreparePersonal(const PersonalCounts* personal, CountIndexes* out,
                         bool index_prefixes = false) const {
    if (!personal) return Status::kOk;
    std::size_t records = 0;
    for (const auto count : {personal->unigrams.size(), personal->bigrams.size(),
                             personal->trigrams.size()}) {
      if (count > kMaxPersonalRecords - records) return Status::kInvalidPersonalCounts;
      records += count;
    }
    if (BuildCounts(ids, personal->unigrams, personal->bigrams, personal->trigrams, out)
        != Status::kOk) return Status::kInvalidPersonalCounts;
    out->Rank();
    if (index_prefixes)
      out->IndexPrefixes(prefix_ordinals,
          prefix_ordinals_by_language.empty() ? nullptr : &prefix_ordinals_by_language);
    return Status::kOk;
  }

  struct Next { LexemeId id; double probability; };
  static bool BetterNext(const Next& a, const Next& b) {
    return a.probability != b.probability ? a.probability > b.probability : a.id < b.id;
  }
  std::vector<Next> TopNext(const Context& context, std::string_view prefix,
                            const PredictOptions& options, const CountIndexes* personal,
                            std::size_t wanted, bool allow_eos, PredictResult* result) const {
    const auto score = WithPersonal(StaticScore(context), context, personal);
    std::vector<std::size_t> cursors(score.terms.size(), 0);  // ranked record ordinals
    std::vector<PrefixCursor> prefix_cursors;
    if (!prefix.empty()) {
      const auto interval = ResolvePrefix(prefix, options.language_id);
      if (interval.begin_record == interval.end_record) return {};
      prefix_cursors.reserve(score.terms.size());
      for (const auto& term : score.terms)
        prefix_cursors.emplace_back(term.distribution->Prefix(interval.language_filtered),
                                    interval.begin_record, interval.end_record);
    }
    const auto exhausted = [&](std::size_t list) {
      return prefix.empty() ? cursors[list] == score.terms[list].distribution->ranked.size() :
                              prefix_cursors[list].empty();
    };
    const auto front = [&](std::size_t list) -> const RankedCount& {
      const auto record = prefix.empty() ? cursors[list] : prefix_cursors[list].Front();
      return score.terms[list].distribution->ranked[record];
    };
    std::unordered_set<LexemeId> seen;
    std::vector<Next> best;
    best.reserve(wanted);
    for (;;) {
      // The first list contains EVERY admitted outcome (all lexical IDs plus
      // EOS unfiltered, or all matching lexical IDs for a prefix), even unseen
      // count-zero words. Exhausting it proves no unscored outcome can remain.
      if (exhausted(0)) break;
      long double bound = score.constant;
      long double largest_contribution = -1.0L;
      std::size_t selected_list = score.terms.size();
      for (std::size_t list = 0; list < score.terms.size(); ++list) {
        const auto& term = score.terms[list];
        if (exhausted(list)) continue;
        const long double contribution = term.coefficient *
            static_cast<long double>(front(list).count);
        bound += contribution;
        if (contribution > largest_contribution) {
          largest_contribution = contribution;
          selected_list = list;
        }
      }
      // Conservative rounding guard. Strict comparison also preserves ID ties.
      const long double rounding_guard = 64.0L * std::numeric_limits<double>::epsilon() *
                                         (1.0L + std::fabs(bound));
      if (best.size() == wanted &&
          static_cast<long double>(best.back().probability) > bound + rounding_guard) break;
      if (options.max_index_entries != 0 &&
          result->index_entries_visited >= options.max_index_entries) {
        result->budget_exhausted = true;
        break;
      }
      if (selected_list == score.terms.size()) break;
      const auto id = front(selected_list).id;
      if (prefix.empty()) ++cursors[selected_list];
      else prefix_cursors[selected_list].Pop();
      ++result->index_entries_visited;
      if (!seen.insert(id).second) continue;
      if (id == 0) { if (!allow_eos || !prefix.empty()) continue; }
      else {
        const auto* row = Find(id);
        if (!options.language_id.empty() && row->language_id != options.language_id) continue;
        if (row->normalized_utf8.compare(0, prefix.size(), prefix) != 0) continue;
      }
      ++result->candidates_scored;
      Next candidate{id, score.Probability(id)};
      const auto where = std::lower_bound(best.begin(), best.end(), candidate, BetterNext);
      best.insert(where, candidate);
      if (best.size() > wanted) best.pop_back();
    }
    return best;
  }
};

Model::Model(std::unique_ptr<const Impl> impl) : impl_(std::move(impl)) {}
Model::~Model() = default;
std::shared_ptr<const Model> Model::Unavailable() {
  return std::shared_ptr<const Model>(new Model(std::make_unique<Impl>()));
}
bool Model::available() const { return impl_->metadata.source != SourceKind::kUnavailable; }
const ModelMetadata& Model::metadata() const { return impl_->metadata; }
const std::vector<LexemeRecord>& Model::lexemes() const { return impl_->lexicon; }
const LexemeRecord* Model::Find(LexemeId id) const { return impl_->Find(id); }
const LexemeRecord* Model::Find(std::string_view language, std::string_view normalized) const {
  const auto* row = Find(StableLexemeId(language, normalized));
  return row && row->language_id == language && row->normalized_utf8 == normalized ? row : nullptr;
}
std::uint64_t Model::UnigramCount(LexemeId id) const { return impl_->counts.unigram.Get(id); }

std::shared_ptr<const Model> Model::FromRecords(const ModelRecords& records, BuildInfo* info) {
  if (info) *info = {};
  const auto fail = [&](Status status) -> std::shared_ptr<const Model> {
    if (info) info->status = status;
    return nullptr;
  };
  try {
    const auto& meta = records.metadata;
    if (meta.version != kModelVersion || meta.stable_id_version != kStableIdVersion ||
        meta.normalization_id != kNormalizationId) return fail(Status::kUnsupportedVersion);
    if (meta.source == SourceKind::kUnavailable) return fail(Status::kModelUnavailable);
    if (meta.source != SourceKind::kTestFixture && meta.source != SourceKind::kProductionCorpus)
      return fail(Status::kInvalidFormat);
    if (!IsFingerprint(meta.source_fingerprint)) return fail(Status::kInvalidFormat);
    if (records.lexicon.size() >= static_cast<std::size_t>(kNoRecord))
      return fail(Status::kLimitExceeded);
    if (records.lexicon.empty() || records.lexicon.size() != meta.lexeme_count ||
        meta.unigram_total == 0) return fail(Status::kInconsistentTotals);
    auto impl = std::make_unique<Impl>();
    impl->metadata = meta;
    impl->lexicon = records.lexicon;
    for (const auto& row : impl->lexicon) {
      if (!IsValidUtf8(row.normalized_utf8)) return fail(Status::kInvalidUtf8);
      if (row.id == 0 || row.id != StableLexemeId(row.language_id, row.normalized_utf8) ||
          (row.kind != TokenKind::kWord && row.kind != TokenKind::kPunctuation))
        return fail(Status::kInvalidLexeme);
      if (!impl->ids.insert(row.id).second) return fail(Status::kDuplicateRecord);
    }
    std::sort(impl->lexicon.begin(), impl->lexicon.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto count_status = BuildCounts(impl->ids, records.unigrams, records.bigrams,
                                          records.trigrams, &impl->counts);
    if (count_status != Status::kOk) return fail(count_status);
    if (impl->counts.unigram.total != meta.unigram_total ||
        impl->counts.bigram_total != meta.bigram_total ||
        impl->counts.trigram_total != meta.trigram_total) return fail(Status::kInconsistentTotals);
    const auto marginal_status = CheckMarginals(impl->counts);
    if (marginal_status != Status::kOk) return fail(marginal_status);
    // Zero-observation catalog entries and EOS participate in exact global rank.
    impl->counts.unigram.counts.emplace(0, 0);
    for (const auto id : impl->ids) impl->counts.unigram.counts.emplace(id, 0);
    impl->counts.Rank();
    impl->IndexPrefixes();
    return std::shared_ptr<const Model>(new Model(std::move(impl)));
  } catch (const std::bad_alloc&) { return fail(Status::kLimitExceeded); }
    catch (const std::length_error&) { return fail(Status::kLimitExceeded); }
}

std::shared_ptr<const Model> Model::Load(const std::filesystem::path& path,
                                       const LoadOptions& options, BuildInfo* info) {
  if (info) *info = {};
  std::size_t line_ordinal = 0;
  const auto fail = [&](Status status) -> std::shared_ptr<const Model> {
    if (info) { info->status = status; info->line_ordinal = line_ordinal; }
    return nullptr;
  };
  try {
    if (!options.expected_source_fingerprint.empty() &&
        !IsFingerprint(options.expected_source_fingerprint)) return fail(Status::kInvalidFormat);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return fail(Status::kIoError);
    const auto file_end = file.tellg();
    if (file_end < 0) return fail(Status::kIoError);
    const auto file_bytes = static_cast<std::uintmax_t>(file_end);
    if (file_bytes > options.max_file_utf8_bytes ||
        file_bytes > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
      return fail(Status::kLimitExceeded);
    std::string bytes(static_cast<std::size_t>(file_bytes), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
      return fail(Status::kIoError);
    if (bytes.empty() || bytes.back() != '\n' || bytes.find('\r') != std::string::npos ||
        bytes.find('\0') != std::string::npos) return fail(Status::kInvalidFormat);
    if (!IsValidUtf8(bytes)) return fail(Status::kInvalidUtf8);
    Lines lines{bytes};
    std::string_view line;
    const auto header = [&](std::string_view key, std::string_view* value) {
      if (!lines.Next(&line)) return false;
      line_ordinal = lines.ordinal;
      const auto fields = Columns(line);
      if (fields.size() != 2 || fields[0] != key) return false;
      *value = fields[1];
      return true;
    };
    std::string_view value;
    if (!header("EPOCANVA_PREDICTION_TSV", &value)) return fail(Status::kInvalidFormat);
    if (value != "1") return fail(Status::kUnsupportedVersion);
    if (!header("id_scheme", &value)) return fail(Status::kInvalidFormat);
    if (value != kStableIdScheme) return fail(Status::kUnsupportedVersion);
    if (!header("normalization", &value)) return fail(Status::kInvalidFormat);
    if (value != kNormalizationId) return fail(Status::kUnsupportedVersion);
    ModelRecords records;
    if (!header("source", &value)) return fail(Status::kInvalidFormat);
    if (value == "test-fixture") {
      if (!options.allow_test_fixture) return fail(Status::kTestFixtureDenied);
      records.metadata.source = SourceKind::kTestFixture;
    } else if (value == "production-corpus") records.metadata.source = SourceKind::kProductionCorpus;
    else return fail(Status::kInvalidFormat);
    if (!header("source_sha256", &value) || !IsFingerprint(value)) return fail(Status::kInvalidFormat);
    records.metadata.source_fingerprint = std::string(value);
    if (!options.expected_source_fingerprint.empty()) {
      if (value != options.expected_source_fingerprint) return fail(Status::kFingerprintMismatch);
      if (info) info->source_fingerprint_matched = true;
    }
    std::string_view payload_fingerprint;
    if (!header("payload_sha256", &payload_fingerprint) || !IsFingerprint(payload_fingerprint))
      return fail(Status::kInvalidFormat);
    for (const auto& field : std::array<std::pair<const char*, Count*>, 4>{{
           {"lexemes", &records.metadata.lexeme_count},
           {"unigram_total", &records.metadata.unigram_total},
           {"bigram_total", &records.metadata.bigram_total},
           {"trigram_total", &records.metadata.trigram_total}}}) {
      if (!header(field.first, &value)) return fail(Status::kInvalidFormat);
      const auto status = Decimal(value, field.second);
      if (status != Status::kOk) return fail(status);
    }
    if (!lines.Next(&line) || line != "data") return fail(Status::kInvalidFormat);
    line_ordinal = lines.ordinal;
    if (Sha256(std::string_view(bytes).substr(lines.utf8_byte)) != payload_fingerprint)
      return fail(Status::kFingerprintMismatch);
    if (info) info->payload_fingerprint_verified = true;
    unsigned previous_group = 0;
    std::vector<Count> previous_tuple;
    while (lines.Next(&line)) {
      line_ordinal = lines.ordinal;
      const auto fields = Columns(line);
      if (fields.empty()) return fail(Status::kInvalidFormat);
      unsigned group = 0;
      std::size_t expected_fields = 0;
      std::size_t tuple_fields = 0;
      if (fields[0] == "L") { group=1; expected_fields=5; tuple_fields=1; }
      else if (fields[0] == "U") { group=2; expected_fields=3; tuple_fields=1; }
      else if (fields[0] == "B") { group=3; expected_fields=4; tuple_fields=2; }
      else if (fields[0] == "T") { group=4; expected_fields=5; tuple_fields=3; }
      else return fail(Status::kInvalidFormat);
      if (fields.size() != expected_fields || group < previous_group)
        return fail(Status::kInvalidFormat);
      std::vector<Count> tuple(tuple_fields);
      for (std::size_t ordinal = 0; ordinal < tuple_fields; ++ordinal) {
        const auto status = Decimal(fields[ordinal + 1], &tuple[ordinal]);
        if (status != Status::kOk) return fail(status);
      }
      if (group == previous_group && !(previous_tuple < tuple))
        return fail(previous_tuple == tuple ? Status::kDuplicateRecord : Status::kInvalidFormat);
      previous_group = group;
      previous_tuple = tuple;
      if (group == 1) {
        TokenKind kind;
        if (fields[3] == "word") kind = TokenKind::kWord;
        else if (fields[3] == "punct") kind = TokenKind::kPunctuation;
        else return fail(Status::kInvalidFormat);
        records.lexicon.push_back({tuple[0], std::string(fields[2]), std::string(fields[4]), kind});
      } else {
        Count count = 0;
        const auto status = Decimal(fields.back(), &count);
        if (status != Status::kOk) return fail(status);
        if (group == 2) records.unigrams.push_back({tuple[0], count});
        else if (group == 3) records.bigrams.push_back({tuple[0], tuple[1], count});
        else records.trigrams.push_back({tuple[0], tuple[1], tuple[2], count});
      }
    }
    BuildInfo semantic;
    auto model = FromRecords(records, &semantic);
    if (!model) { line_ordinal = 0; return fail(semantic.status); }
    if (info) { info->status = Status::kOk; info->line_ordinal = 0; }
    return model;
  } catch (const std::bad_alloc&) { return fail(Status::kLimitExceeded); }
    catch (const std::length_error&) { return fail(Status::kLimitExceeded); }
    catch (const std::ios_base::failure&) { return fail(Status::kIoError); }
}

ScoreResult Model::Score(const Context& context, LexemeId id,
                         const PersonalCounts* personal) const {
  ScoreResult result;
  if (!available()) return result;
  if (context.size() > 2) { result.status = Status::kInvalidContext; return result; }
  if (id != 0 && !Find(id)) { result.status = Status::kUnknownId; return result; }
  CountIndexes prepared;
  result.status = impl_->PreparePersonal(personal, &prepared);
  if (result.status != Status::kOk) return result;
  const auto known_context = impl_->PrepareContext(context, &result.context_backed_off);
  const auto static_score = impl_->StaticScore(known_context);
  result.static_probability = static_score.Probability(id);
  const auto score = impl_->WithPersonal(static_score, known_context, personal ? &prepared : nullptr);
  result.probability = score.Probability(id);
  result.log_probability = std::log(result.probability);
  result.personal_applied = score.terms.size() != static_score.terms.size();
  return result;
}

PredictResult Model::Predict(std::optional<std::string_view> prefix_utf8,
                             const Context& context, const PredictOptions& options,
                             const PersonalCounts* personal) const {
  PredictResult result;
  if (!available()) return result;
  if (context.size() > 2) { result.status = Status::kInvalidContext; return result; }
  if (options.max_suggestions > kMaxSuggestions || options.max_words == 0 ||
      options.max_words > kMaxContinuationWords || options.beam_width == 0 ||
      options.beam_width > kMaxSuggestions ||
      (!options.language_id.empty() && !IsValidLanguageId(options.language_id))) {
    result.status = Status::kInvalidOptions; return result;
  }
  std::string prefix;
  if (prefix_utf8 && !prefix_utf8->empty()) {
    if (!IsValidUtf8(*prefix_utf8)) { result.status = Status::kInvalidUtf8; return result; }
    if (!NormalizeLexemeUtf8(*prefix_utf8, &prefix)) {
      result.status = Status::kInvalidLexeme; return result;
    }
  }
  CountIndexes prepared;
  result.personal_preparations = personal ? 1 : 0;
  result.status = impl_->PreparePersonal(personal, &prepared,
      !prefix.empty() && options.max_suggestions != 0);
  if (result.status != Status::kOk) return result;
  const auto known_context = impl_->PrepareContext(context, &result.context_backed_off);
  if (options.max_suggestions == 0) { result.complete = true; return result; }
  const auto* personal_index = personal ? &prepared : nullptr;
  const auto first = impl_->TopNext(known_context, prefix, options, personal_index,
      options.max_words == 1 ? options.max_suggestions : options.beam_width + 1,
      false, &result);
  struct Path { Suggestion suggestion; Context context; };
  const auto better_path = [](const Path& a, const Path& b) {
    if (a.suggestion.log_probability != b.suggestion.log_probability)
      return a.suggestion.log_probability > b.suggestion.log_probability;
    if (a.suggestion.lexeme_ids != b.suggestion.lexeme_ids)
      return a.suggestion.lexeme_ids < b.suggestion.lexeme_ids;
    return a.suggestion.ended_with_eos < b.suggestion.ended_with_eos;
  };
  const auto extend = [&](Path path, const Impl::Next& next) {
    path.suggestion.log_probability += std::log(next.probability);
    if (next.id == 0) {
      path.suggestion.terminal = true;
      path.suggestion.ended_with_eos = true;
    } else {
      const auto* row = Find(next.id);
      path.suggestion.lexeme_ids.push_back(next.id);
      path.suggestion.tokens_utf8.push_back(row->normalized_utf8);
      path.suggestion.terminal = row->kind == TokenKind::kPunctuation;
      if (path.context.size() == 2) path.context.erase(path.context.begin());
      path.context.push_back(next.id);
    }
    return path;
  };
  std::vector<Path> beam;
  for (const auto& next : first) beam.push_back(extend({{}, known_context}, next));
  if (options.max_words > 1 && beam.size() > options.beam_width) {
    beam.resize(options.beam_width); result.beam_pruned = true;
  }
  for (std::size_t word_count = 1; word_count < options.max_words; ++word_count) {
    std::vector<Path> expanded;
    for (const auto& path : beam) {
      if (path.suggestion.terminal) { expanded.push_back(path); continue; }
      const auto successors = impl_->TopNext(path.context, {}, options, personal_index,
                                             options.beam_width + 1, true, &result);
      for (const auto& next : successors) expanded.push_back(extend(path, next));
    }
    if (expanded.empty()) break;
    std::sort(expanded.begin(), expanded.end(), better_path);
    if (expanded.size() > options.beam_width) {
      expanded.resize(options.beam_width); result.beam_pruned = true;
    }
    beam = std::move(expanded);
  }
  std::sort(beam.begin(), beam.end(), better_path);
  if (beam.size() > options.max_suggestions) beam.resize(options.max_suggestions);
  for (auto& path : beam) result.suggestions.push_back(std::move(path.suggestion));
  result.complete = !result.budget_exhausted && !result.beam_pruned;
  result.status = result.budget_exhausted ? Status::kBudgetExhausted :
                  result.beam_pruned ? Status::kBeamPruned : Status::kOk;
  return result;
}

}  // namespace prediction
}  // namespace epocanva
