# Candidate identity and the unified candidate contract

**Status**: internal contract implemented (`engine/candidates`); public C ABI
unchanged. Proposed additive ABI extension is design-only.

## 1. What exists

`engine/candidates/candidate_contract.{h,cc}` defines one record every candidate
source can describe:

| Field | Meaning | Units |
| --- | --- | --- |
| `identity.language_tag` | lowercase primary subtag (`en`, `zh`, `fr`, `ja`, `ko`) | UTF-8 code units (bytes) |
| `identity.scheme_id` | input scheme (`latin`, `pinyin`, `zhuyin`, `romaji`, `kana`, `two_set`, `katakana`) | UTF-8 code units (bytes) |
| `identity.language` / `identity.input_mode` | the ABI enum values that produced the candidate | enum value |
| `identity.lexeme_ids` | content-derived catalog IDs admitted by the loaded model | record count (0..8) |
| `identity.normalized_identity` | language-specific bounded surface; English folds ASCII A-Z only | UTF-8 code units (bytes) |
| `text_utf8` | the exact surface the user would commit | UTF-8 code units (bytes) |
| `source` | raw / word / spelling / morphology / expansion / conversion / prediction (`EPOCANVA_BACKEND_*` values) | enum value |
| `match` | raw / prefix / fuzzy / abbreviation / context / conversion | enum value |
| `commit` | insert at caret vs replace current input | enum value |
| `availability` | available / unverified / unavailable | enum value |
| `score` | existing model/match score; not a probability | `double` |
| `source_confidence` | `0` means no calibrated confidence is claimed | `[0,1]` |
| `learnable` | true only with admitted lexeme IDs from a non-raw source | boolean |
| `input_begin_utf8_byte` / `input_end_utf8_byte` | match range in the session's raw input | UTF-8 code units (bytes) |
| `stable_id` | canonical identity string | UTF-8 code units (bytes) |

The backend adapter (`engine/backend/backend_api.cc`) fills this record for
every candidate before `Publish`. An invalid record refuses the query rather
than publishing an identity the learning layer cannot round-trip.

`Validate` checks the record's own invariants: legal ASCII tag/scheme strings,
admitted ABI language/input-mode values, finite score, confidence in `[0,1]`,
bounded admitted lexeme IDs, raw-never-learnable, source/match/commit
consistency, and the recomputed stable ID. It does **not** carry the session's
raw input, so it can only check `begin <= end`. `backend_session::Session::Publish`
independently enforces `end <= raw.size()` and a UTF-8 code-unit boundary at both
ends immediately before publication; the backend profile tests assert those
boundaries through the public ABI.

## 2. Stable ID

```
cand:v1:<language>:<scheme>:lex:<id>[+<id>...]     (learnable)
cand:v1:<language>:<scheme>:text:<fnv1a64-hex>     (non-learnable)
```

- Language and scheme are part of the ID, so identical surfaces in different
  languages or schemes cannot collide.
- Learnable candidates use content-derived catalog lexeme IDs. They never use a
  menu token, an ordinal, or a hash of display text.
- Non-learnable candidates (raw fallback) use FNV-1a-64 over
  `epocanva.candidate.identity/v1/<language>/<scheme>/<normalized>`; the result
  `0` maps to `1`. These IDs are stable for a fixed normalization but are **not**
  learning identities and must not be persisted.
- Source is metadata, not identity: the same admitted lexeme from a lexicon and
  from prediction has one ID.
- Keyboard layout is not part of candidate identity: US-QWERTY, Dvorak, QWERTZ
  and AZERTY change how a key reaches the same English scheme, not the lexical
  word. The profile/binding manifest still keeps those configurations distinct.
- The contract's `scheme_id` is the **ABI input mode** (`latin`, `pinyin`,
  `zhuyin`, `romaji`, `kana`, `two_set`, `katakana`), while the locale manifest's
  `scheme_id` is the owning profile's declared scheme. These differ for the
  Korean native-Hangul binding: profile `ko-KR-two-set` declares scheme
  `two_set`, but binding 10 uses ABI input mode `LATIN`. The contract therefore
  labels its candidates `ko/latin`, distinct from binding 5's `ko/two_set`,
  which is intentional: two input modes are two candidate identities. The
  manifest is a profile/resource catalogue, not the candidate identity
  namespace.
