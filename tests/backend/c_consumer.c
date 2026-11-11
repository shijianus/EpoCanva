#include "epocanva_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INIT(value, version) do { memset(&(value), 0, sizeof(value)); (value).struct_size_bytes = sizeof(value); (value).abi_version = version; } while (0)
static int failures = 0;
static void check(int condition, const char* message) {
  if (!condition) { fprintf(stderr, "FAIL %s\n", message); ++failures; }
}
static epocanva_utf8_view view(const char* text) {
  epocanva_utf8_view result = {text, (uint64_t)strlen(text)};
  return result;
}
static int candidates(epocanva_backend_session_handle session, epocanva_backend_request* request,
                      uint32_t kind, const char* wanted) {
  epocanva_backend_query query;
  epocanva_backend_snapshot snapshot;
  epocanva_backend_candidate candidate;
  uint32_t ordinal;
  int found = 0;
  char bytes[8192];
  INIT(query, 1); query.context = request->context; query.learning_enabled = request->learning_enabled;
  query.query_kind = kind; query.max_candidates = 64; query.max_continuation_words = 1;
  check(epocanva_backend_query_candidates(session, &query) == EPOCANVA_OK, "C query");
  INIT(snapshot, 1);
  check(epocanva_backend_read_snapshot(session, request, &snapshot, bytes, sizeof(bytes)) == EPOCANVA_OK, "C snapshot");
  for (ordinal = 0; ordinal < snapshot.candidate_count; ++ordinal) {
    INIT(candidate, 1);
    check(epocanva_backend_read_candidate(session, request, snapshot.snapshot_token, ordinal,
        &candidate, bytes, sizeof(bytes)) == EPOCANVA_OK, "C candidate");
    if (candidate.text.utf8_byte_count == strlen(wanted) &&
        memcmp(bytes + candidate.text.utf8_byte_offset, wanted, strlen(wanted)) == 0) found = 1;
  }
  return found;
}
int main(int argc, char** argv) {
  epocanva_backend_profile_resource resources[2];
  epocanva_backend_profile_options options;
  epocanva_backend_session_options session_options;
  epocanva_backend_request request;
  epocanva_backend_text_confirmation confirmation;
  epocanva_backend_ack_result ack;
  epocanva_backend_profile_snapshot snapshot;
  epocanva_backend_handle backend = 0;
  epocanva_backend_session_handle first = 0, second = 0;
  epocanva_backend_profile unsupported;
  char digest[65] = {0}, bytes[8192];
  FILE* hash_file;
  unsigned ordinal;
  if (argc == 2 && strcmp(argv[1], "--force-failure") == 0) { check(0, "deliberate C assertion"); return 1; }
  if (argc != 3) return 2;
  hash_file = fopen(argv[2], "rb");
  if (!hash_file || fread(digest, 1, 64, hash_file) != 64) return 2;
  fclose(hash_file);
  memset(resources, 0, sizeof(resources));
  for (ordinal = 0; ordinal < 2; ++ordinal) {
    INIT(resources[ordinal].profile, 2);
    resources[ordinal].profile.profile_id = ordinal + 1;
    resources[ordinal].profile.language = EPOCANVA_BACKEND_EN;
    resources[ordinal].profile.input_mode = EPOCANVA_BACKEND_LATIN;
    resources[ordinal].profile.output_mode = EPOCANVA_BACKEND_OUTPUT_LATIN;
    resources[ordinal].profile.keyboard_layout = ordinal ? EPOCANVA_BACKEND_KEYBOARD_DVORAK : EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
    resources[ordinal].profile.variant_id = view("en-US");
    INIT(resources[ordinal].resource, 1);
    resources[ordinal].resource.language = EPOCANVA_BACKEND_EN;
    resources[ordinal].resource.model_path = view(argv[1]);
    resources[ordinal].resource.model_sha256 = view(digest);
  }
  INIT(options, 2); options.profiles = resources; options.profile_count = 2; options.allow_test_fixture = 1;
  check(epocanva_backend_open_profiles(&options, &backend) == EPOCANVA_OK && backend, "C multi-profile open");
  if (!backend) return 1;
  INIT(session_options, 1); session_options.language = EPOCANVA_BACKEND_EN; session_options.input_mode = EPOCANVA_BACKEND_LATIN;
  session_options.context.context_target = 101; session_options.context.generation = 1;
  session_options.context.privacy_class = EPOCANVA_PRIVACY_NORMAL; session_options.learning_enabled = 1;
  check(epocanva_backend_profile_session_open(backend, &resources[0].profile, &session_options, &first) == EPOCANVA_OK, "C first profile session");
  session_options.context.context_target = 102;
  check(epocanva_backend_profile_session_open(backend, &resources[1].profile, &session_options, &second) == EPOCANVA_OK, "C second profile session");
  INIT(request, 1); request.context = session_options.context; request.context.context_target = 101; request.learning_enabled = 1;
  check(epocanva_backend_push_utf8(first, &request, view("g")) == EPOCANVA_OK, "C UTF8 push");
  check(epocanva_backend_push_utf8(first, &request, view("m")) == EPOCANVA_OK, "C UTF8 append");
  check(candidates(first, &request, EPOCANVA_BACKEND_COMPLETION, "good morning"), "C model-derived abbreviation absent from table");
  INIT(snapshot, 2); INIT(snapshot.snapshot, 1);
  check(epocanva_backend_read_profile_snapshot(first, &request, &snapshot, bytes, sizeof(bytes)) == EPOCANVA_OK &&
      snapshot.profile_id == 1 && snapshot.keyboard_layout == 1, "C profile snapshot identity");
  check(epocanva_backend_cancel(first, &request) == EPOCANVA_OK, "C cancel");
  INIT(confirmation, 2); confirmation.request = request; confirmation.event_token = 1; confirmation.text = view("good");
  INIT(ack, 1);
  check(epocanva_backend_confirm_text(first, &confirmation, &ack) == EPOCANVA_OK && ack.consumed && ack.context_advanced && !ack.learned,
      "C confirmed ordinary text advances without persistence");
  check(candidates(first, &request, EPOCANVA_BACKEND_NEXT_UNIT, "morning"), "C next lexical unit after confirmed text");
  check(epocanva_backend_confirm_text(first, &confirmation, &ack) == EPOCANVA_STALE_SNAPSHOT, "C duplicate confirmation stale");
  check(epocanva_backend_set_context(first, &request, view("hello")) == EPOCANVA_OK, "C replace context");
  check(candidates(first, &request, EPOCANVA_BACKEND_NEXT_WORD, "world"), "C imported context predicts");
  check(epocanva_backend_push_utf8(first, &request, view("\xC0\xAF")) == EPOCANVA_INVALID_UTF8, "C rejects malformed UTF8");
  check(epocanva_backend_set_context(first, &request, view("\xED\xA0\x80")) == EPOCANVA_INVALID_UTF8, "C rejects surrogate encoding");
  confirmation.event_token = 2; confirmation.request.context.privacy_class = EPOCANVA_PRIVACY_SENSITIVE;
  check(epocanva_backend_confirm_text(first, &confirmation, &ack) == EPOCANVA_PRIVACY_DENIED && ack.consumed && !ack.learned, "C sensitive clears context");
  unsupported = resources[0].profile; unsupported.variant_id = view("en-unsupported");
  {
    epocanva_backend_session_handle refused = 99;
    check(epocanva_backend_profile_session_open(backend, &unsupported, &session_options, &refused) == EPOCANVA_BACKEND_UNAVAILABLE && !refused,
        "C unavailable variant does not fake support");
  }
  check(epocanva_backend_close(backend) == EPOCANVA_BUSY, "C active sessions prevent close");
  check(epocanva_backend_session_close(first) == EPOCANVA_OK, "C first close");
  check(epocanva_backend_session_close(second) == EPOCANVA_OK, "C second close");
  check(epocanva_backend_close(backend) == EPOCANVA_OK, "C backend close");
  if (failures) return 1;
  puts("PASS pure C backend profiles and prediction");
  return 0;
}
