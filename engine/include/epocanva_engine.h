#ifndef EPOCANVA_ENGINE_H_
#define EPOCANVA_ENGINE_H_

/* Pure C ABI, version 1. No Rime, Win32, C++ or allocator types cross this header.
 * All text is strict UTF-8. Text counts/offsets are UTF-8 code units (bytes), NOT
 * UTF-16 code units, grapheme clusters or caret/screen-cell columns.
 * Structure sizes are storage bytes. Candidate/page ordinals count entries/pages,
 * not text. Callers use the platform's ordinary alignment (no packing pragma).
 *
 * Each request/result starts with struct_size_bytes + abi_version. Zero the whole
 * structure, then set these to sizeof(the_structure) and EPOCANVA_ENGINE_ABI_VERSION.
 * Version 0 is NOT a default. A larger v1 structure is accepted; its tail is untouched.
 * Nested views/spans/context and opaque handles are fixed-layout v1 value types.
 * Non-null pointers must address valid caller-owned memory for the entire call;
 * buffers must not overlap each other or request/result structures.
 *
 * Engine/session operations are synchronous and owner-thread-only. An engine's
 * opening thread owns all its sessions. No callbacks or worker threads are created.
 * Handles are process-local opaque tokens, not pointers or Rime session IDs. Never
 * manufacture or persist one. Zero is invalid; closed tokens are never reused.
 */
#include <stdint.h>

#define EPOCANVA_ENGINE_ABI_VERSION 1u
#define EPOCANVA_NO_UTF8_BYTE_OFFSET UINT64_MAX
#define EPOCANVA_NO_CANDIDATE_ORDINAL UINT32_MAX

