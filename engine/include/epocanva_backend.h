#ifndef EPOCANVA_BACKEND_H_
#define EPOCANVA_BACKEND_H_

#include "epocanva_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPOCANVA_BACKEND_ABI_VERSION 1u

typedef uint64_t epocanva_backend_handle;
typedef uint64_t epocanva_backend_session_handle;

enum {
  EPOCANVA_BACKEND_EN = 1,
  EPOCANVA_BACKEND_ZH = 2,
  EPOCANVA_BACKEND_FR = 3,
  EPOCANVA_BACKEND_JA = 4,
  EPOCANVA_BACKEND_KO = 5,
  /* Additive identity only. Spanish and German are core-required by the product
   * spec, but no engine or admitted resource exists for them yet, so the backend
   * rejects these values at open. Never treat the constant as support. */
  EPOCANVA_BACKEND_ES = 6,
  EPOCANVA_BACKEND_DE = 7
};
enum {
  EPOCANVA_BACKEND_LATIN = 1,
  EPOCANVA_BACKEND_PINYIN = 2,
  EPOCANVA_BACKEND_ZHUYIN = 3,
  EPOCANVA_BACKEND_ROMAJI = 4,
  EPOCANVA_BACKEND_KATAKANA = 5,
  EPOCANVA_BACKEND_TWO_SET = 6
};
enum {
  EPOCANVA_BACKEND_COMPLETION = 1,
  /* Word prediction for an empty current token. Terminal punctuation is excluded;
   * use NEXT_UNIT when a punctuation token is also an acceptable result. */
  EPOCANVA_BACKEND_NEXT_WORD = 2,
  EPOCANVA_BACKEND_CONTINUATION = 3
};
enum {
  EPOCANVA_BACKEND_RAW = 0,
  EPOCANVA_BACKEND_WORD = 1,
  EPOCANVA_BACKEND_SPELLING = 2,
  EPOCANVA_BACKEND_MORPHOLOGY = 3,
  EPOCANVA_BACKEND_EXPANSION = 4,
  EPOCANVA_BACKEND_CONVERSION = 5,
  EPOCANVA_BACKEND_PREDICTION = 6
};
enum {
  EPOCANVA_BACKEND_UNAVAILABLE = -101,
  EPOCANVA_BACKEND_INVALID_RESOURCE = -102,
  EPOCANVA_BACKEND_STALE_COMMIT = -103,
  EPOCANVA_BACKEND_LEARNING_FAILED = -104
};

/* One model/catalog per language. Paths are absolute UTF-8, with an exact SHA-256
 * supplied by the reviewed resource manifest. No download or directory search.
 * A missing resource does not become a supported language with an empty menu.
 */
typedef struct epocanva_backend_resource {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint32_t language;
  uint32_t reserved;
  epocanva_utf8_view model_path;
  epocanva_utf8_view model_sha256;
  epocanva_utf8_view conversion_path;
  epocanva_utf8_view conversion_sha256;
  epocanva_utf8_view auxiliary_path; /* French .dic paired with conversion .aff */
  epocanva_utf8_view auxiliary_sha256;
} epocanva_backend_resource;

typedef struct epocanva_backend_options {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  const epocanva_backend_resource* resources;
  uint32_t resource_count; /* resource records */
  uint32_t allow_test_fixture; /* explicit testing only, default 0 */
  /* Optional existing Rime runtime configuration for Chinese schema sessions.
   * Copied at open; never borrowed. NULL means no Rime conversion capability.
   */
  const epocanva_engine_options* rime_options;
  epocanva_utf8_view learning_root;
} epocanva_backend_options;

typedef struct epocanva_backend_session_options {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  uint32_t language;
  uint32_t input_mode;
  uint32_t learning_enabled;
  uint32_t reserved;
} epocanva_backend_session_options;

typedef struct epocanva_backend_request {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  uint32_t learning_enabled; /* checked afresh, not inferred from session open */
  uint32_t reserved;
} epocanva_backend_request;

