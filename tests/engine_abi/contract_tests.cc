#include "epocanva_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

// Test-only allocation faults exercise the actual C boundary, without a fake
// engine/session/API table or a production test switch. DLL allocations use its
// own CRT; these faults are confined to the executable/static-library side.
namespace { std::atomic<int> allocation_fault{0}; }
void* operator new(std::size_t storage_bytes) {
  const int fault = allocation_fault.exchange(0);
  if (fault == 1) throw std::bad_alloc();
  if (fault == 2) throw 42;
  if (void* memory = std::malloc(storage_bytes == 0 ? 1 : storage_bytes)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t storage_bytes) { return ::operator new(storage_bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
namespace fs = std::filesystem;
int checks = 0;
int failures = 0;
void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL %s\n", label);
  }
}
void Status(epocanva_status actual, epocanva_status expected, const char* label) {
  ++checks;
  if (actual != expected) {
    ++failures;
    std::printf("FAIL %s got=%s expected=%s\n", label,
                epocanva_status_name(actual), epocanva_status_name(expected));
  }
}
template <class T> T Structure() {
  T value{};
  value.struct_size_bytes = static_cast<std::uint32_t>(sizeof(T));
  value.abi_version = EPOCANVA_ENGINE_ABI_VERSION;
  return value;
}
epocanva_utf8_view View(const std::string& value) { return {value.data(), value.size()}; }
epocanva_utf8_view Literal(const char* value) { return {value, std::strlen(value)}; }
epocanva_context Context(std::uint64_t target = 11, std::uint64_t generation = 1) {
  return {target, generation, EPOCANVA_PRIVACY_NORMAL, 0};
}
epocanva_session_request Request(epocanva_context context) {
  auto request = Structure<epocanva_session_request>();
  request.context = context;
  return request;
}
epocanva_key_request Key(epocanva_context context, std::uint32_t keysym) {
  auto key = Structure<epocanva_key_request>();
  key.context = context;
  key.keysym = keysym;
  return key;
}
struct Observation {
  epocanva_snapshot snapshot = Structure<epocanva_snapshot>();
  std::vector<char> bytes;
};
Observation Read(epocanva_session_handle session, epocanva_context context) {
  Observation result;
  const auto request = Request(context);
  Status(epocanva_session_read_snapshot(session, &request, &result.snapshot, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "snapshot sizing query");
  if (result.snapshot.required_utf8_bytes > 1024 * 1024) {
    Check(false, "snapshot capacity is bounded");
    return result;
  }
  result.bytes.resize(static_cast<std::size_t>(result.snapshot.required_utf8_bytes));
  Status(epocanva_session_read_snapshot(session, &request, &result.snapshot,
                                       result.bytes.data(), result.bytes.size()),
         EPOCANVA_OK, "snapshot full copy");
  return result;
}
std::string Field(const std::vector<char>& bytes, epocanva_utf8_span span) {
  if (span.utf8_byte_offset > bytes.size() ||
      span.utf8_byte_count > bytes.size() - span.utf8_byte_offset) {
    Check(false, "output span stays within caller buffer UTF-8 bytes");
    return {};
  }
  if (span.utf8_byte_count == 0) return {};
  return std::string(bytes.data() + static_cast<std::size_t>(span.utf8_byte_offset),
                     static_cast<std::size_t>(span.utf8_byte_count));
}
epocanva_engine_info Info(epocanva_engine_handle engine,
                           const std::string* expected_path = nullptr) {
  auto result = Structure<epocanva_engine_info>();
  Status(epocanva_engine_read_info(engine, &result, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "runtime info sizing");
  if (result.required_utf8_bytes > 65536) {
    Check(false, "runtime info capacity bounded");
    return result;
  }
  std::vector<char> bytes(static_cast<std::size_t>(result.required_utf8_bytes));
  Status(epocanva_engine_read_info(engine, &result, bytes.data(), bytes.size()),
         EPOCANVA_OK, "runtime info copy");
  Check(Field(bytes, result.rime_version) == "1.17.0", "actual pinned runtime version");
  if (expected_path != nullptr) {
    Check(fs::equivalent(fs::u8path(Field(bytes, result.loaded_dll_path)),
                         fs::u8path(*expected_path)), "actual loaded DLL matches requested file");
  }
  return result;
}
bool SameCalls(const epocanva_engine_info& left, const epocanva_engine_info& right) {
  return left.create_session_calls == right.create_session_calls &&
         left.destroy_session_calls == right.destroy_session_calls &&
         left.process_key_calls == right.process_key_calls &&
         left.select_candidate_calls == right.select_candidate_calls &&
         left.status_acquired == right.status_acquired &&
         left.context_acquired == right.context_acquired &&
         left.commit_acquired == right.commit_acquired &&
         left.schema_config_acquired == right.schema_config_acquired;
}
void Balanced(const epocanva_engine_info& info) {
  Check(info.status_acquired == info.status_freed, "status acquisition/free pairing");
  Check(info.context_acquired == info.context_freed, "context acquisition/free pairing");
  Check(info.commit_acquired == info.commit_freed, "commit acquisition/free pairing");
  Check(info.schema_config_acquired == info.schema_config_closed, "schema acquisition/close pairing");
  Check(info.create_session_calls == info.destroy_session_calls, "session create/destroy pairing");
  Check(info.live_session_count == 0, "all real sessions closed");
}
epocanva_session_handle OpenSession(epocanva_engine_handle engine, epocanva_context context,
                                     const char* schema = "epocanva_test_english") {
  auto options = Structure<epocanva_session_options>();
  options.context = context;
  options.schema_id = Literal(schema);
  epocanva_session_handle session = 0;
  Status(epocanva_session_open(engine, &options, &session), EPOCANVA_OK, "open real session");
  Check(session != 0, "session handle is nonzero");
  return session;
}
epocanva_operation_result Process(epocanva_session_handle session, epocanva_context context,
                                    std::uint32_t keysym) {
  auto key = Key(context, keysym);
  auto result = Structure<epocanva_operation_result>();
  Status(epocanva_session_process_key(session, &key, &result), EPOCANVA_OK, "real process_key");
  Check(result.backend_call_made == 1 && result.state_available == 1,
        "process_key call and observation witnesses");
  return result;
}
void Type(epocanva_session_handle session, epocanva_context context, const char* ascii_fixture) {
  for (const unsigned char* byte = reinterpret_cast<const unsigned char*>(ascii_fixture);
       *byte != 0; ++byte) {
    const auto result = Process(session, context, *byte);
    Check(result.backend_reported_handled == 1, "fixture key handled by actual engine");
  }
}
epocanva_candidate_request Selection(epocanva_context context,
                                      const epocanva_snapshot& snapshot, std::uint32_t ordinal) {
  auto request = Structure<epocanva_candidate_request>();
  request.context = context;
  request.snapshot_version = snapshot.snapshot_version;
  request.candidate_ordinal_on_page = ordinal;
  return request;
}
std::string Candidate(epocanva_session_handle session, epocanva_candidate_request request,
                      std::string* comment = nullptr) {
  auto candidate = Structure<epocanva_candidate>();
  Status(epocanva_session_read_candidate(session, &request, &candidate, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "candidate sizing");
  if (candidate.required_utf8_bytes > 65536) {
    Check(false, "candidate capacity bounded");
    return {};
  }
  std::vector<char> bytes(static_cast<std::size_t>(candidate.required_utf8_bytes));
  Status(epocanva_session_read_candidate(session, &request, &candidate, bytes.data(), bytes.size()),
         EPOCANVA_OK, "candidate copy");
  if (comment != nullptr) *comment = Field(bytes, candidate.comment);
  return Field(bytes, candidate.text);
}
void CopyFixture(const fs::path& source, const fs::path& dest) {
  fs::create_directories(dest.parent_path());
  fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
}
struct Inputs {
  std::string mode, dll, shared, user, log;
  std::vector<std::string> schema_paths;
  std::vector<epocanva_utf8_view> deployment;
  epocanva_engine_options options = Structure<epocanva_engine_options>();
};
void Prepare(Inputs* input, const fs::path& repo, const fs::path& run_root) {
  const auto relative = run_root.lexically_normal().lexically_relative((repo / "build").lexically_normal());
  if (!repo.is_absolute() || !run_root.is_absolute() || relative.empty() ||
      *relative.begin() == ".." || *relative.begin() == ".")
    throw std::invalid_argument("run-root must be below repository build/<preset>");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path run = run_root / ("run-" + std::to_string(stamp));
  const auto shared = run / fs::u8path(u8"shared-資料");
  const auto user = run / "user";
  const auto log = run / "log";
  for (const auto& dir : {shared, user, log}) fs::create_directories(dir);
  const auto fixtures = repo / "tests" / "fixtures";
  CopyFixture(fixtures / "english_completion/default.yaml", shared / "default.yaml");
  CopyFixture(fixtures / "english_completion/epocanva_test_english.schema.yaml",
              shared / "epocanva_test_english.schema.yaml");
  CopyFixture(fixtures / "english_completion/lexicon.tsv", shared / "lexicon/epocanva_test_lexicon.tsv");
  CopyFixture(fixtures / "english_spelling/epocanva_spell_english.schema.yaml",
              shared / "epocanva_spell_english.schema.yaml");
  CopyFixture(fixtures / "english_spelling/lexicon_spell.tsv", shared / "lexicon/epocanva_spell_lexicon.tsv");
  // Real Rime table-translator fixture, not an ABI backend or language feature.
  // Its multibyte preedit and supplementary scalar prove the UTF-8 byte contract.
  std::ofstream schema(shared / "abi_unicode.schema.yaml", std::ios::binary);
  schema << u8"schema:\n  schema_id: abi_unicode\n  name: ABI 邊界 probe\n  version: '1'\n"
            "menu:\n  page_size: 5\nengine:\n  processors: [speller, fluid_editor]\n"
            "  segmentors: [abc_segmentor, fallback_segmentor]\n  translators: [table_translator]\n"
            "speller:\n  alphabet: 'a'\n  delimiter: ''\ntranslator:\n  dictionary: abi_unicode\n"
            "  enable_user_dict: false\n  enable_completion: false\n"
            "  preedit_format:\n    - 'xform/^a$/文😀/'\n";
  schema.close();
  std::ofstream dictionary(shared / "abi_unicode.dict.yaml", std::ios::binary);
  dictionary << u8"---\nname: abi_unicode\nversion: '1'\nsort: original\n...\n文😀\ta\t1\n";
  dictionary.close();
  if (!schema || !dictionary) throw std::runtime_error("fixture write failed");
  input->shared = shared.u8string();
  input->user = user.u8string();
  input->log = log.u8string();
  input->schema_paths = {(shared / "epocanva_test_english.schema.yaml").u8string(),
                         (shared / "epocanva_spell_english.schema.yaml").u8string(),
                         (shared / "abi_unicode.schema.yaml").u8string()};
  for (const auto& path : input->schema_paths) input->deployment.push_back(View(path));
  input->options.rime_dll_path = View(input->dll);
  input->options.shared_data_dir = View(input->shared);
  input->options.user_data_dir = View(input->user);
  input->options.log_dir = View(input->log);
  input->options.expected_rime_version = Literal("1.17.0");
  input->options.deployment_schema_paths = input->deployment.data();
  input->options.deployment_schema_count = static_cast<std::uint32_t>(input->deployment.size());
}
int Finish() {
  std::printf("checks=%d failures=%d\n", checks, failures);
  if (failures != 0) return 5;
  std::puts("RESULT PASS");
  return 0;
}
int DeliberatelyExpectSuccess(epocanva_status observed) {
  if (failures != 0) return 5; // A broken setup must NOT count as a working fail control.
  if (observed == EPOCANVA_OK) {
    std::puts("RESULT PASS invalid request accepted (control failed to detect rejection)");
    return 0;
  }
  std::printf("FAIL deliberately expected acceptance; actual=%s\n", epocanva_status_name(observed));
  return 14;
}

void Contracts(epocanva_engine_handle engine, Inputs& input) {
  auto info = Info(engine, &input.dll);
  Check(info.initialized == 1 && info.english_module_loaded == 1 && info.initialize_calls == 1,
        "existing runtime initialized once and English module really loaded");
  epocanva_engine_handle second = 0;
  Status(epocanva_engine_open(&input.options, &second), EPOCANVA_BUSY,
         "second runtime is refused by shared process slot");
  Check(second == 0, "refused runtime leaves no token");

  epocanva_context context = Context();
  auto options = Structure<epocanva_session_options>();
  options.context = context;
  options.schema_id = Literal("epocanva_test_english");
  const auto before_invalid = Info(engine);
  epocanva_session_handle invalid = 88;
  options.abi_version = 0;
  Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_ABI_VERSION_MISMATCH,
         "missing session ABI version rejected");
  Check(invalid == 0, "rejected session clears output token");
  options.abi_version = EPOCANVA_ENGINE_ABI_VERSION + 1;
  Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_ABI_VERSION_MISMATCH,
         "future incompatible ABI version rejected");
  options.abi_version = EPOCANVA_ENGINE_ABI_VERSION;
  options.struct_size_bytes = 4;
  Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_STRUCT_TOO_SMALL,
         "truncated structure rejected before later fields");
  options.struct_size_bytes = sizeof(options);
  for (const std::string& malformed : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                      std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)}) {
    options.schema_id = View(malformed);
    Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_INVALID_UTF8,
           "malformed UTF-8 refused without replacement");
  }
  const std::string embedded_nul("safe\0suffix", 11);
  options.schema_id = View(embedded_nul);
  Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_INVALID_ARGUMENT,
         "embedded NUL identifier refused");
  options.schema_id = Literal("epocanva_test_english");
  for (const std::uint32_t privacy : {0u, 2u, 999u}) {
    options.context.privacy_class = privacy;
    Status(epocanva_session_open(engine, &options, &invalid), EPOCANVA_PRIVACY_DENIED,
           "unknown/sensitive/unrecognized privacy cannot create session");
  }
  Check(SameCalls(before_invalid, Info(engine)), "all invalid opens make zero Rime calls");

  const auto session = OpenSession(engine, context);
  const auto other = OpenSession(engine, Context(22));
  Status(epocanva_engine_close(engine), EPOCANVA_BUSY, "runtime cannot close under live sessions");
  const auto before_keys = Info(engine);
  Type(session, context, "hel");
  auto observation = Read(session, context);
  Check(Field(observation.bytes, observation.snapshot.raw_input) == "hel", "real raw input exact");
  Check(Field(observation.bytes, observation.snapshot.preedit) == "hel", "real preedit exact");
  Check(observation.snapshot.cursor_utf8_byte_offset == 3, "ASCII cursor is 3 UTF-8 bytes");
  Check(observation.snapshot.candidate_count_on_page == 4, "real English completion menu count");
  const auto after_keys = Info(engine);
  Check(after_keys.process_key_calls == before_keys.process_key_calls + 3,
        "three requests made exactly three real process_key calls");
  const char* expected[] = {"hel", "hello", "help", "helmet"};
  for (std::uint32_t ordinal = 0; ordinal < 4; ++ordinal)
    Check(Candidate(session, Selection(context, observation.snapshot, ordinal)) == expected[ordinal],
          "candidate is copied from real English translator");
  const auto other_observation = Read(other, Context(22));
  Check(other_observation.snapshot.is_composing == 0, "sessions remain independent");
  Check(SameCalls(after_keys, Info(engine)), "cached state/candidate reads make zero engine calls");

  auto request = Request(context);
  auto snapshot = Structure<epocanva_snapshot>();
  std::vector<char> guard(static_cast<std::size_t>(observation.snapshot.required_utf8_bytes + 2), 'Z');
  Status(epocanva_session_read_snapshot(session, &request, &snapshot, guard.data() + 1,
                                        observation.snapshot.required_utf8_bytes - 1),
         EPOCANVA_BUFFER_TOO_SMALL, "short snapshot buffer refused");
  Check(std::all_of(guard.begin(), guard.end(), [](char byte) { return byte == 'Z'; }),
        "short snapshot buffer writes no partial text");
  Status(epocanva_session_read_snapshot(session, &request, &snapshot, guard.data() + 1,
                                        observation.snapshot.required_utf8_bytes),
         EPOCANVA_OK, "exact snapshot capacity accepted");
  Check(guard.front() == 'Z' && guard.back() == 'Z', "snapshot does not append a terminator");
  struct ExtendedSnapshot { epocanva_snapshot known; std::uint64_t tail; } extended{};
  extended.known = Structure<epocanva_snapshot>();
  extended.known.struct_size_bytes = sizeof(extended);
  extended.tail = 0x55aa11223344ull;
  Status(epocanva_session_read_snapshot(session, &request, &extended.known, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "larger same-version structure accepted");
  Check(extended.tail == 0x55aa11223344ull && extended.known.struct_size_bytes == sizeof(extended),
        "future structure tail and caller size remain untouched");

  const auto before_denial = Info(engine);
  auto selection = Selection(context, observation.snapshot, 1);
  auto operation = Structure<epocanva_operation_result>();
  auto key = Key(context, 'x');
  auto candidate = Structure<epocanva_candidate>();
  auto commit = Structure<epocanva_commit>();
  for (const std::uint32_t privacy : {0u, 2u}) {
    key.context.privacy_class = privacy;
    selection.context.privacy_class = privacy;
    request.context.privacy_class = privacy;
    Status(epocanva_session_process_key(session, &key, &operation), EPOCANVA_PRIVACY_DENIED,
           "dynamic privacy blocks key after Normal admission");
    Check(operation.backend_call_made == 0, "privacy denial never enters mutation");
    Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_PRIVACY_DENIED,
           "dynamic privacy blocks candidate selection");
    Status(epocanva_session_read_snapshot(session, &request, &snapshot, guard.data(), guard.size()),
           EPOCANVA_PRIVACY_DENIED, "privacy blocks snapshot read");
    Status(epocanva_session_read_candidate(session, &selection, &candidate, guard.data(), guard.size()),
           EPOCANVA_PRIVACY_DENIED, "privacy blocks candidate read");
    Status(epocanva_session_read_commit(session, &request, &commit, guard.data(), guard.size()),
           EPOCANVA_PRIVACY_DENIED, "privacy blocks commit read");
    Status(epocanva_session_cancel(session, &request, &operation), EPOCANVA_PRIVACY_DENIED,
           "privacy blocks session recreation; close remains available");
  }
  request.context = context;
  selection.context = context;
  key.context = context;
  selection.context.context_target++;
  Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_CONTEXT_MISMATCH,
         "candidate cannot cross context target");
  selection.context = context;
  selection.context.generation++;
  Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_GENERATION_MISMATCH,
         "candidate cannot cross generation");
  selection.context = context;
  selection.candidate_ordinal_on_page = 1000;
  Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_OUT_OF_RANGE,
         "candidate ordinal range is checked");
  selection.candidate_ordinal_on_page = 1;
  auto cross_session = selection;
  cross_session.context = Context(22);
  Status(epocanva_session_select_candidate(other, &cross_session, &operation), EPOCANVA_STALE_SNAPSHOT,
         "process-unique snapshot token cannot cross sessions");
  key.abi_version = 0;
  Status(epocanva_session_process_key(session, &key, &operation), EPOCANVA_ABI_VERSION_MISMATCH,
         "missing key ABI version blocked");
  key.abi_version = EPOCANVA_ENGINE_ABI_VERSION;
  key.keysym = 0x0100d800;
  Status(epocanva_session_process_key(session, &key, &operation), EPOCANVA_INVALID_ARGUMENT,
         "surrogate Unicode keysym refused");
  key.keysym = 'x';
  key.modifiers = 0x80000000u;
  Status(epocanva_session_process_key(session, &key, &operation), EPOCANVA_INVALID_ARGUMENT,
         "unsupported modifier bit refused");
  Check(SameCalls(before_denial, Info(engine)), "privacy/freshness/ABI/keysym denials make zero Rime calls");

  std::thread wrong_thread([&]() {
    auto thread_request = Request(context);
    auto thread_snapshot = Structure<epocanva_snapshot>();
    auto thread_result = Structure<epocanva_operation_result>();
    auto thread_key = Key(context, 'x');
    Status(epocanva_session_process_key(session, &thread_key, &thread_result), EPOCANVA_WRONG_THREAD,
           "wrong-thread key refused");
    Status(epocanva_session_read_snapshot(session, &thread_request, &thread_snapshot, nullptr, 0),
           EPOCANVA_WRONG_THREAD, "wrong-thread snapshot refused");
    Status(epocanva_session_close(session), EPOCANVA_WRONG_THREAD, "wrong-thread close refused");
    Status(epocanva_engine_close(engine), EPOCANVA_WRONG_THREAD, "wrong-thread runtime close refused");
  });
  wrong_thread.join();
  Check(SameCalls(before_denial, Info(engine)), "wrong-thread attempts make zero Rime calls");

  Process(session, context, 'l');
  const auto before_stale = Info(engine);
  Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_STALE_SNAPSHOT,
         "old candidate snapshot rejected after next key");
  Check(operation.backend_call_made == 0, "stale selection does not enter backend");
  Check(SameCalls(before_stale, Info(engine)), "stale candidate zero-call witness");

  Status(epocanva_session_cancel(session, &request, &operation), EPOCANVA_OK, "cancel closes/recreates real session");
  Check(operation.generation == context.generation + 1 && operation.backend_call_made == 1,
        "cancel advances generation and reports actual backend operation");
  context.generation = operation.generation;
  Status(epocanva_session_read_snapshot(session, &request, &snapshot, nullptr, 0),
         EPOCANVA_GENERATION_MISMATCH, "pre-cancel identity invalidated");
  request.context = context;
  observation = Read(session, context);
  Check(observation.snapshot.is_composing == 0 && observation.snapshot.commit_pending == 0,
        "cancel clears composition and pending commit");

  Type(session, context, "hel");
  observation = Read(session, context);
  selection = Selection(context, observation.snapshot, 1);
  Status(epocanva_session_select_candidate(session, &selection, &operation), EPOCANVA_OK,
         "current candidate accepted by real engine");
  Check(operation.backend_call_made == 1 && operation.backend_reported_handled == 1,
        "selection has real accepted witness");
  // The current English schema selects into preedit, then Space commits it.
  observation = Read(session, context);
  Check(Field(observation.bytes, observation.snapshot.preedit) == "hello", "selected completion preedit");
  operation = Process(session, context, ' ');
  Check(operation.commit_pending == 1, "actual English space commit is pending");
  const auto before_read = Info(engine);
  observation = Read(session, context);
  Status(epocanva_session_read_commit(session, &request, &commit, nullptr, 0),
         EPOCANVA_BUFFER_TOO_SMALL, "commit sizing retains event");
  Check(commit.required_utf8_bytes == 6, "completion plus space is 6 UTF-8 bytes");
  std::vector<char> commit_bytes(7, 'Z');
  Status(epocanva_session_read_commit(session, &request, &commit, commit_bytes.data(), 5),
         EPOCANVA_BUFFER_TOO_SMALL, "short commit copy retains event");
  Check(std::all_of(commit_bytes.begin(), commit_bytes.end(), [](char byte) { return byte == 'Z'; }),
        "short commit writes nothing");
  key = Key(context, 'x');
  Status(epocanva_session_process_key(session, &key, &operation), EPOCANVA_BUSY,
         "unread commit prevents overwrite by another key");
  Check(operation.backend_call_made == 0 && operation.commit_pending == 1,
        "pending-commit refusal does not replay or overwrite");
  Status(epocanva_session_read_commit(session, &request, &commit, commit_bytes.data(), 6),
         EPOCANVA_OK, "full commit copy consumes exactly once");
  Check(Field(commit_bytes, commit.text) == "hello " && commit_bytes.back() == 'Z',
        "exact real commit bytes with no terminator overwrite");
  Status(epocanva_session_read_commit(session, &request, &commit, nullptr, 0),
         EPOCANVA_NO_COMMIT, "second commit read is empty");
  Check(SameCalls(before_read, Info(engine)), "all snapshot/commit queries stay out of Rime");

  // ABI exceptions are isolated without exporting exception::what() or text.
  auto fault_info = Structure<epocanva_engine_info>();
  allocation_fault = 1;
  const auto allocation_status = epocanva_engine_read_info(engine, &fault_info, nullptr, 0);
  const int allocation_unused = allocation_fault.exchange(0);
  Check(allocation_unused == 0, "allocation fault actually triggered inside API");
  Status(allocation_status, EPOCANVA_OUT_OF_MEMORY, "bad_alloc isolated at C boundary");
  allocation_fault = 2;
  const auto unknown_status = epocanva_engine_read_info(engine, &fault_info, nullptr, 0);
  const int unknown_unused = allocation_fault.exchange(0);
  Check(unknown_unused == 0, "nonstandard exception actually triggered inside API");
  Status(unknown_status, EPOCANVA_INTERNAL_ERROR, "unknown C++ exception isolated at C boundary");
  Check(SameCalls(before_read, Info(engine)), "read exceptions leave engine counters unchanged");

  // The first static-library allocation after a real key occurs while capturing
  // its state. Exercise the dangerous post-mutation exception, not just a read.
  for (int fault_kind : {1, 2}) {
    const auto fault_context = Context(70u + static_cast<std::uint64_t>(fault_kind));
    const auto fault_session = OpenSession(engine, fault_context);
    const auto before_fault = Info(engine);
    auto fault_key = Key(fault_context, 'h');
    auto fault_result = Structure<epocanva_operation_result>();
    allocation_fault = fault_kind;
    const auto fault_status = epocanva_session_process_key(fault_session, &fault_key, &fault_result);
    const int fault_unused = allocation_fault.exchange(0);
    Check(fault_unused == 0, "post-key allocation fault actually triggered");
    Status(fault_status, fault_kind == 1 ? EPOCANVA_OUT_OF_MEMORY : EPOCANVA_INTERNAL_ERROR,
           "post-key exception contained at C boundary");
    Check(fault_result.backend_call_made == 1 && fault_result.state_available == 0,
          "post-key failure prohibits replay instead of reporting unhandled");
    const auto after_fault = Info(engine);
    Check(after_fault.process_key_calls == before_fault.process_key_calls + 1,
          "fault probe actually entered real process_key once");
    Status(epocanva_session_process_key(fault_session, &fault_key, &fault_result),
           EPOCANVA_SESSION_FAULTED, "faulted session refuses replay");
    Check(fault_result.backend_call_made == 0, "faulted session has no second key call");
    auto fault_request = Request(fault_context);
    auto fault_snapshot = Structure<epocanva_snapshot>();
    Status(epocanva_session_read_snapshot(fault_session, &fault_request, &fault_snapshot, nullptr, 0),
           EPOCANVA_SESSION_FAULTED, "faulted cache cannot be read as current");
    Check(SameCalls(after_fault, Info(engine)), "faulted replay and read cause no Rime calls");
    Status(epocanva_session_close(fault_session), EPOCANVA_OK, "faulted real session remains closable");
  }

  // Discarding a pending event is explicit. A successful cancel never hands the
  // discarded commit back to a newly classified generation.
  Type(session, context, "hi");
  Process(session, context, ' ');
  Status(epocanva_session_cancel(session, &request, &operation), EPOCANVA_OK,
         "cancel explicitly discards an unread real commit");
  context.generation = operation.generation;
  request.context = context;
  Status(epocanva_session_read_commit(session, &request, &commit, nullptr, 0),
         EPOCANVA_NO_COMMIT, "discarded commit never appears in next generation");

  // Comments from the existing real spelling translator are not dropped.
  const auto spell_context = Context(33);
  const auto spelling = OpenSession(engine, spell_context, "epocanva_spell_english");
  Type(spelling, spell_context, "hellp");
  const auto spell_snapshot = Read(spelling, spell_context);
  std::string comment;
  Check(Candidate(spelling, Selection(spell_context, spell_snapshot.snapshot, 1), &comment) == "hello",
        "real spelling candidate text copied");
  Check(comment == "spell", "real spelling comment copied");
  Status(epocanva_session_close(spelling), EPOCANVA_OK, "close spelling session");

  // A real table translator, explicitly deployed above, checks non-ASCII spans.
  const auto unicode_context = Context(44);
  const auto unicode = OpenSession(engine, unicode_context, "abi_unicode");
  Type(unicode, unicode_context, "a");
  const auto unicode_snapshot = Read(unicode, unicode_context);
  Check(Field(unicode_snapshot.bytes, unicode_snapshot.snapshot.schema_name) == u8"ABI 邊界 probe",
        "Unicode schema name crosses ABI as UTF-8");
  Check(Field(unicode_snapshot.bytes, unicode_snapshot.snapshot.preedit) == u8"文😀",
        "real Rime multibyte preedit preserved");
  Check(unicode_snapshot.snapshot.preedit.utf8_byte_count == 7 &&
        unicode_snapshot.snapshot.cursor_utf8_byte_offset == 7,
        "multibyte caret is 7 UTF-8 bytes, not 3 UTF-16 units");
  Check(Candidate(unicode, Selection(unicode_context, unicode_snapshot.snapshot, 0)) == u8"文😀",
        "real Unicode candidate preserved");
  auto unicode_operation = Process(unicode, unicode_context, 0xff0d);
  Check(unicode_operation.commit_pending == 1, "real Unicode composition commits on Return");
  auto unicode_request = Request(unicode_context);
  auto unicode_commit = Structure<epocanva_commit>();
  char unicode_commit_bytes[8] = {};
  Status(epocanva_session_read_commit(unicode, &unicode_request, &unicode_commit,
                                     unicode_commit_bytes, 7), EPOCANVA_OK,
         "Unicode commit fits exact UTF-8 byte capacity");
  Check(unicode_commit.required_utf8_bytes == 7 &&
        std::string(unicode_commit_bytes, 7) == u8"文😀",
        "actual Unicode commit preserved byte-for-byte");
  Status(epocanva_session_close(unicode), EPOCANVA_OK, "close Unicode probe session");

  Status(epocanva_session_close(session), EPOCANVA_OK, "close main session");
  Status(epocanva_session_close(session), EPOCANVA_INVALID_HANDLE, "closed token is invalid");
  Status(epocanva_session_close(other), EPOCANVA_OK, "close independent session");
  Balanced(Info(engine));
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    Inputs input;
    fs::path repo, run_root;
    for (int argument = 1; argument < argc; argument += 2) {
      if (argument + 1 == argc) return 2;
      const auto name = fs::path(argv[argument]).u8string();
      if (name == "--mode") input.mode = fs::path(argv[argument + 1]).u8string();
      else if (name == "--rime-dll") input.dll = fs::path(argv[argument + 1]).u8string();
      else if (name == "--repo") repo = argv[argument + 1];
      else if (name == "--run-root") run_root = argv[argument + 1];
      else return 2;
    }
    if (input.mode.empty() || input.dll.empty() || repo.empty() || run_root.empty()) return 2;
    if (input.mode == "control-abi") {
      auto options = Structure<epocanva_engine_options>();
      options.abi_version = 0;
      epocanva_engine_handle engine = 0;
      const auto observed = epocanva_engine_open(&options, &engine);
      Status(observed, EPOCANVA_ABI_VERSION_MISMATCH, "control setup detects missing version");
      return DeliberatelyExpectSuccess(observed);
    }
    Prepare(&input, repo, run_root);
    if (input.mode == "missing-dll") {
      input.dll = (run_root / "absent-rime.dll").u8string();
      input.options.rime_dll_path = View(input.dll);
    } else if (input.mode == "wrong-version") {
      input.options.expected_rime_version = Literal("0.0.invalid");
    }
    epocanva_engine_handle engine = 0;
    const auto opened = epocanva_engine_open(&input.options, &engine);
    if (input.mode == "missing-dll" || input.mode == "wrong-version") {
      Status(opened, input.mode == "missing-dll" ? EPOCANVA_BACKEND_ERROR : EPOCANVA_BACKEND_VERSION_MISMATCH,
             "missing/wrong real backend refused instead of fake fallback");
      Check(engine == 0, "failed backend has no token");
      return Finish();
    }
    Status(opened, EPOCANVA_OK, "start actual pinned Rime runtime");
    if (opened != EPOCANVA_OK) return 5;
    if (input.mode.rfind("control-", 0) == 0) {
      const auto context = Context();
      epocanva_status observed = EPOCANVA_OK;
      if (input.mode == "control-utf") {
        auto options = Structure<epocanva_session_options>();
        options.context = context;
        const std::string malformed("\xed\xa0\x80", 3);
        options.schema_id = View(malformed);
        epocanva_session_handle session = 0;
        observed = epocanva_session_open(engine, &options, &session);
        Status(observed, EPOCANVA_INVALID_UTF8, "UTF fail-control prerequisite");
        if (session != 0) epocanva_session_close(session);
      } else {
        const auto session = OpenSession(engine, context);
        auto result = Structure<epocanva_operation_result>();
        if (input.mode == "control-privacy") {
          auto key = Key(context, 'h');
          key.context.privacy_class = EPOCANVA_PRIVACY_SENSITIVE;
          observed = epocanva_session_process_key(session, &key, &result);
          Status(observed, EPOCANVA_PRIVACY_DENIED, "privacy fail-control prerequisite");
        } else if (input.mode == "control-stale") {
          Type(session, context, "hel");
          const auto snapshot = Read(session, context);
          auto selection = Selection(context, snapshot.snapshot, 1);
          Process(session, context, 'l');
          observed = epocanva_session_select_candidate(session, &selection, &result);
          Status(observed, EPOCANVA_STALE_SNAPSHOT, "stale fail-control prerequisite");
        } else return 2;
        Status(epocanva_session_close(session), EPOCANVA_OK, "control closes real session");
      }
      Balanced(Info(engine));
      Status(epocanva_engine_close(engine), EPOCANVA_OK, "control shuts down real runtime");
      return DeliberatelyExpectSuccess(observed);
    }
    if (input.mode != "real") return 2;
    Contracts(engine, input);
    Status(epocanva_engine_close(engine), EPOCANVA_OK, "runtime shutdown completes");
    Status(epocanva_engine_close(engine), EPOCANVA_INVALID_HANDLE, "closed runtime token invalid");
    // No implicit deployment on an ordinary runtime restart. Successful restart
    // also proves the existing shared runtime slot was released by prior close.
    input.options.deployment_schema_paths = nullptr;
    input.options.deployment_schema_count = 0;
    epocanva_engine_handle restarted = 0;
    Status(epocanva_engine_open(&input.options, &restarted), EPOCANVA_OK, "restart uses prepared data without deployment");
    if (restarted != 0) {
      Check(restarted != engine, "closed engine token never reused");
      const auto session = OpenSession(restarted, Context(55));
      Status(epocanva_session_close(session), EPOCANVA_OK, "prepared runtime real session closes");
      Balanced(Info(restarted));
      Status(epocanva_engine_close(restarted), EPOCANVA_OK, "prepared runtime closes");
    }
    return Finish();
  } catch (...) {
    allocation_fault = 0;
    std::puts("FAIL test setup threw; no success or fail-control marker");
    return 5;
  }
}