#if defined(_WIN32)
#define EPOCANVA_ENGINE_CALL __cdecl
#else
#define EPOCANVA_ENGINE_CALL
#endif
#ifdef __cplusplus
#define EPOCANVA_ENGINE_NOEXCEPT noexcept
extern "C" {
#else
#define EPOCANVA_ENGINE_NOEXCEPT
#endif

typedef uint64_t epocanva_engine_handle;
typedef uint64_t epocanva_session_handle;
typedef int32_t epocanva_status;
enum {
  EPOCANVA_OK = 0,
  EPOCANVA_NO_COMMIT = 1,
  EPOCANVA_INVALID_ARGUMENT = -1,
  EPOCANVA_STRUCT_TOO_SMALL = -2,
  EPOCANVA_ABI_VERSION_MISMATCH = -3,
  EPOCANVA_INVALID_UTF8 = -4,
  EPOCANVA_PRIVACY_DENIED = -5,
  EPOCANVA_INVALID_HANDLE = -6,
  EPOCANVA_WRONG_THREAD = -7,
  EPOCANVA_BUFFER_TOO_SMALL = -8,
  EPOCANVA_CONTEXT_MISMATCH = -9,
  EPOCANVA_GENERATION_MISMATCH = -10,
  EPOCANVA_STALE_SNAPSHOT = -11,
  EPOCANVA_OUT_OF_RANGE = -12,
  EPOCANVA_BUSY = -13,
  EPOCANVA_BACKEND_ERROR = -14,
  EPOCANVA_SESSION_FAULTED = -15,
  EPOCANVA_OUT_OF_MEMORY = -16,
  EPOCANVA_INTERNAL_ERROR = -17,
  EPOCANVA_BACKEND_VERSION_MISMATCH = -18
};

enum {
  EPOCANVA_PRIVACY_UNKNOWN = 0,
  EPOCANVA_PRIVACY_NORMAL = 1,
  EPOCANVA_PRIVACY_SENSITIVE = 2
};

typedef struct epocanva_utf8_view {
  const char* data;
  uint64_t utf8_byte_count; /* Explicit count, excluding any C terminator. */
} epocanva_utf8_view;

typedef struct epocanva_utf8_span {
  uint64_t utf8_byte_offset; /* From the beginning of the supplied output buffer. */
  uint64_t utf8_byte_count;  /* No terminator is stored or included. */
} epocanva_utf8_span;

typedef struct epocanva_context {
  /* Nonzero caller-assigned, content-free identity. Never an app/field name.
   * The caller must revalidate actual focus before dispatch AND before host edits;
   * the engine cannot inspect or certify the host's focused context. */
  uint64_t context_target;
  uint64_t generation;
  uint32_t privacy_class; /* Only NORMAL is admitted, including on read operations. */
  uint32_t reserved;      /* Must be zero. */
} epocanva_context;

typedef struct epocanva_engine_options {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  /* Absolute strict-UTF-8 paths; directories must already exist. No path search,
   * directory creation or implicit deployment. The caller must verify the DLL
   * against its pinned build/package manifest before opening it. */
  epocanva_utf8_view rime_dll_path;
  epocanva_utf8_view shared_data_dir;
  epocanva_utf8_view user_data_dir;
  epocanva_utf8_view log_dir;
  /* Required exact version, checked against the loaded runtime. Not a hash check. */
  epocanva_utf8_view expected_rime_version;
  /* Optional EXPLICIT installer/test preparation, performed once at engine_open.
   * Each absolute schema path must be directly inside shared_data_dir. Normal
   * runtime callers pass NULL/0 and use previously compiled data. */
  const epocanva_utf8_view* deployment_schema_paths;
  uint32_t deployment_schema_count; /* Number of path-view entries. */
  uint32_t reserved;
} epocanva_engine_options;

typedef struct epocanva_session_options {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  epocanva_utf8_view schema_id; /* Strict UTF-8, then the backend's safe ASCII ID rule. */
} epocanva_session_options;

typedef struct epocanva_session_request {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
} epocanva_session_request;

typedef struct epocanva_key_request {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  /* X11 keysym, NOT a native virtual key. Printable ASCII equals its scalar value;
   * Unicode keysyms use 0x01000000 | scalar. The caller does layout/shortcut policy.
   * Case/shift translation must not be applied twice. One request makes at most
   * one engine key call; OS repeat counts are not expanded here. */
  uint32_t keysym;
  /* X11/Rime bit mask, defined by the backend; unsupported bits are rejected.
   * This low-level operation does not replace the host's DecideKey policy. */
  uint32_t modifiers;
} epocanva_key_request;

typedef struct epocanva_candidate_request {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  epocanva_context context;
  uint64_t snapshot_version;
  uint32_t candidate_ordinal_on_page; /* Zero-based entry ordinal, never text offset. */
  uint32_t reserved;
} epocanva_candidate_request;

typedef struct epocanva_operation_result {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  /* Set BEFORE entering a mutating backend operation. If an exception or a
   * follow-up observation fails, a true value forbids replaying that request.
   * For cancel it denotes a close/recreate attempt, not a key call. */
  uint32_t backend_call_made;
  uint32_t backend_reported_handled; /* For selection: backend reported accepted. */
  uint32_t state_available;
  uint32_t commit_pending;
  uint64_t snapshot_version;
  uint64_t context_target;
  uint64_t generation;
} epocanva_operation_result;

typedef struct epocanva_snapshot {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t snapshot_version; /* Process-unique, invalidated before every mutation. */
  uint64_t context_target;
  uint64_t generation;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span schema_id;
  epocanva_utf8_span schema_name;
  epocanva_utf8_span raw_input;
  epocanva_utf8_span preedit;
  /* Offsets into preedit, NOT into raw_input or the concatenated output buffer.
   * NO_UTF8_BYTE_OFFSET means absent. Invalid boundaries are never rounded. */
  uint64_t cursor_utf8_byte_offset;
  uint64_t selection_start_utf8_byte_offset;
  uint64_t selection_end_utf8_byte_offset;
  uint32_t is_composing;
  uint32_t service_disabled;
  uint32_t candidate_count_on_page;
  uint32_t highlighted_candidate_ordinal_on_page;
  uint32_t menu_page_ordinal;
  uint32_t menu_page_capacity; /* Number of candidate entries, not text bytes. */
  uint32_t is_last_page;
  /* Session event availability, not part of backend snapshot identity: this may
   * clear after successful read_commit without changing snapshot_version. */
  uint32_t commit_pending;
} epocanva_snapshot;

typedef struct epocanva_candidate {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t snapshot_version;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span text;
  epocanva_utf8_span comment;
} epocanva_candidate;

typedef struct epocanva_commit {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t context_target;
  uint64_t generation;
  uint64_t snapshot_version; /* Mutation that produced this pending event. */
  uint64_t required_utf8_bytes;
  epocanva_utf8_span text;
} epocanva_commit;

typedef struct epocanva_engine_info {
  uint32_t struct_size_bytes;
  uint32_t abi_version;
  uint64_t required_utf8_bytes;
  epocanva_utf8_span loaded_dll_path;
  epocanva_utf8_span rime_version;
  uint32_t initialized;
  uint32_t english_module_loaded;
  uint64_t initialize_calls;
  uint64_t finalize_calls;
  uint64_t live_session_count;
  uint64_t create_session_calls;
  uint64_t destroy_session_calls;
  uint64_t process_key_calls;
  uint64_t select_candidate_calls;
  uint64_t status_acquired;
  uint64_t status_freed;
  uint64_t context_acquired;
  uint64_t context_freed;
  uint64_t commit_acquired;
  uint64_t commit_freed;
  uint64_t schema_config_acquired;
  uint64_t schema_config_closed;
} epocanva_engine_info;

uint32_t EPOCANVA_ENGINE_CALL epocanva_engine_abi_version(void) EPOCANVA_ENGINE_NOEXCEPT;
/* Returns a static text-free ASCII label; never an exception message or user text. */
const char* EPOCANVA_ENGINE_CALL epocanva_status_name(epocanva_status status) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_open(
    const epocanva_engine_options* options,
    epocanva_engine_handle* out_engine) EPOCANVA_ENGINE_NOEXCEPT;
/* Refuses live sessions. On failure the token remains valid for explicit cleanup;
 * terminal backend finalize/unload failures are NOT retried by the backend. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_close(
    epocanva_engine_handle engine) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_read_info(
    epocanva_engine_handle engine, epocanva_engine_info* out_info,
    char* utf8_buffer, uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_open(
    epocanva_engine_handle engine, const epocanva_session_options* options,
    epocanva_session_handle* out_session) EPOCANVA_ENGINE_NOEXCEPT;
/* Cleanup is permitted without a privacy grant; never commits pending input. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_close(
    epocanva_session_handle session) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_process_key(
    epocanva_session_handle session, const epocanva_key_request* request,
    epocanva_operation_result* out_result) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_select_candidate(
    epocanva_session_handle session, const epocanva_candidate_request* request,
    epocanva_operation_result* out_result) EPOCANVA_ENGINE_NOEXCEPT;

/* Explicit discard: closes and recreates the REAL backend session in the same
 * schema, clears cached state/commit, advances generation by one, and returns the
 * new identity. Does not simulate Escape or commit text. A failed recreation
 * faults the token until close. Sensitive/Unknown callers use close instead. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_cancel(
    epocanva_session_handle session, const epocanva_session_request* request,
    epocanva_operation_result* out_result) EPOCANVA_ENGINE_NOEXCEPT;

/* Queries copy a cached immutable observation, without ANY process_key/select or
 * get_commit call. Query with NULL/0 to learn required_utf8_bytes; on
 * BUFFER_TOO_SMALL, metadata is filled but NO output buffer byte is written.
 * On success exactly required_utf8_bytes are copied, with NO NUL terminators.
 * Memory belongs to the caller; there is no cross-CRT free operation.
 * All counts remain stable until a mutation, cancel or close on this session. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_snapshot(
    epocanva_session_handle session, const epocanva_session_request* request,
    epocanva_snapshot* out_snapshot, char* utf8_buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;

epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_candidate(
    epocanva_session_handle session, const epocanva_candidate_request* request,
    epocanva_candidate* out_candidate, char* utf8_buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;

/* process_key/select capture one backend commit into ABI-owned pending storage.
 * This call consumes it ONLY on successful complete copying, not on sizing or
 * BUFFER_TOO_SMALL. NO_COMMIT means no pending event. While a commit is pending,
 * further process/select calls return BUSY; read it, explicitly cancel, or close.
 * A returned commit is not evidence that a host document applied it. */
epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_commit(
    epocanva_session_handle session, const epocanva_session_request* request,
    epocanva_commit* out_commit, char* utf8_buffer,
    uint64_t capacity_utf8_bytes) EPOCANVA_ENGINE_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* EPOCANVA_ENGINE_H_ */