typedef struct epocanva_backend_query {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  uint32_t learning_enabled;
  uint32_t query_kind;
  uint32_t max_candidates; /* candidate records, 1..64 */
  uint32_t max_continuation_words; /* lexical records, 1..4 */
  uint64_t search_budget; /* evaluated search states, not text bytes */
} epocanva_backend_query;

typedef struct epocanva_backend_snapshot {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t snapshot_token;
  uint64_t context_target;
  uint64_t generation;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span raw_input;
  epocanva_utf8_span preedit;
  uint64_t caret_utf8_byte_offset; /* relative to preedit */
  uint32_t candidate_count;
  uint32_t search_complete;
  uint32_t language;
  uint32_t input_mode;
} epocanva_backend_snapshot;

typedef struct epocanva_backend_candidate {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t snapshot_token;
  uint64_t candidate_token;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span text;
  uint32_t kind;
  uint32_t lexeme_count; /* at most 8; zero IDs means no learning */
  uint64_t lexeme_ids[8];
  double score; /* log-model + match features, not a probability; an exact
                   resource abbreviation precedes inferred candidates */
  uint64_t input_begin_utf8_byte;
  uint64_t input_end_utf8_byte;
} epocanva_backend_candidate;

typedef struct epocanva_backend_commit {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t commit_token;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span text;
  epocanva_utf8_span remaining_preedit;
  uint32_t awaiting_ack;
  uint32_t reserved;
} epocanva_backend_commit;

typedef struct epocanva_backend_ack_result {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint32_t consumed;
  uint32_t learned;
  uint32_t context_advanced;
  uint32_t persistence_status;
} epocanva_backend_ack_result;

/* Same ownership/thread rules as the engine ABI: owner-thread operations, caller
 * buffers, no exceptions/allocators across the boundary, no content diagnostics.
 * Every operation checks current context and privacy. The caller is responsible
 * for obtaining those facts from its current field and for a valid permission
 * lock throughout each call that queries personal data or acknowledges learning.
 * The supplied context value is stable only under that caller-owned lock; it is
 * not a callback, OS grant or independently verified field classification.
 */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_open(
    const epocanva_backend_options* options, epocanva_backend_handle* out_backend) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_close(
    epocanva_backend_handle backend) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_session_open(
    epocanva_backend_handle backend, const epocanva_backend_session_options* options,
    epocanva_backend_session_handle* out_session) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_session_close(
    epocanva_backend_session_handle session) EPOCANVA_ENGINE_NOEXCEPT;