- Model/dictionary updates keep IDs stable only when the lexeme text and the
  model's ID scheme stay the same. A model rebuilt with a different ID scheme
  requires a catalog change, which invalidates the old learning namespace rather
  than silently remapping counts.

### Schema v3 compatibility

Learning schema v3 persists `candidate_id` scoped by catalog namespace and model
SHA-256. The learning key remains exactly `lex:<id>[+<id>...]`
(`LearningCandidateId`), so existing files, migrations and rollbacks are
unchanged. `cand:v1:` IDs are a public/record identity, not a storage key.

### What is deliberately not claimed

- No Unicode NFC/NFD equivalence: composed and decomposed forms are distinct and
  the test states that.
- No full Unicode case folding: only English ASCII A-Z is folded.
- No 64-bit collision proof: `cand:v1:` is the identity string; the FNV value is
  only the non-learning fallback component. A consumer must not truncate it to
  `uint64`.
- No cross-model ID continuity when the model's identity scheme changes.
- `language_tag` is the primary subtag today. The backend only admits
  `zh-Hans-CN` and `yue-Hant-HK` has no engine, so no collision exists in the
  current catalogue. Before any second Chinese script/region engine is bound,
  the ID namespace must use the full canonical profile tag (for example
  `zh-Hans-CN` vs `zh-Hant-TW`), not the primary subtag alone.

## 3. Public ABI extension (design only)

The v1 `epocanva_backend_candidate` layout is frozen. A later additive read
function would serialize the contract:

```c
#define EPOCANVA_BACKEND_CANDIDATE_IDENTITY_ABI_VERSION 1u
typedef struct epocanva_backend_candidate_identity {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t snapshot_token;
  uint64_t candidate_token;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span stable_id;
  epocanva_utf8_span language_tag;
  epocanva_utf8_span scheme_id;
  uint32_t language;              /* EPOCANVA_BACKEND_EN..KO */
  uint32_t input_mode;            /* EPOCANVA_BACKEND_LATIN..KANA */
  uint32_t source;                /* existing EPOCANVA_BACKEND_* kind */
  uint32_t match;
  uint32_t commit;
  uint32_t availability;
  uint32_t learnable;
  uint32_t lexeme_count;          /* 0..8 */
  uint64_t lexeme_ids[8];
  uint64_t input_begin_utf8_byte;
  uint64_t input_end_utf8_byte;
  double score;
  double source_confidence;
  uint64_t reserved[4];
} epocanva_backend_candidate_identity;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_candidate_identity(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    uint64_t snapshot_token, uint32_t candidate_ordinal,
    epocanva_backend_candidate_identity* out, char* buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;
```

Rules: the new struct starts with `struct_size_bytes`/`abi_version`; existing
functions and layouts do not change; a smaller struct is rejected with
`EPOCANVA_STRUCT_TOO_SMALL`; unknown tail fields are ignored. The function is
read-only, owner-thread-only, privacy-gated, and returns
`EPOCANVA_BUFFER_TOO_SMALL` for sizing. Rollback is deleting the caller's use of
the new function; no persisted format changes.

## 4. Tests

- `tests/candidate_contract` (3 CTests): ABI kind and language/input-mode value
  mirror checks against `epocanva_backend.h`; normalization, fixed FNV vectors,
  cross-language/scheme distinctness, lexeme-vs-text namespace separation,
  learning-key compatibility, source/match/commit mapping, learnable flag, span
  ordering, NaN/±Infinity score and confidence rejection, out-of-range ABI
  identity rejection, tamper rejection, and a falsifiability control.
- `backend.profiles.real_acceptance` exercises the contract for all five
  languages because every published candidate passes through it. The same test
  asserts the ABI-visible raw-input UTF-8 boundaries.
- `backend_session.validation` independently covers publication-time range,
  boundary, finite-score and admitted-ID rejection.