/* SetInput replaces only this session's uncommitted raw input. PushAscii feeds
 * a composing keyboard mode. Neither reads a host document nor records history.
 */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_set_input(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_utf8_view input) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_push_ascii(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    uint32_t ascii_byte) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_backspace(
    epocanva_backend_session_handle session, const epocanva_backend_request* request) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_cancel(
    epocanva_backend_session_handle session, const epocanva_backend_request* request) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_query_candidates(
    epocanva_backend_session_handle session, const epocanva_backend_query* request) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_snapshot(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_backend_snapshot* out_snapshot, char* buffer, uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_candidate(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    uint64_t snapshot_token, uint32_t candidate_ordinal,
    epocanva_backend_candidate* out_candidate, char* buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_select(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    uint64_t snapshot_token, uint64_t candidate_token) EPOCANVA_ENGINE_NOEXCEPT;
/* Complete copying enables acknowledgement, but does not learn or advance context. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_commit(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_backend_commit* out_commit, char* buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_ack_commit(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    uint64_t commit_token, uint32_t applied,
    epocanva_backend_ack_result* out_result) EPOCANVA_ENGINE_NOEXCEPT;

/* Additive v2 interfaces leave every v1 layout and entry point unchanged. */
#define EPOCANVA_BACKEND_PROFILE_ABI_VERSION 2u
enum {
  EPOCANVA_BACKEND_KEYBOARD_NEUTRAL = 0,
  EPOCANVA_BACKEND_KEYBOARD_US_QWERTY = 1,
  EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY = 2,
  EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ = 3,
  EPOCANVA_BACKEND_KEYBOARD_DVORAK = 4
};
enum {
  EPOCANVA_BACKEND_OUTPUT_LATIN = 1,
  EPOCANVA_BACKEND_OUTPUT_HANZI = 2,
  EPOCANVA_BACKEND_OUTPUT_HIRAGANA = 3,
  EPOCANVA_BACKEND_OUTPUT_KATAKANA = 4,
  EPOCANVA_BACKEND_OUTPUT_KANJI = 5,
  EPOCANVA_BACKEND_OUTPUT_HANGUL = 6
};
enum {
  EPOCANVA_BACKEND_KANA = 7,
  /* One model lexical token, NOT one byte, Unicode scalar or grapheme cluster.
   * Unlike NEXT_WORD this admits terminal punctuation because it is a lexical unit. */
  EPOCANVA_BACKEND_NEXT_UNIT = 4,
  EPOCANVA_BACKEND_UNIT_LEXICAL_TOKEN = 1,
  EPOCANVA_BACKEND_INVALID_PROFILE = -105
};

typedef struct epocanva_backend_profile {
  uint32_t struct_size_bytes;
  uint32_t abi_version; /* EPOCANVA_BACKEND_PROFILE_ABI_VERSION */
  uint32_t profile_id; /* nonzero, unique within one backend */
  uint32_t language;
  uint32_t input_mode;
  uint32_t output_mode;
  uint32_t keyboard_layout;
  uint32_t reserved;
  epocanva_utf8_view variant_id;
  epocanva_utf8_view abbreviation_path;
  epocanva_utf8_view abbreviation_sha256;
} epocanva_backend_profile;

/* Multiple profiles may share a language but bind different model catalogs. */
typedef struct epocanva_backend_profile_resource {
  epocanva_backend_profile profile;
  epocanva_backend_resource resource; /* unchanged v1 resource */
} epocanva_backend_profile_resource;

typedef struct epocanva_backend_profile_options {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  const epocanva_backend_profile_resource* profiles;
  uint32_t profile_count; /* resource records, 1..32 */
  uint32_t allow_test_fixture;
  const epocanva_engine_options* rime_options;
  epocanva_utf8_view learning_root;
} epocanva_backend_profile_options;

typedef struct epocanva_backend_profile_snapshot {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_backend_snapshot snapshot; /* caller initializes its v1 header */
  uint32_t profile_id;
  uint32_t output_mode;
  uint32_t keyboard_layout;
  uint32_t unit_kind; /* NEXT_UNIT is a complete model lexical token */
} epocanva_backend_profile_snapshot;

/* A monotonic caller event token counts confirmed edits, not text units.
 * Confirmation updates transient context only. Ordinary unselected text is NOT
 * persisted; learning remains exclusive to admitted candidates and commit ACK.
 * Sensitive/Unknown confirmation discards transient text and never persists.
 */
typedef struct epocanva_backend_text_confirmation {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_backend_request request; /* unchanged v1 request */
  uint64_t event_token;
  epocanva_utf8_view text; /* one confirmed word/phrase, <=4096 UTF-8 bytes */
} epocanva_backend_text_confirmation;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_open_profiles(
    const epocanva_backend_profile_options* options,
    epocanva_backend_handle* out_backend) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_profile_session_open(
    epocanva_backend_handle backend, const epocanva_backend_profile* profile,
    const epocanva_backend_session_options* options,
    epocanva_backend_session_handle* out_session) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_push_utf8(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_utf8_view text) EPOCANVA_ENGINE_NOEXCEPT;
/* Replaces transient preceding context; no history log or persistence. The
 * caller must supply confirmed text from the current field under permission.
 * Invalidates the old candidate menu and refuses while commit ACK is pending. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_set_context(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_utf8_view preceding_text) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_confirm_text(
    epocanva_backend_session_handle session,
    const epocanva_backend_text_confirmation* confirmation,
    epocanva_backend_ack_result* out_result) EPOCANVA_ENGINE_NOEXCEPT;
epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_profile_snapshot(
    epocanva_backend_session_handle session, const epocanva_backend_request* request,
    epocanva_backend_profile_snapshot* out_snapshot, char* buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif
