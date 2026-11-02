// EpoCanva W2 -- product DLL verification through real TSF keystroke routing.
//
// WHAT THIS IS, AND IS NOT. The test loads the real epocanva_tsf.dll by absolute path in a
// controlled STA host (real msctf thread manager, document manager, context and
// ITextStoreACP from tests/windows/tsf_edit). It does not register the DLL, does not write
// the registry, does not switch the system input language and never touches another
// application. The test obtains IID_ITfKeyEventSink from the TIP and calls OnTestKeyDown/
// OnKeyDown/OnKeyUp directly with the real context; text is read back from the controlled
// store. An unregistered TIP cannot advise a real key sink (AdviseKeyEventSink rejects a
// synthetic client id), so this is "controlled TextStore + direct sink invocation", not
// "registered input in a real application".
//
// WHAT IT PROVES. Activation/deactivation of the product DLL, deployment and marker
// retirement, the default raw English path, case preservation, Space/Enter/BackSpace/
// Escape, punctuation and identifiers, shortcut pass-through, permission refusals,
// external-edit recovery and two STA sessions in one process sharing one engine.
//
// PRIVACY. All input in this file is fixed test data. Failures report lengths and the first
// differing UTF-16 offset, never the document text.
#include <windows.h>
#include <msctf.h>
#include <inputscope.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "identifiers.h"
#include "crash_capture.h"
#include "observer_test_support.h"
#include "product_paths.h"
#include "product_settings.h"
#include "test_host.h"

namespace {

using epocanva::tsf::context::test::CreateVisibleTestWindow;
using epocanva::tsf::context::test::ExternalEditMode;
using epocanva::tsf::context::test::ExternalEditResult;
using epocanva::tsf::context::test::RunExternalEdit;
using epocanva::tsf::context::test::SetThreadFocus;
using epocanva::tsf::context::test::TestDocument;
using epocanva::tsf::context::test::TestDocumentOptions;
using epocanva::tsf::context::test::TestHost;

int g_failures = 0;

void LocalCheck(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

void CheckDocExact(const std::wstring& actual, const std::wstring& expected,
                   const std::string& what) {
  const bool equal = actual == expected;
  LocalCheck(equal, what);
  if (!equal) {
    std::size_t offset = 0;
    while (offset < actual.size() && offset < expected.size() &&
           actual[offset] == expected[offset]) {
      ++offset;
    }
    std::cout << "    doc mismatch actual_len=" << actual.size()
              << " expected_len=" << expected.size()
              << " first_diff=" << offset << "\n";
  }
}

bool CopyFileTo(const std::filesystem::path& from, const std::filesystem::path& to,
                std::wstring* why) {
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  if (ec || !::CopyFileW(from.c_str(), to.c_str(), FALSE)) {
    if (why != nullptr) {
      *why = L"cannot copy " + from.wstring() + L" to " + to.wstring();
    }
    return false;
  }
  return true;
}

std::string ReadNarrow(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

bool StageProduct(const std::filesystem::path& stage, const std::filesystem::path& dll,
                  const std::filesystem::path& rime_dll,
                  const std::filesystem::path& data_dir,
                  const std::filesystem::path& lexicon, bool with_rime,
                  std::wstring* why) {
  std::error_code ec;
  std::filesystem::remove_all(stage, ec);
  std::filesystem::create_directories(stage / L"data" / L"lexicon", ec);
  if (!CopyFileTo(dll, stage / L"epocanva_tsf.dll", why)) {
    return false;
  }
  if (with_rime && !CopyFileTo(rime_dll, stage / L"rime.dll", why)) {
    return false;
  }
  if (!CopyFileTo(data_dir / L"default.yaml", stage / L"data" / L"default.yaml", why) ||
      !CopyFileTo(data_dir / L"epocanva_english.schema.yaml",
                  stage / L"data" / L"epocanva_english.schema.yaml", why) ||
      !CopyFileTo(lexicon, stage / L"data" / L"lexicon" / L"epocanva_en_alpha.tsv", why)) {
    return false;
  }
  return true;
}

bool PrepareUserRoot(const std::filesystem::path& user_root,
                     const epocanva::product::ProductSettings& settings,
                     std::wstring* why) {  std::error_code ec;
  std::filesystem::remove_all(user_root, ec);
  std::filesystem::create_directories(user_root / L"engine", ec);
  std::filesystem::create_directories(user_root / L"logs", ec);
  epocanva::product::ProductPaths paths;
  paths.user_root = user_root;
  paths.engine_user_dir = user_root / L"engine";
  paths.log_dir = user_root / L"logs";
  paths.settings_file = user_root / L"settings.ini";
  paths.schema_patch_file = paths.engine_user_dir / L"epocanva_english.custom.yaml";
  paths.deploy_marker_file = paths.engine_user_dir / L"deploy-required";
  paths.deployed_state_file = paths.engine_user_dir / L"deployed-state";
  paths.compiled_schema_file =
      paths.engine_user_dir / L"build" / L"epocanva_english.schema.yaml";
  paths.compiled_schema_binary =
      paths.engine_user_dir / L"build" / L"epocanva_english.schema.bin";
  std::string token;
  if (!epocanva::product::SaveSettingsBundle(paths, settings, &token, why)) {
    return false;
  }
  return true;
}

bool WriteSettingsOnly(const std::filesystem::path& user_root,
                       const epocanva::product::ProductSettings& settings,
                       std::wstring* why) {
  epocanva::product::ProductPaths paths;
  paths.user_root = user_root;
  paths.engine_user_dir = user_root / L"engine";
  paths.log_dir = user_root / L"logs";
  paths.settings_file = user_root / L"settings.ini";
  paths.schema_patch_file = paths.engine_user_dir / L"epocanva_english.custom.yaml";
  paths.deploy_marker_file = paths.engine_user_dir / L"deploy-required";
  paths.deployed_state_file = paths.engine_user_dir / L"deployed-state";
  paths.compiled_schema_file =
      paths.engine_user_dir / L"build" / L"epocanva_english.schema.yaml";
  paths.compiled_schema_binary =
      paths.engine_user_dir / L"build" / L"epocanva_english.schema.bin";
  std::string token;
  return epocanva::product::SaveSettingsBundle(paths, settings, &token, why);
}

struct LoadedProduct {
  HMODULE module = nullptr;
  IClassFactory* factory = nullptr;
  ITfTextInputProcessorEx* tip = nullptr;
  ITfKeyEventSink* sink = nullptr;
  TfClientId client_id = 0;
};

using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using CanUnloadNowFn = HRESULT(WINAPI*)();

bool PrepareProduct(LoadedProduct* product, ITfThreadMgr* thread_manager,
                    const std::filesystem::path& stage, std::wstring* why) {
  const std::filesystem::path dll = stage / L"epocanva_tsf.dll";
  product->module = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (product->module == nullptr) {
    if (why != nullptr) {
      *why = L"LoadLibraryExW failed with error " + std::to_wstring(::GetLastError());
    }
    return false;
  }
  const auto get_class_object = reinterpret_cast<GetClassObjectFn>(
      ::GetProcAddress(product->module, "DllGetClassObject"));
  if (get_class_object == nullptr) {
    if (why != nullptr) {
      *why = L"DllGetClassObject is missing";
    }
    return false;
  }
  HRESULT hr = get_class_object(epocanva::tsf::kTextServiceClsid,
                                IID_IClassFactory,
                                reinterpret_cast<void**>(&product->factory));
  if (FAILED(hr) || product->factory == nullptr) {
    if (why != nullptr) {
      *why = L"DllGetClassObject failed";
    }
    return false;
  }
  hr = product->factory->CreateInstance(
      nullptr, IID_ITfTextInputProcessorEx,
      reinterpret_cast<void**>(&product->tip));
  if (FAILED(hr) || product->tip == nullptr) {
    if (why != nullptr) {
      *why = L"IClassFactory::CreateInstance failed";
    }
    return false;
  }
  ITfClientId* id_source = nullptr;
  hr = thread_manager->QueryInterface(IID_ITfClientId,
                                      reinterpret_cast<void**>(&id_source));
  if (FAILED(hr) || id_source == nullptr) {
    if (why != nullptr) {
      *why = L"ITfClientId is unavailable";
    }
    return false;
  }
  hr = id_source->GetClientId(epocanva::tsf::kTextServiceClsid,
                              &product->client_id);
  id_source->Release();
  if (FAILED(hr) || product->client_id == 0) {
    if (why != nullptr) {
      *why = L"ITfClientId::GetClientId failed";
    }
    return false;
  }
  hr = product->tip->QueryInterface(IID_ITfKeyEventSink,
                                     reinterpret_cast<void**>(&product->sink));
  if (FAILED(hr) || product->sink == nullptr) {
    if (why != nullptr) {
      *why = L"ITfKeyEventSink is unavailable";
    }
    return false;
  }
  return true;
}

bool ActivateProduct(LoadedProduct* product, ITfThreadMgr* thread_manager,
                     const std::filesystem::path& stage, std::wstring* why) {
  if (!PrepareProduct(product, thread_manager, stage, why)) {
    return false;
  }
  // Retry activation: a previous case's engine may still be finishing its stop after the
  // last lease was released. A real failure is still reported after the deadline.
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  HRESULT last = E_FAIL;
  for (;;) {
    last = product->tip->Activate(thread_manager, product->client_id);
    if (SUCCEEDED(last)) {
      break;
    }
    if (::GetTickCount64() >= deadline) {
      break;
    }
    ::Sleep(50);
  }
  if (FAILED(last)) {
    if (why != nullptr) {
      wchar_t buffer[32] = {};
      swprintf_s(buffer, L"Activate failed hr=0x%08lX",
                 static_cast<unsigned long>(last));
      *why = buffer;
    }
    return false;
  }
  return true;
}

void UnregisterSeam(ITfKeyEventSink* sink);

void ReleaseProduct(LoadedProduct* product, bool deactivate) {
  if (product->tip != nullptr) {
    if (deactivate) {
      (void)product->tip->Deactivate();
    }
    product->tip->Release();
    product->tip = nullptr;
  }
  if (product->sink != nullptr) {
    UnregisterSeam(product->sink);
    product->sink->Release();
    product->sink = nullptr;
  }
  if (product->factory != nullptr) {
    product->factory->Release();
    product->factory = nullptr;
  }
}

// Two concurrent sessions in one process share one loaded module. A session that finishes
// first cannot free its LoadLibrary reference while the other is still running, so it parks
// the handle here and the case frees every parked reference once all sessions are gone.
std::mutex g_deferred_modules_mutex;
std::vector<HMODULE> g_deferred_modules;

void DeferProductModule(LoadedProduct* product) {
  if (product->module == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_deferred_modules_mutex);
  g_deferred_modules.push_back(product->module);
  product->module = nullptr;
}

bool FreeDeferredProductModules(DWORD timeout_ms, const char* label) {
  std::vector<HMODULE> modules;
  {
    std::lock_guard<std::mutex> guard(g_deferred_modules_mutex);
    modules.swap(g_deferred_modules);
  }
  if (modules.empty()) {
    return true;
  }
  const auto can_unload = reinterpret_cast<CanUnloadNowFn>(
      ::GetProcAddress(modules.front(), "DllCanUnloadNow"));
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  HRESULT status = can_unload != nullptr ? can_unload() : E_FAIL;
  while (status != S_OK && ::GetTickCount64() < deadline) {
    ::Sleep(20);
    status = can_unload != nullptr ? can_unload() : E_FAIL;
  }
  if (status != S_OK) {
    std::cout << "  [unload] " << label
              << ": DllCanUnloadNow stayed S_FALSE; deferred references are left loaded"
              << std::endl;
    std::lock_guard<std::mutex> guard(g_deferred_modules_mutex);
    g_deferred_modules.insert(g_deferred_modules.end(), modules.begin(), modules.end());
    return false;
  }
  for (HMODULE module : modules) {
    (void)::FreeLibrary(module);
  }
  HMODULE still = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(modules.front()), &still) != FALSE) {
    std::cout << "  [unload] " << label
              << ": the image is still mapped after freeing deferred references"
              << std::endl;
    return false;
  }
  return true;
}

HRESULT ProductUnloadStatus(LoadedProduct* product) {
  if (product->module == nullptr) {
    return S_OK;
  }
  const auto can_unload = reinterpret_cast<CanUnloadNowFn>(
      ::GetProcAddress(product->module, "DllCanUnloadNow"));
  return can_unload != nullptr ? can_unload() : E_FAIL;
}

// The COM unload contract, applied by the client that loaded the DLL: free the module only
// after the server says it can unload, and only after every TSF-owned object the DLL handed
// out has been released (which requires tearing the TSF host down first). A forced
// FreeLibrary here would leave TSF holding vtables in an unmapped image.
bool FreeProductModule(LoadedProduct* product, DWORD timeout_ms,
                       const char* label) {
  if (product->module == nullptr) {
    return true;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  HRESULT status = ProductUnloadStatus(product);
  while (status != S_OK && ::GetTickCount64() < deadline) {
    ::Sleep(20);
    status = ProductUnloadStatus(product);
  }
  if (status != S_OK) {
    std::cout << "  [unload] " << label
              << ": DllCanUnloadNow stayed S_FALSE; the module is left loaded (not forced)"
              << std::endl;
    return false;
  }
  HMODULE module = product->module;
  product->module = nullptr;
  if (!::FreeLibrary(module)) {
    std::cout << "  [unload] " << label << ": FreeLibrary failed err="
              << ::GetLastError() << std::endl;
    return false;
  }
  HMODULE still = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(module), &still) != FALSE) {
    std::cout << "  [unload] " << label
              << ": the image is still mapped after FreeLibrary" << std::endl;
    return false;
  }
  return true;
}

HANDLE g_pump_event = nullptr;

void PumpSlice(TestHost& host, DWORD milliseconds);

// The direct-sink test DLL's controlled seam (product_test_seam.h). The struct layout is
// duplicated here because the test executable is not compiled with the DLL's private
// header; both sides use plain C types.
struct SeamCounters {
  unsigned long long key_downs;
  unsigned long long eaten;
  unsigned long long literal_key_downs;
  unsigned long long raw_dropped_keys;
  unsigned long long raw_unrecoverable;
  unsigned long long raw_applied_units;
  unsigned long long raw_cancelled_units;
  unsigned long long raw_commits;
  unsigned long long raw_ignored_edits;
  unsigned long long candidate_version;
  unsigned long long candidate_items;
  unsigned long long candidate_stale;
  int candidate_menu_open;
  int candidate_selected;
  int page_no;
  int page_size;
  int is_last_page;
  int engine_ready;
  int engine_state;
  unsigned long long engine_attach_failures;
  unsigned long long queue_pending;
  unsigned long long queue_in_flight;
  unsigned long long coordinator_phase;
  unsigned long long engine_requests;
  unsigned long long engine_results;
  int raw_active;
  int raw_submitted;
  unsigned long long works_accepted;
  unsigned long long target_binds;
  unsigned long long rebind_requests;
  unsigned long long raw_entered;
  unsigned long long last_submit_status;
  unsigned long long gate;
  unsigned long long permission;
  unsigned long long composition_length;
  int composition_active;
  unsigned long long raw_reason_unapplied;
  unsigned long long raw_reason_not_candidate;
  unsigned long long raw_reason_high_water;
  unsigned long long raw_reason_admission;
  unsigned long long last_raw_composition_start;
  unsigned long long last_raw_composition_units;
  unsigned long long last_raw_input_units;
  unsigned long long last_raw_plan_units;
  unsigned long long raw_unapplied_units;
  unsigned long long raw_exits;
  unsigned long long raw_failed_keys;
  unsigned long long raw_failed_units;
  unsigned long long host_text_keys;
  unsigned long long suggestion_queued;
  unsigned long long suggestion_submitted;
  unsigned long long suggestion_completed;
  unsigned long long suggestion_dropped;
  unsigned long long suggestion_stale;
  unsigned long long suggestion_resets;
  unsigned long long suggestion_pending;
  unsigned long long replacement_attempts;
  unsigned long long replacement_applied;
  unsigned long long replacement_refused;
  unsigned long long internal_text_losses;
  unsigned long long host_token_units;
  int suggestion_active;
  int suggestion_in_flight;
  int replacement_pending;
  int coordinator_bound;
  unsigned long long last_edit_status;
  unsigned long long last_edit_reason;
  unsigned long long pumps;
  int session_pending;
  int edit_completion_signaled;
  unsigned long long plans_submitted;
  unsigned long long edits_applied;
  unsigned long long edits_refused;
  unsigned long long edits_stale;
  unsigned long long work_kind;
  unsigned long long work_outcome;
  unsigned long long edit_pump_checks;
  unsigned long long edit_pump_ready;
  unsigned long long coordinator_pumps;
  unsigned long long host_edits_matched;
  unsigned long long host_edits_unexpected;
  unsigned long long reset_boundary;
  unsigned long long reset_owed;
  unsigned long long reset_submit_refusal;
  unsigned long long reset_replacement;
  unsigned long long target_losses;
  int observer_awaiting_bind;
  int observer_published;
  unsigned long long observer_last_drop;
  int rebind_pending;
  unsigned long long last_bind_status;
  unsigned long long binds_refused;
  unsigned long long binds_paused_retiring;
  unsigned long long observer_selection_refusals;
  unsigned long long observer_selection_reads;
  int observer_selection_pending;
  int observer_has_context;
  int observer_subscribed;
  unsigned long long retiring_bindings;
  int admission_paused;
  unsigned long long retiring_compositions;
  unsigned long long retired_settled;
  unsigned long long retired_already_ordinary;
  unsigned long long retired_blocked;
  unsigned long long close_requests;
  unsigned long long close_results;
  unsigned long long close_failures;
  unsigned long long unresolved_close;
  unsigned long long unresolved_engine;
  int pending_edit_event;
  int token_abandoned;
  int suggestion_stale_now;
  unsigned long long binding_invalidations;
  unsigned long long binding_last_loss_reason;
  long long candidate_caret;
  unsigned long long candidate_input_units;
  unsigned long long target_revalidations;
  unsigned long long target_refusals;
  unsigned long long last_target_revalidation;
  long long last_edit_request_hr;
  long long last_edit_session_hr;
  long long last_edit_operation_hr;
  unsigned long long target_callback_checks;
  unsigned long long target_rejections;
  unsigned long long permission_rechecks;
  int last_replacement_outcome;
  int last_replacement_edit_status;
  int last_replacement_edit_reason;
  long long last_replacement_expected_units;
  long long last_edit_observed_caret;
  long long last_suggestion_expected_caret;
  long long candidate_anchor_caret;
  long long candidate_anchor_expected;
  int candidate_anchor_retries;
  int candidate_token_origin_valid;
  long long candidate_token_origin_caret;
  long long predicted_host_caret;
};

using SeamQueryFn = int (*)(SeamCounters*);
using SeamTextFn = int (*)(unsigned, wchar_t*, unsigned);
using SeamKindFn = int (*)(unsigned);
using SeamSelectFn = int (*)(unsigned long long, int);
using SeamBeginStartFn = int (*)(void*, void*);
using SeamResetFn = int (*)();
using SeamStateFn = int (*)();
using SeamModuleStateFn = int (*)(long long*, long long*, long long*, long long*,
                                  long long*);
using SeamGuardedObjectsFn = int (*)(long long*, int);

struct ProductSeam {
  SeamQueryFn query = nullptr;
  SeamTextFn text = nullptr;
  SeamKindFn kind = nullptr;
  SeamSelectFn select = nullptr;
  SeamBeginStartFn begin_start = nullptr;
  SeamResetFn reset = nullptr;
  SeamStateFn state = nullptr;
  SeamModuleStateFn module_state = nullptr;
  SeamGuardedObjectsFn guarded_objects = nullptr;

  bool usable() const {
    return query != nullptr && text != nullptr && kind != nullptr &&
           select != nullptr;
  }
  bool startup_usable() const {
    return begin_start != nullptr && reset != nullptr && state != nullptr;
  }
};

// Test-only crash evidence: the handler prints these text-free counters into the crash
// report. The buffer is static because the handler cannot rely on allocation.
ProductSeam g_crash_seam;
std::mutex g_seam_map_mutex;
std::map<ITfKeyEventSink*, ProductSeam> g_seam_map;

ProductSeam LoadSeam(LoadedProduct& product) {
  ProductSeam seam;
  seam.query = reinterpret_cast<SeamQueryFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestQueryCounters"));
  seam.text = reinterpret_cast<SeamTextFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestCandidateText"));
  seam.kind = reinterpret_cast<SeamKindFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestCandidateKind"));
  seam.select = reinterpret_cast<SeamSelectFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestSelectCandidate"));
  seam.begin_start = reinterpret_cast<SeamBeginStartFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestBeginBlockingStart"));
  seam.reset = reinterpret_cast<SeamResetFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestResetBootstrap"));
  seam.state = reinterpret_cast<SeamStateFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestBootstrapState"));
  seam.module_state = reinterpret_cast<SeamModuleStateFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestModuleState"));
  seam.guarded_objects = reinterpret_cast<SeamGuardedObjectsFn>(
      ::GetProcAddress(product.module, "EpoCanvaTestGuardedObjects"));
  g_crash_seam = seam;
  if (product.sink != nullptr) {
    std::lock_guard<std::mutex> guard(g_seam_map_mutex);
    g_seam_map[product.sink] = seam;
  }
  return seam;
}

void UnregisterSeam(ITfKeyEventSink* sink) {
  if (sink == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_seam_map_mutex);
  g_seam_map.erase(sink);
}

bool QuerySeamForSink(ITfKeyEventSink* sink, SeamCounters* counters) {
  if (sink == nullptr || counters == nullptr) {
    return false;
  }
  ProductSeam seam;
  {
    std::lock_guard<std::mutex> guard(g_seam_map_mutex);
    const auto found = g_seam_map.find(sink);
    if (found == g_seam_map.end() || found->second.query == nullptr) {
      return false;
    }
    seam = found->second;
  }
  return seam.query(counters) == 0;
}

const char* CrashCountersForReport() {
  static char buffer[2048] = "seam unavailable";
  if (g_crash_seam.query == nullptr) {
    return buffer;
  }
  SeamCounters counters = {};
  if (g_crash_seam.query(&counters) != 0) {
    std::snprintf(buffer, sizeof(buffer), "controller unavailable");
    return buffer;
  }
  std::snprintf(
      buffer, sizeof(buffer),
      "keys=%llu eaten=%llu raw_dropped=%llu raw_unrecoverable=%llu raw_applied=%llu "
      "raw_cancelled=%llu raw_commits=%llu raw_active=%d raw_submitted=%d raw_entered=%llu "
      "raw_exits=%llu queue=%llu in_flight=%llu phase=%llu comp=%d/%llu binds=%llu "
      "rebinds=%llu menu=%d version=%llu selected=%d engine_ready=%d work=%llu req=%llu "
      "res=%llu last_raw_start=%llu last_raw_comp=%llu last_raw_input=%llu last_raw_plan=%llu",
      counters.key_downs, counters.eaten, counters.raw_dropped_keys,
      counters.raw_unrecoverable, counters.raw_applied_units,
      counters.raw_cancelled_units, counters.raw_commits, counters.raw_active,
      counters.raw_submitted, counters.raw_entered, counters.raw_exits,
      counters.queue_pending, counters.queue_in_flight, counters.coordinator_phase,
      counters.composition_active, counters.composition_length, counters.target_binds,
      counters.rebind_requests, counters.candidate_menu_open, counters.candidate_version,
      counters.candidate_selected, counters.engine_ready, counters.works_accepted,
      counters.engine_requests, counters.engine_results,
      counters.last_raw_composition_start, counters.last_raw_composition_units,
      counters.last_raw_input_units, counters.last_raw_plan_units);
  return buffer;
}

void CrashPhase(const std::string& phase) {
  epocanva::tsf::product::test::SetCrashPhase(phase);
  std::cout << "[phase] " << phase << std::endl;
}

bool WaitForCandidateMenu(TestHost& host, const ProductSeam& seam,
                          SeamCounters* counters, std::size_t minimum_items,
                          DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (seam.query != nullptr && seam.query(counters) == 0 &&
        counters->candidate_menu_open != 0 &&
        counters->candidate_items >= minimum_items &&
        counters->composition_active != 0 && counters->coordinator_phase == 0 &&
        counters->queue_pending == 0 && counters->queue_in_flight == 0) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      std::cout << "  menu wait timeout: open="
                << (counters != nullptr ? counters->candidate_menu_open : -1)
                << " items=" << (counters != nullptr ? counters->candidate_items : 0)
                << " version=" << (counters != nullptr ? counters->candidate_version : 0)
                << " selected=" << (counters != nullptr ? counters->candidate_selected : -1)
                << " engine=" << (counters != nullptr ? counters->engine_ready : -1)
                << " key_downs=" << (counters != nullptr ? counters->key_downs : 0)
                << " eaten=" << (counters != nullptr ? counters->eaten : 0)
                << " queued=" << (counters != nullptr ? counters->queue_pending : 0)
                << " in_flight=" << (counters != nullptr ? counters->queue_in_flight : 0)
                << " phase=" << (counters != nullptr ? counters->coordinator_phase : 0)
                << " req=" << (counters != nullptr ? counters->engine_requests : 0)
                << " res=" << (counters != nullptr ? counters->engine_results : 0)
                << " sug_q=" << (counters != nullptr ? counters->suggestion_queued : 0)
                << " sug_done=" << (counters != nullptr ? counters->suggestion_completed : 0)
                << " sug_drop=" << (counters != nullptr ? counters->suggestion_dropped : 0)
                << " sug_stale=" << (counters != nullptr ? counters->suggestion_stale : 0)
                << " token=" << (counters != nullptr ? counters->host_token_units : 0)
                << " abandoned=" << (counters != nullptr ? counters->token_abandoned : 0)
                << " stale_now=" << (counters != nullptr ? counters->suggestion_stale_now : 0)
                << " invalid=" << (counters != nullptr ? counters->binding_invalidations : 0)
                << "/" << (counters != nullptr ? counters->binding_last_loss_reason : 0)
                << " bound=" << (counters != nullptr ? counters->coordinator_bound : 0)
                << " raw=" << (counters != nullptr ? counters->raw_active : 0)
                << "/" << (counters != nullptr ? counters->raw_submitted : 0)
                << " works=" << (counters != nullptr ? counters->works_accepted : 0)
                << " binds=" << (counters != nullptr ? counters->target_binds : 0)
                << " rebinds=" << (counters != nullptr ? counters->rebind_requests : 0)
                << " raw_entered=" << (counters != nullptr ? counters->raw_entered : 0)
                << " submit=" << (counters != nullptr ? counters->last_submit_status : 0)
                << " gate=" << (counters != nullptr ? counters->gate : 0)
                << " perm=" << (counters != nullptr ? counters->permission : 0)
                << " comp=" << (counters != nullptr ? counters->composition_active : 0)
                << "/" << (counters != nullptr ? counters->composition_length : 0)
                << " rawwhy=" << (counters != nullptr ? counters->raw_reason_unapplied : 0)
                << "," << (counters != nullptr ? counters->raw_reason_not_candidate : 0)
                << "," << (counters != nullptr ? counters->raw_reason_high_water : 0)
                << "," << (counters != nullptr ? counters->raw_reason_admission : 0)
                << std::endl;
      return false;
    }
    PumpSlice(host, 10);
  }
}

std::wstring ReadCandidateText(const ProductSeam& seam, unsigned index) {
  wchar_t buffer[128] = {};
  const int len = seam.text != nullptr ? seam.text(index, buffer, 128) : -1;
  return len >= 0 ? std::wstring(buffer) : std::wstring();
}

// Selects a candidate only after the menu and the engine stream are stable: the snapshot
// version must stop changing and no suggestion/replacement work may be in flight. Returns
// the version and text that were actually current when the click was delivered.
bool WaitForStableMenu(TestHost& host, const ProductSeam& seam,
                       SeamCounters* counters, DWORD timeout_ms) {
  unsigned long long stable_version = 0;
  int stable_rounds = 0;
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    SeamCounters current = {};
    if (seam.query != nullptr && seam.query(&current) == 0 &&
        current.candidate_menu_open != 0 && current.queue_pending == 0 &&
        current.queue_in_flight == 0 && current.suggestion_pending == 0 &&
        current.suggestion_in_flight == 0 && current.replacement_pending == 0) {
      if (stable_rounds > 0 && current.candidate_version == stable_version) {
        *counters = current;
        return true;
      }
      stable_version = current.candidate_version;
      stable_rounds = 1;
    } else {
      stable_rounds = 0;
    }
    if (::GetTickCount64() >= deadline) {
      if (counters != nullptr) {
        *counters = current;
      }
      return false;
    }
    PumpSlice(host, 10);
  }
}

bool SelectStableCandidate(TestHost& host, const ProductSeam& seam, int index,
                           unsigned long long* version, std::wstring* text) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    SeamCounters counters = {};
    if (seam.query == nullptr || seam.query(&counters) != 0) {
      PumpSlice(host, 10);
      continue;
    }
    if (counters.candidate_version > *version) {
      *version = counters.candidate_version;
    }
    const bool stable = counters.candidate_menu_open != 0 &&
                        counters.candidate_version == *version &&
                        counters.queue_pending == 0 && counters.queue_in_flight == 0 &&
                        counters.suggestion_pending == 0 &&
                        counters.suggestion_in_flight == 0 &&
                        counters.replacement_pending == 0;
    if (stable) {
      *text = ReadCandidateText(seam, static_cast<unsigned>(index));
      if (text->empty()) {
        PumpSlice(host, 10);
        continue;
      }
      (void)seam.select(*version, static_cast<int>(index));
      return true;
    }
    PumpSlice(host, 10);
  }
  return false;
}

void PumpSlice(TestHost& host, DWORD milliseconds) {
  if (g_pump_event == nullptr) {
    g_pump_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  host.PumpUntil(g_pump_event, milliseconds);
}

void PrintTextDiff(const std::wstring& actual, const std::wstring& expected) {
  std::size_t first = 0;
  while (first < actual.size() && first < expected.size() &&
         actual[first] == expected[first]) {
    ++first;
  }
  std::cout << "  text mismatch actual_len=" << actual.size()
            << " expected_len=" << expected.size() << " first_diff=" << first;
  if (first < actual.size()) {
    std::cout << " actual_cp=" << static_cast<unsigned>(actual[first]);
  }
  if (first < expected.size()) {
    std::cout << " expected_cp=" << static_cast<unsigned>(expected[first]);
  }
  std::cout << std::endl;
}

bool WaitForText(TestHost& host, TestDocument* document,
                 const std::wstring& expected, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (document->text_utf16() == expected) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      if (document->text_utf16() != expected) {
        PrintTextDiff(document->text_utf16(), expected);
      }
      return false;
    }
    PumpSlice(host, 10);
  }
}

std::array<std::uint8_t, 256> g_saved_key_state = {};
bool g_key_state_saved = false;

void SaveKeyStateOnce() {
  if (!g_key_state_saved) {
    ::GetKeyboardState(g_saved_key_state.data());
    g_key_state_saved = true;
  }
}

void SetStateForKey(WCHAR ch, bool shift, bool control, bool alt) {
  std::array<std::uint8_t, 256> state = {};
  if (shift) {
    state[VK_SHIFT] = 0x80;
    state[VK_LSHIFT] = 0x80;
  }
  if (control) {
    state[VK_CONTROL] = 0x80;
    state[VK_LCONTROL] = 0x80;
  }
  if (alt) {
    state[VK_MENU] = 0x80;
    state[VK_LMENU] = 0x80;
  }
  (void)ch;
  ::SetKeyboardState(state.data());
}

void RestoreKeyState() {
  if (g_key_state_saved) {
    ::SetKeyboardState(g_saved_key_state.data());
  }
}

LPARAM KeyLparam(UINT vk, bool down, bool repeat) {
  UINT scan = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  LPARAM lparam = 1 | (static_cast<LPARAM>(scan) << 16);
  if (down) {
    if (repeat) {
      lparam |= (1LL << 30);
    }
  } else {
    lparam |= (1LL << 30) | (1LL << 31);
  }
  return lparam;
}

// The controlled host's real keyboard target plus the application's own client id. When the
// product does not eat a key, the host must perform the same default effect a real
// application would: insert the translated character through a real TSF edit session with
// the application's client id (the observer classifies it as external, as it would in a real
// host). Skipping that effect would make a pass-through look like a no-op and hide the
// document getting out of order.
struct InputRoute {
  TestHost* host = nullptr;
  TestDocument* doc = nullptr;
  ITfKeyEventSink* sink = nullptr;
  ITfContext* context = nullptr;
  // Units offered by keys that really produce text, and where they went. The product may
  // only answer TRUE when it delivers the text itself; ordinary input must always be the
  // host's. This accounting is written from the test's own actions, never from product state.
  std::size_t typed_text_units = 0;
  std::size_t host_inserted_units = 0;
  std::size_t product_suppressed_units = 0;

  bool TextWasSuppressed() const { return product_suppressed_units != 0; }
};

bool ReadyToType(TestHost& host, ITfKeyEventSink* sink, ITfContext* context,
                 DWORD timeout_ms, SeamCounters* last_observed = nullptr);

// Independent reference edit model. It is written from the documented default English
// behaviour (raw characters preserved, Space commits and inserts one space) and never calls
// product code. The final controlled-store text must equal its text.
struct ReferenceDoc {
  std::wstring text;
  void HostInsert(const std::wstring& units) { text += units; }
  void RawAppend(const std::wstring& units) { text += units; }
  void HostBackspace() {
    if (text.empty()) {
      return;
    }
    std::size_t remove = 1;
    if (text.size() >= 2) {
      const wchar_t last = text[text.size() - 1];
      const wchar_t previous = text[text.size() - 2];
      if (last >= 0xDC00 && last <= 0xDFFF && previous >= 0xD800 &&
          previous <= 0xDBFF) {
        remove = 2;
      }
    }
    text.resize(text.size() - remove);
  }
  // Explicit candidate acceptance: replace the whole current token with a candidate.
  void ReplaceToken(const std::wstring& token, const std::wstring& replacement) {
    if (token.empty() || text.size() < token.size()) {
      return;
    }
    if (text.compare(text.size() - token.size(), token.size(), token) == 0) {
      text.resize(text.size() - token.size());
      text += replacement;
    }
  }
};

bool ApplyHostDefault(InputRoute* route, UINT vk, const std::wstring& text,
                      std::string* error) {
  std::wstring default_text = text;
  LONG start = route->doc->selection_start_utf16();
  LONG end = route->doc->selection_end_utf16();
  if (default_text.empty()) {
    if (vk == VK_BACK) {
      if (start != end) {
        // The selection is replaced by nothing.
      } else if (start > 0) {
        // Remove one code point (a surrogate pair is removed as a pair).
        LONG from = start - 1;
        const std::wstring& current = route->doc->text_utf16();
        if (from > 0) {
          const wchar_t previous = current[static_cast<std::size_t>(from - 1)];
          const wchar_t last = current[static_cast<std::size_t>(from)];
          if (previous >= 0xD800 && previous <= 0xDBFF && last >= 0xDC00 &&
              last <= 0xDFFF) {
            --from;
          }
        }
        start = from;
      } else {
        return true;  // nothing before the caret: the host does nothing
      }
    } else if (vk == VK_RETURN) {
      default_text = L"\n";
    } else {
      return true;  // this controlled host has no text default for the key
    }
  }
  if (default_text.empty() && start == end) {
    return true;
  }
  std::string client_error;
  const TfClientId client =
      epocanva::tsf::context::test::ExternalClientId(
          route->host->thread_manager_for_test(), &client_error);
  if (client == 0) {
    *error = "the host default input has no client id: " + client_error;
    return false;
  }
  ExternalEditResult edit;
  const ExternalEditMode mode = (vk == VK_BACK && default_text.empty())
                                    ? ExternalEditMode::kReplaceText
                                    : ExternalEditMode::kInsertText;
  if (!RunExternalEdit(*route->host, route->doc, client, mode, start, end,
                       default_text, &edit, error)) {
    *error = "the host default edit did not run: " + *error;
    return false;
  }
  return true;
}

// One key through the real sink. OnTestKeyDown is called twice: it must be repeatable and
// must have no side effect. OnKeyDown must agree with the prediction. When the product does
// not eat the key, the host applies its default text.
bool SendVirtualKeyAdaptive(InputRoute* route, UINT vk, const std::wstring& host_text,
                            bool* eaten_out, std::string* error) {
  const LPARAM down = KeyLparam(vk, true, false);
  BOOL test_eaten = FALSE;
  HRESULT hr = route->sink->OnTestKeyDown(route->context, vk, down, &test_eaten);
  if (FAILED(hr)) {
    *error = "OnTestKeyDown failed";
    return false;
  }
  BOOL test_again = FALSE;
  hr = route->sink->OnTestKeyDown(route->context, vk, down, &test_again);
  if (FAILED(hr) || (test_again != FALSE) != (test_eaten != FALSE)) {
    *error = "OnTestKeyDown is not repeatable";
    return false;
  }
  BOOL eaten = FALSE;
  hr = route->sink->OnKeyDown(route->context, vk, down, &eaten);
  if (FAILED(hr)) {
    *error = "OnKeyDown failed";
    return false;
  }
  BOOL up_eaten = TRUE;
  hr = route->sink->OnKeyUp(route->context, vk, KeyLparam(vk, false, false), &up_eaten);
  if (FAILED(hr)) {
    *error = "OnKeyUp failed";
    return false;
  }
  if ((test_eaten != FALSE) != (eaten != FALSE)) {
    *error = "OnTestKeyDown and OnKeyDown disagreed";
    std::cerr << "  send mismatch test=" << (test_eaten != FALSE)
              << " key=" << (eaten != FALSE) << " vk=" << vk << std::endl;
    return false;
  }
  if (up_eaten != FALSE) {
    *error = "a key-up was eaten";
    return false;
  }
  *eaten_out = eaten != FALSE;
  if (!host_text.empty()) {
    route->typed_text_units += host_text.size();
    if (*eaten_out) {
      route->product_suppressed_units += host_text.size();
    } else {
      route->host_inserted_units += host_text.size();
    }
  }
  if (!*eaten_out && !ApplyHostDefault(route, vk, host_text, error)) {
    return false;
  }
  return true;
}

bool SendVirtualKey(InputRoute* route, UINT vk, const std::wstring& host_text,
                    bool expect_eaten, std::string* error) {
  bool eaten = false;
  if (!SendVirtualKeyAdaptive(route, vk, host_text, &eaten, error)) {
    return false;
  }
  if (eaten != expect_eaten) {
    *error = expect_eaten ? "a key was not eaten" : "a key was eaten";
    std::cerr << "  send expectation failed expect=" << expect_eaten
              << " key=" << eaten << " vk=" << vk << std::endl;
    return false;
  }
  return true;
}

// Special keys keep a natural host default: Space inserts one space, every other special key
// has no text effect in this controlled host. Text keys use SendChar, which passes the exact
// translated character.
bool SendVirtualKey(InputRoute* route, UINT vk, bool expect_eaten, std::string* error) {
  return SendVirtualKey(route, vk, vk == VK_SPACE ? std::wstring(L" ") : std::wstring(),
                        expect_eaten, error);
}

bool SendChar(InputRoute* route, WCHAR ch, bool expect_eaten, std::string* error) {
  const SHORT mapped = ::VkKeyScanW(ch);
  if (mapped == -1) {
    *error = "the active layout cannot type a test character";
    std::cerr << "  untypable codepoint=" << static_cast<unsigned>(ch) << std::endl;
    return false;
  }
  const UINT vk = static_cast<UINT>(LOBYTE(mapped));
  const BYTE modifiers = HIBYTE(mapped);
  SetStateForKey(ch, (modifiers & 1) != 0, (modifiers & 2) != 0,
                 (modifiers & 4) != 0);
  const bool sent = SendVirtualKey(route, vk, std::wstring(1, ch), expect_eaten, error);
  if (!sent) {
    std::cerr << "  send char failed codepoint=" << static_cast<unsigned>(ch)
              << " vk=" << vk << " shift=" << (modifiers & 1) << std::endl;
  }
  return sent;
}

bool SendCharAdaptive(InputRoute* route, WCHAR ch, bool* eaten, std::string* error) {
  const SHORT mapped = ::VkKeyScanW(ch);
  if (mapped == -1) {
    *error = "the active layout cannot type a test character";
    return false;
  }
  const UINT vk = static_cast<UINT>(LOBYTE(mapped));
  const BYTE modifiers = HIBYTE(mapped);
  SetStateForKey(ch, (modifiers & 1) != 0, (modifiers & 2) != 0,
                 (modifiers & 4) != 0);
  return SendVirtualKeyAdaptive(route, vk, std::wstring(1, ch), eaten, error);
}

bool SendText(InputRoute* route, const std::wstring& text,
              bool expect_eaten, std::string* error) {
  for (const WCHAR ch : text) {
    if (!SendChar(route, ch, expect_eaten, error)) {
      return false;
    }
  }
  return true;
}

bool SendTextWhenReady(InputRoute* route, const std::wstring& text,
                       bool expect_eaten, std::string* error) {
  // Fixed expectations are valid only while the engine path is known to be live. A target
  // revalidation or selection read can legitimately drop and rebuild the binding between
  // two sections; that pass-through is not what these sections test, so wait for readiness
  // again instead of sending into an unbound window.
  if (expect_eaten &&
      !ReadyToType(*route->host, route->sink, route->context, 15000)) {
    *error = "the engine path did not become ready";
    return false;
  }
  return SendText(route, text, expect_eaten, error);
}

bool SendTextAdaptive(InputRoute* route, const std::wstring& text,
                      std::vector<bool>* eaten, std::string* error) {
  for (const WCHAR ch : text) {
    bool key_eaten = false;
    if (!SendCharAdaptive(route, ch, &key_eaten, error)) {
      return false;
    }
    if (eaten != nullptr) {
      eaten->push_back(key_eaten);
    }
  }
  return true;
}

// Independent per-key ownership check: a TRUE answer must never swallow characters. The
// counts come from the test's own keys, not from any product counter.
void CheckNoSuppressedText(const InputRoute& route, const std::string& prefix) {
  std::cout << "  [" << prefix << "] typed=" << route.typed_text_units
            << " host=" << route.host_inserted_units
            << " suppressed=" << route.product_suppressed_units << std::endl;
  LocalCheck(route.product_suppressed_units == 0,
             prefix + ": no text-producing key was swallowed by the product");
}

// Host-owned text: every key must be passed through, and the host applies its own default.
bool SendTextHost(InputRoute* route, const std::wstring& text, std::string* error) {
  std::vector<bool> eaten;
  if (!SendTextAdaptive(route, text, &eaten, error)) {
    return false;
  }
  for (const bool key_was_eaten : eaten) {
    if (key_was_eaten) {
      *error = "a host-owned text key was eaten";
      return false;
    }
  }
  return true;
}

bool SendShortcut(InputRoute* route, UINT vk,
                  bool control, bool alt, bool windows, std::string* error) {
  std::array<std::uint8_t, 256> state = {};
  if (control) {
    state[VK_CONTROL] = 0x80;
    state[VK_LCONTROL] = 0x80;
  }
  if (alt) {
    state[VK_MENU] = 0x80;
    state[VK_LMENU] = 0x80;
  }
  if (windows) {
    state[VK_LWIN] = 0x80;
  }
  ::SetKeyboardState(state.data());
  return SendVirtualKey(route, vk, std::wstring(), false, error);
}

bool ReadyToType(TestHost& host, ITfKeyEventSink* sink, ITfContext* context,
                 DWORD timeout_ms, SeamCounters* last_observed) {
  // In the host-owned model no ordinary key is ever eaten, so readiness is the product's
  // own state: engine pipeline attached, binding open, permission resolved, no suggestion
  // work pending. The prediction call is kept only as a liveness probe.
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  const UINT vk = static_cast<UINT>(LOBYTE(::VkKeyScanW(L'a')));
  for (;;) {
    SeamCounters counters = {};
    const bool have_counters = QuerySeamForSink(sink, &counters);
    if (last_observed != nullptr && have_counters) {
      *last_observed = counters;
    }
    if (have_counters && counters.engine_ready != 0 &&
        counters.coordinator_bound != 0 && counters.gate == 0 &&
        counters.permission == 0 && counters.coordinator_phase == 0 &&
        counters.queue_pending == 0 && counters.queue_in_flight == 0 &&
        counters.suggestion_pending == 0 && counters.suggestion_in_flight == 0) {
      return true;
    }
    std::array<std::uint8_t, 256> state = {};
    ::SetKeyboardState(state.data());
    BOOL probe = FALSE;
    (void)sink->OnTestKeyDown(context, vk, KeyLparam(vk, true, false), &probe);
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    PumpSlice(host, 20);
  }
}

void PrintSeamCounters(const SeamCounters& counters, const char* label) {
  std::cout << "  [" << label << "]"
            << " keys=" << counters.key_downs
            << " eaten=" << counters.eaten
            << " host=" << counters.host_text_keys
            << " sug_q=" << counters.suggestion_queued
            << " sug_sub=" << counters.suggestion_submitted
            << " sug_done=" << counters.suggestion_completed
            << " sug_drop=" << counters.suggestion_dropped
            << " sug_stale=" << counters.suggestion_stale
            << " token=" << counters.host_token_units
            << " bound=" << counters.coordinator_bound
            << " literal=" << counters.literal_key_downs
            << " raw_active=" << counters.raw_active
            << " raw_submitted=" << counters.raw_submitted
            << " raw_unapplied=" << counters.raw_unapplied_units
            << " commits=" << counters.raw_commits
            << " applied=" << counters.raw_applied_units
            << " cancelled=" << counters.raw_cancelled_units
            << " dropped=" << counters.raw_dropped_keys
            << " unrecoverable=" << counters.raw_unrecoverable
            << " entered=" << counters.raw_entered
            << " exits=" << counters.raw_exits
            << " ignored=" << counters.raw_ignored_edits
            << " sug_act=" << counters.suggestion_active
            << " sug_inf=" << counters.suggestion_in_flight
            << " repl=" << counters.replacement_pending
            << " losses=" << counters.internal_text_losses
            << " hedits=" << counters.host_edits_matched
            << "/" << counters.host_edits_unexpected
            << " resets=" << counters.reset_boundary
            << "/" << counters.reset_owed
            << "/" << counters.reset_submit_refusal
            << "/" << counters.reset_replacement
            << " last_edit=" << counters.last_edit_status
            << "/" << counters.last_edit_reason
            << " queue=" << counters.queue_pending
            << "/" << counters.queue_in_flight
            << " phase=" << counters.coordinator_phase
            << " works=" << counters.works_accepted
            << " pumps=" << counters.pumps
            << " engine=" << counters.engine_ready
            << " estate=" << counters.engine_state
            << " eatt=" << counters.engine_attach_failures
            << " comp=" << counters.composition_active
            << "/" << counters.composition_length
            << " binds=" << counters.target_binds
            << " rebinds=" << counters.rebind_requests
            << " menu=" << counters.candidate_menu_open
            << " v=" << counters.candidate_version
            << " items=" << counters.candidate_items
            << " sel=" << counters.candidate_selected
            << " req=" << counters.engine_requests
            << " res=" << counters.engine_results
            << " last_submit=" << counters.last_submit_status
            << " gate=" << counters.gate
            << " perm=" << counters.permission
            << " rawwhy=" << counters.raw_reason_unapplied << ","
            << counters.raw_reason_not_candidate << ","
            << counters.raw_reason_high_water << ","
            << counters.raw_reason_admission
            << std::endl;
}

void PrintModuleState(const ProductSeam& seam, const char* label) {
  long long live = -1;
  long long locks = -1;
  long long host_active = -1;
  long long host_failed = -1;
  long long guarded = -1;
  if (seam.module_state != nullptr) {
    (void)seam.module_state(&live, &locks, &host_active, &host_failed, &guarded);
  }
  std::cout << "  [module] " << label << " live=" << live << " locks=" << locks
            << " host_active=" << host_active << " host_failed=" << host_failed
            << " guarded=" << guarded << std::endl;
}

// ReadyToType answers "would the product eat a plain letter now"; while the degraded run is// still active that is true for a raw capture, not for an engine key. This helper waits for
// the run to be closed, the coordinator to be idle and a bounded engine prediction to be
// positive. It is used only by cases that must exercise the recovered engine path.
bool WaitForEnginePath(TestHost& host, const ProductSeam& seam, ITfKeyEventSink* sink,
                       ITfContext* context, SeamCounters* counters,
                       DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (seam.query != nullptr && seam.query(counters) == 0 &&
        counters->engine_ready != 0 && counters->raw_active == 0 &&
        counters->raw_submitted == 0 && counters->raw_unapplied_units == 0 &&
        counters->queue_pending == 0 && counters->queue_in_flight == 0 &&
        counters->coordinator_phase == 0 &&
        ReadyToType(host, sink, context, 200)) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      if (counters != nullptr) {
        PrintSeamCounters(*counters, "engine path wait timeout");
      }
      return false;
    }
    PumpSlice(host, 20);
  }
}

struct Case {
  TestHost* host = nullptr;
  TestDocument* doc = nullptr;
  LoadedProduct product;
  ProductSeam seam;
  std::string error;
};

bool BeginCase(Case* test_case, const std::filesystem::path& stage,
               const std::filesystem::path& user_root,
               const TestDocumentOptions& options, std::wstring* why) {
  CrashPhase("begin-case: creating the controlled host");
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  test_case->host = TestHost::Create(&test_case->error).release();
  if (test_case->host == nullptr) {
    std::cout << "  host: " << test_case->error << "\n";
    return false;
  }
  test_case->doc = test_case->host->CreateDocument(options, &test_case->error);
  if (test_case->doc == nullptr) {
    std::cout << "  document: " << test_case->error << "\n";
    return false;
  }
  if (!epocanva::tsf::context::test::EnsureTestThreadFocus(&test_case->error)) {
    std::cout << "  focus: " << test_case->error << "\n";
    return false;
  }
  epocanva::tsf::context::test::FocusCallResult focus;
  if (!SetThreadFocus(*test_case->host, test_case->doc, &focus, &test_case->error)) {
    std::cout << "  tsf focus: " << test_case->error << "\n";
    return false;
  }
  std::wstring activate_why;
  CrashPhase("begin-case: activating the product");
  if (!ActivateProduct(&test_case->product, test_case->host->thread_manager_for_test(),
                       stage, &activate_why)) {
    std::cout << "  activate: "
              << std::string(activate_why.begin(), activate_why.end()) << "\n";
    if (why != nullptr) {
      *why = activate_why;
    }
    return false;
  }
  test_case->seam = LoadSeam(test_case->product);
  return true;
}

void EndCase(Case* test_case) {
  if (test_case->host != nullptr) {
    CrashPhase("end-case: deactivating and releasing the product interfaces");
    ReleaseProduct(&test_case->product, true);
    // TSF-owned objects implemented by the DLL (queued edit sessions, composition sinks,
    // candidate elements) are released when the documents are popped, so the host must go
    // first. Only then may the module be freed, and only when it agrees.
    CrashPhase("end-case: tearing down the controlled host");
    delete test_case->host;
    test_case->host = nullptr;
    test_case->doc = nullptr;
    CrashPhase("end-case: waiting for the module to become unloadable");
    if (!FreeProductModule(&test_case->product, 10000, "end-case") &&
        test_case->product.module != nullptr) {
      const auto module_state = reinterpret_cast<SeamModuleStateFn>(
          ::GetProcAddress(test_case->product.module, "EpoCanvaTestModuleState"));
      long long live = -1;
      long long locks = -1;
      long long host_active = -1;
      long long host_failed = -1;
      long long guarded = -1;
      if (module_state != nullptr) {
        (void)module_state(&live, &locks, &host_active, &host_failed, &guarded);
      }
      std::cout << "  [unload] end-case retained: live=" << live
                << " locks=" << locks << " host_active=" << host_active
                << " host_failed=" << host_failed << " guarded=" << guarded
                << std::endl;
      const auto guarded_objects = reinterpret_cast<SeamGuardedObjectsFn>(
          ::GetProcAddress(test_case->product.module, "EpoCanvaTestGuardedObjects"));
      long long kinds[6] = {-1, -1, -1, -1, -1, -1};
      if (guarded_objects != nullptr) {
        (void)guarded_objects(kinds, 6);
      }
      std::cout << "  [unload] kinds edit_session=" << kinds[0]
                << " selection_read=" << kinds[1] << " composition_sink=" << kinds[2]
                << " rect_read=" << kinds[3] << " candidate_element=" << kinds[4]
                << " candidate_sink=" << kinds[5] << std::endl;
    }
  }
}

struct Paths {
  std::filesystem::path stage;
  std::filesystem::path user_root;
  std::filesystem::path marker;
  std::filesystem::path state;
  std::filesystem::path compiled;
};

bool RunRawInputCases(const Paths& paths) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, paths.stage, paths.user_root, options, nullptr)) {
    LocalCheck(false, "raw: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = LoadSeam(test_case.product);
  LocalCheck(seam.usable(), "raw: controlled seam available");
  auto* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "raw: the product controller bound and resolved permission");
  LocalCheck(::GetFileAttributesW(paths.marker.c_str()) == INVALID_FILE_ATTRIBUTES,
             "raw: the deployment marker was retired after the successful start");
  LocalCheck(std::filesystem::is_regular_file(paths.state),
             "raw: the deployment snapshot was recorded");
  LocalCheck(std::filesystem::is_regular_file(paths.compiled),
             "raw: the compiled schema exists in the user engine directory");
  const std::string compiled_text = ReadNarrow(paths.compiled);
  LocalCheck(compiled_text.find("max_completions: 4") != std::string::npos,
             "raw: the compiled schema carries the configured completion count");
  LocalCheck(compiled_text.find("max_suggestions: 2") != std::string::npos,
             "raw: the compiled schema carries the configured suggestion count");

  // Ordinary text is host-owned: the product returns pfEaten=FALSE, and the controlled
  // application performs its own default insertion. Suggestions are asynchronous and can
  // never change whether this text is retained.
  std::string error;
  ReferenceDoc reference;
  LocalCheck(SendTextHost(&route, L"Hello", &error),
             "raw: Hello stays with the host: " + error);
  reference.HostInsert(L"Hello");
  bool space_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "raw: Space stays with the host: " + error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: Hello + Space is in the document exactly once");

  error.clear();
  LocalCheck(SendTextHost(&route, L"helo", &error),
             "raw: helo stays with the host: " + error);
  reference.HostInsert(L"helo");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: the default path does not auto-correct helo");

  error.clear();
  LocalCheck(SendTextHost(&route, L"helo", &error), "raw: helo again: " + error);
  reference.HostInsert(L"helo");
  bool back_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_BACK, L"", &back_eaten, &error) &&
                 !back_eaten,
             "raw: BackSpace stays with the host and deletes: " + error);
  reference.HostBackspace();
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: BackSpace deleted exactly one host code point");

  error.clear();
  LocalCheck(SendTextHost(&route, L"123-45_67@x.y", &error),
             "raw: digits and technical marks stay with the host: " + error);
  reference.HostInsert(L"123-45_67@x.y");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: digits and marks are unchanged");

  error.clear();
  LocalCheck(SendTextHost(&route, L"user@example.com", &error),
             "raw: email stays with the host: " + error);
  reference.HostInsert(L"user@example.com");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text, "raw: email is unchanged");

  error.clear();
  LocalCheck(SendTextHost(&route, L"https://example.com/a?x=1&y=2#z", &error),
             "raw: URL stays with the host: " + error);
  reference.HostInsert(L"https://example.com/a?x=1&y=2#z");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text, "raw: URL is unchanged");

  error.clear();
  LocalCheck(SendTextHost(&route, L"C:\tmp\file.txt", &error),
             "raw: path stays with the host: " + error);
  reference.HostInsert(L"C:\tmp\file.txt");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text, "raw: path is unchanged");

  error.clear();
  LocalCheck(SendTextHost(&route, L"foo_bar123", &error),
             "raw: identifier stays with the host: " + error);
  reference.HostInsert(L"foo_bar123");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: identifier is unchanged");

  // Enter is host-owned. The controlled host inserts the newline its own default defines.
  error.clear();
  LocalCheck(SendTextHost(&route, L"enter", &error), "raw: type enter: " + error);
  reference.HostInsert(L"enter");
  bool enter_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_RETURN, L"", &enter_eaten, &error) &&
                 !enter_eaten,
             "raw: Enter stays with the host: " + error);
  reference.HostInsert(L"\n");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: Enter and Space left the host's full text");

  error.clear();
  LocalCheck(SendShortcut(&route, 'C', true, false, false, &error),
             "raw: Ctrl+C passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'V', true, false, false, &error),
             "raw: Ctrl+V passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'Z', true, false, false, &error),
             "raw: Ctrl+Z passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'X', false, true, false, &error),
             "raw: Alt+X passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'R', false, false, true, &error),
             "raw: Win+R passes to the host: " + error);
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: shortcut pass-through changed no text");

  error.clear();
  LocalCheck(SendTextHost(&route, L"quickbrownfox", &error),
             "raw: fast typing stays with the host: " + error);
  reference.HostInsert(L"quickbrownfox");
  (void)SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: fast typing keeps order and is inserted once");

  // Escape closes an open suggestion menu; it never removes the host's text. Whether the
  // menu happened to be open at this instant, the original text must remain complete.
  error.clear();
  LocalCheck(SendTextHost(&route, L"zzz", &error), "raw: type zzz: " + error);
  reference.HostInsert(L"zzz");
  bool escape_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_ESCAPE, L"", &escape_eaten, &error),
             "raw: Escape is delivered: " + error);
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "raw: Escape left every typed character in the document");

  // No product-owned text exists to lose. The per-key ledger and the product's own
  // accounting must both say so, and the document must equal the independent reference.
  SeamCounters counters = {};
  (void)seam.query(&counters);
  LocalCheck(counters.host_text_keys > 0,
             "raw: ordinary keys were counted as host-owned");
  LocalCheck(counters.internal_text_losses == 0,
             "raw: the product reports zero internal text losses");
  CheckNoSuppressedText(route, "raw");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "raw: the whole session equals the independent reference");

  EndCase(&test_case);
  return g_failures == 0;
}

bool RunPermissionCase(const Paths& paths, TestDocumentOptions options,
                       const std::string& label) {
  Case test_case;
  options.initial_text_utf16 = L"protected";
  options.initial_selection_start_utf16 =
      static_cast<LONG>(options.initial_text_utf16.size());
  options.initial_selection_end_utf16 =
      static_cast<LONG>(options.initial_text_utf16.size());
  if (!BeginCase(&test_case, paths.stage, paths.user_root, options, nullptr)) {
    LocalCheck(false, label + ": host and activation");
    EndCase(&test_case);
    return false;
  }
  PumpSlice(*test_case.host, 500);
  std::string error;
  const UINT vk =
      static_cast<UINT>(LOBYTE(::VkKeyScanW(L'a')));
  BOOL test_eaten = TRUE;
  ITfContext* context = test_case.doc->context();
  const HRESULT hr = test_case.product.sink->OnTestKeyDown(
      context, vk, KeyLparam(vk, true, false), &test_eaten);
  BOOL test_again = TRUE;
  const HRESULT hr_again = test_case.product.sink->OnTestKeyDown(
      context, vk, KeyLparam(vk, true, false), &test_again);
  LocalCheck(SUCCEEDED(hr) && SUCCEEDED(hr_again) &&
                 test_eaten == FALSE && test_again == FALSE,
             label + ": prediction passes through and is repeatable");
  BOOL eaten = TRUE;
  test_case.product.sink->OnKeyDown(context, vk, KeyLparam(vk, true, false), &eaten);
  LocalCheck(eaten == FALSE, label + ": the key is not eaten");
  // pfEaten == FALSE means the host must perform its default input. A read-only store
  // refuses the write; a password/unknown field receives the host's own character, which is
  // exactly what the IME must not be credited for.
  InputRoute route{test_case.host, test_case.doc, test_case.product.sink, context};
  if (eaten == FALSE) {
    if (options.read_only) {
      // The read-only store refuses the host's own write as well; the attempt must not be
      // skipped, and the refusal must leave the document unchanged.
      error.clear();
      (void)ApplyHostDefault(&route, vk, L"a", &error);
    } else {
      LocalCheck(ApplyHostDefault(&route, vk, L"a", &error),
                 label + ": the host default input ran: " + error);
    }
  }
  const std::wstring expected =
      options.read_only ? L"protected" : L"protecteda";
  LocalCheck(test_case.doc->text_utf16() == expected,
             label + (options.read_only
                          ? ": the product wrote nothing and the read-only host refused"
                          : ": only the host's own character was inserted"));
  EndCase(&test_case);
  return true;
}

bool RunExternalEditCase(const Paths& paths) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"base";
  options.initial_selection_start_utf16 = 4;
  options.initial_selection_end_utf16 = 4;
  if (!BeginCase(&test_case, paths.stage, paths.user_root, options, nullptr)) {
    LocalCheck(false, "external edit: host and activation");
    EndCase(&test_case);
    return false;
  }
  auto* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "external edit: ready to type");
  std::string error;
  ReferenceDoc reference;
  reference.text = L"base";
  LocalCheck(SendTextHost(&route, L"abc", &error),
             "external edit: abc stays with the host: " + error);
  reference.HostInsert(L"abc");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "external edit: the host text is visible immediately");
  SeamCounters before_external = {};
  (void)test_case.seam.query(&before_external);
  const TfClientId external =
      epocanva::tsf::context::test::ExternalClientId(
          test_case.host->thread_manager_for_test(), &error);
  ExternalEditResult edit;
  LocalCheck(RunExternalEdit(*test_case.host, test_case.doc, external,
                             ExternalEditMode::kInsertText, 0, 0, L"X", &edit, &error),
             "external edit: another client writes: " + error);
  const bool external_ready = ReadyToType(*test_case.host, sink, context, 10000);
  LocalCheck(external_ready,
             "external edit: the binding is still ready (host text is not a loss)");
  if (!external_ready) {
    SeamCounters stalled = {};
    (void)test_case.seam.query(&stalled);
    PrintSeamCounters(stalled, "external edit readiness timeout");
  }
  SeamCounters after_external_counters = {};
  (void)test_case.seam.query(&after_external_counters);
  LocalCheck(after_external_counters.target_binds == before_external.target_binds,
             "external edit: the host's own edit did not force a rebind");
  const std::wstring after_external = test_case.doc->text_utf16();
  error.clear();
  LocalCheck(SendTextHost(&route, L"def", &error),
             "external edit: typing after the external write stays with the host: " + error);
  reference.text = after_external + L"def";
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "external edit: the host inserted its text after the external write");
  bool space_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "external edit: Space stays with the host: " + error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "external edit: the full host text is intact after the external write");
  EndCase(&test_case);
  return true;
}


struct SessionResult {
  bool ok = false;
  std::wstring text;
  std::size_t suppressed_units = 0;
  SeamCounters counters = {};
  std::string error;
};

void RunSessionWorker(const std::filesystem::path& stage, const std::wstring& word,
                      std::atomic<int>* activated, SessionResult* result) {
  TestHost* host = TestHost::Create(&result->error).release();
  if (host == nullptr) {
    return;
  }
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  options.initial_selection_start_utf16 = 0;
  options.initial_selection_end_utf16 = 0;
  TestDocument* document = host->CreateDocument(options, &result->error);
  HWND window = nullptr;
  if (document == nullptr ||
      !epocanva::tsf::context::test::CreateVisibleTestWindow(&window, &result->error) ||
      ::SetFocus(window) == nullptr) {
    delete host;
    return;
  }
  epocanva::tsf::context::test::FocusCallResult focus;
  if (!SetThreadFocus(*host, document, &focus, &result->error)) {
    ::DestroyWindow(window);
    delete host;
    return;
  }
  LoadedProduct product;
  if (!ActivateProduct(&product, host->thread_manager_for_test(), stage,
                       nullptr)) {
    ::DestroyWindow(window);
    delete host;
    return;
  }
  (void)LoadSeam(product);
  activated->fetch_add(1);
  const ULONGLONG deadline = ::GetTickCount64() + 15000;
  while (activated->load() < 2 && ::GetTickCount64() < deadline) {
    ::Sleep(5);
  }
  ITfContext* context = document->context();
  InputRoute route{host, document, product.sink, context};
  ReferenceDoc reference;
  bool space_eaten = true;
  result->counters.pumps = ~0ull;
  if (!ReadyToType(*host, product.sink, context, 15000, &result->counters)) {
    result->error = "the session never became ready";
  } else if (!SendTextHost(&route, word, &result->error)) {
    result->error = "the session did not keep its word with the host";
  } else if (!SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten,
                                    &result->error) ||
             space_eaten) {
    result->error = "the session's Space was not the host's";
  } else {
    reference.HostInsert(word);
    reference.HostInsert(L" ");
    if (!WaitForText(*host, document, reference.text, 8000)) {
      result->error = "the session did not commit its word";
    } else {
      result->ok = true;
      result->text = document->text_utf16();
      result->suppressed_units = route.product_suppressed_units;
    }
  }
  ReleaseProduct(&product, true);
  ::DestroyWindow(window);
  delete host;
  DeferProductModule(&product);
}

bool RunMultiSessionCase(const std::filesystem::path& stage,
                         const std::filesystem::path& user_root,
                         const epocanva::product::ProductSettings& settings,
                         std::wstring* why) {
  // Force one deployment: both sessions race through the same per-user start lock.
  std::error_code ec;
  std::filesystem::remove_all(user_root / L"engine" / L"build", ec);
  std::filesystem::remove(user_root / L"engine" / L"deployed-state", ec);
  if (!PrepareUserRoot(user_root, settings, why)) {
    return false;
  }
  std::atomic<int> activated{0};
  SessionResult first;
  SessionResult second;
  std::thread a(RunSessionWorker, stage, L"alpha", &activated, &first);
  std::thread b(RunSessionWorker, stage, L"beta", &activated, &second);
  a.join();
  b.join();
  if (!first.ok) {
    std::cout << "  [multi] session A error: " << first.error << std::endl;
    PrintSeamCounters(first.counters, "multi A");
  }
  if (!second.ok) {
    std::cout << "  [multi] session B error: " << second.error << std::endl;
    PrintSeamCounters(second.counters, "multi B");
  }
  LocalCheck(first.ok && first.text == L"alpha " && first.suppressed_units == 0,
             "session A committed its own text with no suppressed key");
  LocalCheck(second.ok && second.text == L"beta " && second.suppressed_units == 0,
             "session B committed its own text with no suppressed key");
  const bool unloaded = FreeDeferredProductModules(15000, "multi-session");
  LocalCheck(unloaded, "multi-session: both load references were released");
  return first.ok && second.ok;
}

bool RunStrictUnregisteredCase(const std::filesystem::path& stage,
                               const std::filesystem::path& user_root) {
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  if (!host) {
    LocalCheck(false, "strict: host created");
    return false;
  }
  TestDocumentOptions options;
  TestDocument* document = host->CreateDocument(options, &error);
  if (document == nullptr ||
      !epocanva::tsf::context::test::EnsureTestThreadFocus(&error)) {
    LocalCheck(false, "strict: document and focus");
    return false;
  }
  epocanva::tsf::context::test::FocusCallResult focus;
  SetThreadFocus(*host, document, &focus, &error);
  LoadedProduct product;
  const bool activated =
      ActivateProduct(&product, host->thread_manager_for_test(), stage, nullptr);
  LocalCheck(!activated,
             "strict product DLL refuses activation when the key sink cannot be advised");
  if (product.sink != nullptr) {
    BOOL eaten = TRUE;
    const UINT vk = static_cast<UINT>(LOBYTE(::VkKeyScanW(L'a')));
    product.sink->OnKeyDown(document->context(), vk, KeyLparam(vk, true, false), &eaten);
    LocalCheck(eaten == FALSE, "strict: a failed activation eats no key");
  }
  ReleaseProduct(&product, false);
  LocalCheck(document->text_utf16().empty(), "strict: the document is unchanged");
  {
    ProductSeam strict_seam = LoadSeam(product);
    PrintModuleState(strict_seam, "strict after releasing the interfaces");
  }
  host.reset();
  // The activation failed after the asynchronous engine start was requested. That start is
  // bounded (60 s) and keeps the module loaded until it exits; the host must wait for the
  // gate instead of exiting with the background thread still inside the DLL.
  const ULONGLONG strict_gate_deadline = ::GetTickCount64() + 75000;
  HRESULT strict_gate = ProductUnloadStatus(&product);
  while (strict_gate != S_OK && ::GetTickCount64() < strict_gate_deadline) {
    ::Sleep(100);
    strict_gate = ProductUnloadStatus(&product);
  }
  LocalCheck(strict_gate == S_OK,
             "strict: the failed activation's background start ended and the gate opened");
  {
    ProductSeam strict_seam = LoadSeam(product);
    PrintModuleState(strict_seam, "strict after the host teardown");
  }
  const bool strict_freed = FreeProductModule(&product, 5000, "strict");
  LocalCheck(strict_freed, "strict: the module is freed after the background start ended");
  return true;
}

struct HoldSessionResult {
  std::atomic<bool> ready{false};
  std::atomic<bool> ok{false};
  SeamCounters counters = {};
  std::string error;
};

void HoldSessionWorker(const std::filesystem::path& stage, HANDLE ready_event,
                       HANDLE release_event, HoldSessionResult* result) {
  TestHost* host = TestHost::Create(&result->error).release();
  if (host == nullptr) {
    ::SetEvent(ready_event);
    return;
  }
  TestDocumentOptions options;
  TestDocument* document = host->CreateDocument(options, &result->error);
  HWND window = nullptr;
  if (document == nullptr ||
      !epocanva::tsf::context::test::CreateVisibleTestWindow(&window, &result->error) ||
      ::SetFocus(window) == nullptr) {
    ::SetEvent(ready_event);
    delete host;
    return;
  }
  epocanva::tsf::context::test::FocusCallResult focus;
  if (!SetThreadFocus(*host, document, &focus, &result->error)) {
    ::SetEvent(ready_event);
    ::DestroyWindow(window);
    delete host;
    return;
  }
  LoadedProduct product;
  if (!ActivateProduct(&product, host->thread_manager_for_test(), stage, nullptr)) {
    result->error = "the deferred session did not activate";
    ::SetEvent(ready_event);
    ::DestroyWindow(window);
    delete host;
    return;
  }
  (void)LoadSeam(product);
  result->ready.store(true);
  result->counters.pumps = ~0ull;
  result->ok.store(
      ReadyToType(*host, product.sink, document->context(), 15000, &result->counters));
  ::SetEvent(ready_event);
  (void)::WaitForSingleObject(release_event, 30000);
  ReleaseProduct(&product, true);
  ::DestroyWindow(window);
  delete host;
  DeferProductModule(&product);
}


bool RunFocusSwitchCase(const Paths& paths) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"parent";
  options.initial_selection_start_utf16 = 6;
  options.initial_selection_end_utf16 = 6;
  if (!BeginCase(&test_case, paths.stage, paths.user_root, options, nullptr)) {
    LocalCheck(false, "focus: host and activation");
    EndCase(&test_case);
    return false;
  }
  auto* sink = test_case.product.sink;
  ITfContext* parent_context = test_case.doc->context();
  LocalCheck(ReadyToType(*test_case.host, sink, parent_context, 15000),
             "focus: the parent context is ready");
  std::string error;
  InputRoute parent_route{test_case.host, test_case.doc, sink, parent_context};
  ReferenceDoc parent_reference;
  parent_reference.text = L"parent";
  bool space_eaten = true;
  LocalCheck(SendTextHost(&parent_route, L"one", &error) &&
                 SendVirtualKeyAdaptive(&parent_route, VK_SPACE, L" ", &space_eaten,
                                        &error) &&
                 !space_eaten,
             "focus: the parent context accepts host-owned text: " + error);
  parent_reference.HostInsert(L"one ");
  CheckDocExact(test_case.doc->text_utf16(), parent_reference.text,
                "focus: the parent text is the exact host reference");
  // A real menu in the parent, so its selection can be attempted after the switch.
  LocalCheck(SendTextHost(&parent_route, L"hel", &error),
             "focus: the parent's suggestion token stays with the host: " + error);
  parent_reference.HostInsert(L"hel");
  SeamCounters parent_menu = {};
  (void)test_case.seam.query(&parent_menu);
  const bool parent_menu_open =
      WaitForCandidateMenu(*test_case.host, test_case.seam, &parent_menu, 2, 8000);
  LocalCheck(parent_menu_open, "focus: the parent menu opened before the switch");
  if (!parent_menu_open) {
    (void)test_case.seam.query(&parent_menu);
    PrintSeamCounters(parent_menu, "focus parent menu timeout");
  }
  const unsigned long long parent_version = parent_menu.candidate_version;

  TestDocumentOptions child_options;
  child_options.initial_text_utf16 = L"";
  child_options.initial_selection_start_utf16 = 0;
  child_options.initial_selection_end_utf16 = 0;
  TestDocument* child =
      test_case.host->CreateChildContext(test_case.doc, child_options, &error);
  LocalCheck(child != nullptr, "focus: a child context is pushed");
  if (child != nullptr) {
    PumpSlice(*test_case.host, 300);
    LocalCheck(ReadyToType(*test_case.host, sink, child->context(), 15000),
               "focus: the child context is bound");
    error.clear();
    InputRoute child_route{test_case.host, child, sink, child->context()};
    ReferenceDoc child_reference;
    bool child_space = true;
    LocalCheck(SendTextHost(&child_route, L"child", &error) &&
                   SendVirtualKeyAdaptive(&child_route, VK_SPACE, L" ", &child_space,
                                          &error) &&
                   !child_space,
               "focus: the child context accepts host-owned text: " + error);
    child_reference.HostInsert(L"child ");
    CheckDocExact(child->text_utf16(), child_reference.text,
                  "focus: the child text is the exact host reference");
    // A selection drawn from the parent menu must not write into the child.
    const std::wstring child_before_parent_click = child->text_utf16();
    (void)test_case.seam.select(parent_version, 1);
    PumpSlice(*test_case.host, 300);
    CheckDocExact(child->text_utf16(), child_before_parent_click,
                  "focus: the parent's stale selection wrote nothing into the child");
    CheckDocExact(test_case.doc->text_utf16(), parent_reference.text,
                  "focus: the parent document kept its own complete text");
    CheckNoSuppressedText(parent_route, "focus parent");
    CheckNoSuppressedText(child_route, "focus child");
    SeamCounters focus_counters = {};
    (void)test_case.seam.query(&focus_counters);
    LocalCheck(focus_counters.internal_text_losses == 0,
               "focus: the product reports zero internal text losses across the switch");
  }
  EndCase(&test_case);
  return true;
}

bool RunSettingsTimingCase(const std::filesystem::path& stage,
                            const std::filesystem::path& user_root,
                            const std::filesystem::path& marker,
                            const std::filesystem::path& compiled,
                            std::wstring* why) {
  epocanva::product::ProductSettings first;
  first.completion_enabled = true;
  first.completion_candidates = 4;
  first.spelling_enabled = false;
  first.spelling_candidates = 2;
  epocanva::product::ProductSettings second;
  second.completion_enabled = true;
  second.completion_candidates = 6;
  second.spelling_enabled = true;
  second.spelling_candidates = 2;

  if (!PrepareUserRoot(user_root, first, why)) {
    return false;
  }
  Paths paths;
  paths.stage = stage;
  paths.user_root = user_root;
  paths.marker = marker;
  paths.compiled = compiled;
  Case held;
  TestDocumentOptions options;
  if (!BeginCase(&held, stage, user_root, options, nullptr)) {
    LocalCheck(false, "timing: the first session activates");
    return false;
  }
  LocalCheck(ReadyToType(*held.host, held.product.sink, held.doc->context(), 15000),
             "timing: the first session is ready");
  const std::string before = ReadNarrow(compiled);
  LocalCheck(before.find("max_completions: 4") != std::string::npos,
             "timing: the first engine compiled the first settings");

  {
    const bool saved = WriteSettingsOnly(user_root, second, why);
    LocalCheck(saved, "timing: new settings are saved while the engine runs: " +
                          std::string(why->begin(), why->end()));
  }
  HANDLE ready_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HoldSessionResult deferred;
  std::thread waiter(HoldSessionWorker, stage, ready_event, release_event, &deferred);
  const bool ready_signalled = ::WaitForSingleObject(ready_event, 25000) == WAIT_OBJECT_0;
  LocalCheck(ready_signalled && deferred.ok.load(),
             "timing: the second session joins the running engine");
  if (!deferred.ok.load()) {
    std::cout << "  [timing] second session error: " << deferred.error << std::endl;
    PrintSeamCounters(deferred.counters, "timing second session");
  }
  LocalCheck(std::filesystem::is_regular_file(marker),
             "timing: the new settings marker survives while the engine runs");
  const std::string during = ReadNarrow(compiled);
  LocalCheck(during.find("max_completions: 4") != std::string::npos,
             "timing: the running engine is not redeployed underneath itself");
  ::SetEvent(release_event);
  waiter.join();
  ::CloseHandle(release_event);
  ::CloseHandle(ready_event);
  // The engine lease release is deliberately off the STA. Wait (bounded) until the engine
  // host is really idle before activating again, so this case tests a restart rather than
  // an asynchronous stop followed by a joined old engine.
  {
    using HostStateFn = int (*)();
    const auto host_state = reinterpret_cast<HostStateFn>(
        ::GetProcAddress(held.product.module, "EpoCanvaTestEngineHostState"));
    (void)held.product.tip->Deactivate();
    const ULONGLONG idle_deadline = ::GetTickCount64() + 10000;
    while (host_state != nullptr && host_state() != 0 &&
           ::GetTickCount64() < idle_deadline) {
      ::Sleep(20);
    }
    LocalCheck(host_state != nullptr && host_state() == 0,
               "timing: the previous engine host is idle before the restart");
  }
  EndCase(&held);
  // The worker session's load reference was parked. Free it now so the restarted case gets a
  // fresh module and a fresh bootstrap singleton, exactly as the settings contract assumes.
  LocalCheck(FreeDeferredProductModules(15000, "settings-timing"),
             "timing: the parked load references were released before the restart");

  Case renewed;
  if (!BeginCase(&renewed, stage, user_root, options, nullptr)) {
    LocalCheck(false, "timing: the restarted engine activates");
    return false;
  }
  LocalCheck(ReadyToType(*renewed.host, renewed.product.sink, renewed.doc->context(),
                         15000),
             "timing: the restarted engine is ready");
  const std::string after = ReadNarrow(compiled);
  LocalCheck(after.find("max_completions: 6") != std::string::npos &&
                 after.find("max_suggestions: 2") != std::string::npos,
             "timing: the restarted engine compiled the saved settings");
  LocalCheck(!std::filesystem::is_regular_file(marker),
             "timing: the marker is retired after the deferred deployment");
  std::string error;
  InputRoute renewed_route{renewed.host, renewed.doc, renewed.product.sink,
                           renewed.doc->context()};
  ReferenceDoc renewed_reference;
  bool renewed_space = true;
  LocalCheck(SendTextHost(&renewed_route, L"timing", &error) &&
                 SendVirtualKeyAdaptive(&renewed_route, VK_SPACE, L" ", &renewed_space,
                                        &error) &&
                 !renewed_space,
             "timing: the restarted engine keeps its input with the host: " + error);
  renewed_reference.HostInsert(L"timing ");
  CheckDocExact(renewed.doc->text_utf16(), renewed_reference.text,
                "timing: the restarted engine's document is the host reference");
  CheckNoSuppressedText(renewed_route, "timing");
  EndCase(&renewed);
  return true;
}


bool RunEngineMissingCase(const std::filesystem::path& stage,
                          const std::filesystem::path& user_root,
                          const std::filesystem::path& marker,
                          const std::filesystem::path& compiled) {
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  if (!host) {
    LocalCheck(false, "missing engine: host created");
    return false;
  }
  TestDocumentOptions options;
  TestDocument* document = host->CreateDocument(options, &error);
  if (document == nullptr) {
    LocalCheck(false, "missing engine: document created");
    return false;
  }
  epocanva::tsf::context::test::EnsureTestThreadFocus(&error);
  epocanva::tsf::context::test::FocusCallResult focus;
  SetThreadFocus(*host, document, &focus, &error);

  LoadedProduct product;
  std::wstring why;
  const bool activated =
      ActivateProduct(&product, host->thread_manager_for_test(), stage, &why);
  LocalCheck(activated,
             "unusable rime.dll: activation succeeds immediately in literal mode");
  ProductSeam seam = LoadSeam(product);
  LocalCheck(seam.startup_usable(), "unusable rime.dll: startup seam available");
  int state = -1;
  const ULONGLONG deadline = ::GetTickCount64() + 15000;
  for (;;) {
    state = seam.state != nullptr ? seam.state() : -1;
    if (state == 3) {  // EngineBootstrapState::kFailed
      break;
    }
    if (::GetTickCount64() >= deadline) {
      break;
    }
    PumpSlice(*host, 20);
  }
  LocalCheck(state == 3,
             "unusable rime.dll: the asynchronous start reports failure, not success");
  InputRoute route{host.get(), document, product.sink, document->context()};
  SeamCounters counters;
  (void)seam.query(&counters);
  LocalCheck(counters.engine_ready == 0,
             "unusable rime.dll: no engine pipeline is claimed");
  LocalCheck(SendText(&route, L"abc", false, &error),
             "unusable rime.dll: every key stays with the host: " + error);
  LocalCheck(document->text_utf16() == L"abc",
             "unusable rime.dll: the literal path delivers the host's own text exactly");
  (void)seam.query(&counters);
  LocalCheck(counters.literal_key_downs >= 3,
             "unusable rime.dll: the literal path is counted");
  LocalCheck(std::filesystem::is_regular_file(marker),
             "unusable rime.dll: the marker is kept for a retry");
  LocalCheck(!std::filesystem::is_regular_file(compiled),
             "unusable rime.dll: no compiled schema is claimed");
  const ULONGLONG stop_start = ::GetTickCount64();
  (void)product.tip->Deactivate();
  LocalCheck(::GetTickCount64() - stop_start < 1000,
             "unusable rime.dll: deactivation does not wait for the failed start");
  ReleaseProduct(&product, false);
  host.reset();
  (void)FreeProductModule(&product, 10000, "engine-missing");
  return true;
}

bool RunCandidateCase(const std::filesystem::path& stage,
                      const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "candidate: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  LocalCheck(seam.usable(), "candidate: controlled seam available");
  if (!seam.usable()) {
    EndCase(&test_case);
    return false;
  }
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  ReferenceDoc reference;
  std::string error;
  SeamCounters counters;

  // 1. The host owns the literal text. Enter with no explicit selection is host input, not
  // an automatic acceptance of the suggestion.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: the engine path is ready");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "candidate: the first token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the real menu opened");
  LocalCheck(counters.page_size == 8 && counters.is_last_page == 1 &&
                 counters.candidate_items <= 8,
             "candidate: the engine's real page size is 8 and this is its last page");
  LocalCheck(seam.kind(0) == 0, "candidate: index 0 is the original text");
  LocalCheck(seam.kind(1) == 1, "candidate: index 1 is a completion");
  bool no_selection_enter_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_RETURN, L"", &no_selection_enter_eaten,
                                    &error) &&
                 !no_selection_enter_eaten,
             "candidate: Enter with no selection stays with the host: " + error);
  reference.HostInsert(L"\n");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "candidate: Enter with no selection did not auto-accept a suggestion");

  // 2. Arrow navigation selects an explicit row; Enter accepts that row through the
  // validated replacement path.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: ready for arrow navigation");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "candidate: the second token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the second menu opened");
  PrintSeamCounters(counters, "candidate second menu");
  bool down1 = false;
  bool down2 = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_DOWN, L"", &down1, &error) && down1,
             "candidate: the first Down is eaten by the menu");
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_DOWN, L"", &down2, &error) && down2,
             "candidate: the second Down is eaten by the menu");
  (void)seam.query(&counters);
  LocalCheck(counters.candidate_selected == 1,
             "candidate: the explicit selection is the completion row");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 8000),
             "candidate: the selection menu is stable before Enter");
  const std::wstring completion = ReadCandidateText(seam, 1);
  LocalCheck(!completion.empty(), "candidate: the completion text is available");
  LocalCheck(SendVirtualKey(&route, VK_RETURN, true, &error),
             "candidate: Enter accepts the explicit selection");
  reference.ReplaceToken(L"hel", completion);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 8000),
             "candidate: the selected completion replaced the token exactly once");

  // 3. Space is host input even while a completion is selected.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: ready for the Space contract");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "candidate: the third token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the third menu opened");
  bool down3 = false;
  bool down4 = false;
  (void)SendVirtualKeyAdaptive(&route, VK_DOWN, L"", &down3, &error);
  (void)SendVirtualKeyAdaptive(&route, VK_DOWN, L"", &down4, &error);
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "candidate: Space stays with the host while the menu is open: " + error);
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "candidate: Space inserted the host's space, not the selected completion");

  // 4. Escape dismisses the menu and cancels only the enhancement; the typed token stays.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: ready for Escape");
  LocalCheck(SendTextHost(&route, L"wor", &error),
             "candidate: the Escape token stays with the host: " + error);
  reference.HostInsert(L"wor");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the Escape menu opened");
  bool escape_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_ESCAPE, L"", &escape_eaten, &error) &&
                 escape_eaten,
             "candidate: Escape is eaten to dismiss the menu");
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 8000),
             "candidate: Escape kept every typed character");
  PumpSlice(*test_case.host, 300);
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "candidate: no late write followed the dismiss");
  bool separator2_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &separator2_eaten, &error) &&
                 !separator2_eaten,
             "candidate: the second separator space stays with the host");
  reference.HostInsert(L" ");

  // 5. Stale selection: a click drawn from an older menu must not commit into the newer one.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: ready for the stale-selection case");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "candidate: the stale-selection token stays with the host: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "candidate: the stale-selection token is exactly in the document");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the stale-selection menu opened");
  const unsigned long long old_version = counters.candidate_version;
  bool extra_eaten = true;
  LocalCheck(SendCharAdaptive(&route, L'l', &extra_eaten, &error) && !extra_eaten,
             "candidate: another character stays with the host");
  reference.HostInsert(L"l");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "candidate: the extra character is exactly in the document");
  const std::uint64_t set_text_before_stable =
      test_case.doc->diagnostics().store.set_text_calls;
  const bool stable_ok =
      WaitForStableMenu(*test_case.host, seam, &counters, 8000) &&
      counters.candidate_version > old_version;
  std::cout << "  [debug] after stable doc=" << test_case.doc->text_utf16().size()
            << " ref=" << reference.text.size()
            << " set_text_delta="
            << (test_case.doc->diagnostics().store.set_text_calls - set_text_before_stable)
            << " caret=" << test_case.doc->selection_start_utf16()
            << " v=" << counters.candidate_version
            << " input=" << counters.candidate_input_units
            << " host_token=" << counters.host_token_units
            << " plans=" << counters.plans_submitted
            << " eapplied=" << counters.edits_applied
            << " erefused=" << counters.edits_refused << std::endl;
  LocalCheck(stable_ok, "candidate: a newer stable menu exists before the stale click");
  const unsigned long long newer_version = counters.candidate_version;
  const std::wstring before_stale = test_case.doc->text_utf16();
  (void)seam.select(old_version, 1);
  PumpSlice(*test_case.host, 300);
  SeamCounters after_stale;
  (void)seam.query(&after_stale);
  LocalCheck(test_case.doc->text_utf16() == before_stale,
             "candidate: a stale selection changed nothing in the document");
  LocalCheck(after_stale.candidate_stale >= 1,
             "candidate: the stale selection is reported");
  const std::wstring newer_completion = ReadCandidateText(seam, 1);
  const std::size_t before_newer = test_case.doc->text_utf16().size();
  (void)seam.select(newer_version, 1);
  reference.ReplaceToken(L"hell", newer_completion);
  (void)WaitForText(*test_case.host, test_case.doc, reference.text, 8000);
  {
    SeamCounters after_newer = {};
    (void)seam.query(&after_newer);
    std::cout << "  [debug] newer v=" << newer_version
              << " completion_len=" << newer_completion.size()
              << " before=" << before_newer
              << " after=" << test_case.doc->text_utf16().size()
              << " ref=" << reference.text.size()
              << " snapshot_caret=" << after_newer.candidate_caret
              << " input_units=" << after_newer.candidate_input_units
              << " host_token=" << after_newer.host_token_units
              << " origin=" << after_newer.candidate_token_origin_caret << "/"
              << after_newer.candidate_token_origin_valid
              << " expected=" << after_newer.candidate_anchor_expected
              << " anchor=" << after_newer.candidate_anchor_caret
              << " predicted=" << after_newer.predicted_host_caret
              << " retries=" << after_newer.candidate_anchor_retries
              << " doc_caret=" << test_case.doc->selection_start_utf16()
              << " attempts=" << after_newer.replacement_attempts
              << " applied=" << after_newer.replacement_applied
              << " refused=" << after_newer.replacement_refused
              << " stale=" << after_newer.candidate_stale
              << " v_now=" << after_newer.candidate_version
              << " menu_now=" << after_newer.candidate_menu_open
              << " submit=" << after_newer.last_submit_status
              << " stale_now=" << after_newer.suggestion_stale_now
              << " abandoned=" << after_newer.token_abandoned
              << " outcome=" << after_newer.work_outcome
              << " bound=" << after_newer.coordinator_bound
              << " invalid=" << after_newer.binding_invalidations << "/"
              << after_newer.binding_last_loss_reason
              << " reval=" << after_newer.target_revalidations << "/"
              << after_newer.target_refusals << "/"
              << after_newer.last_target_revalidation
              << " plans=" << after_newer.plans_submitted
              << " eapplied=" << after_newer.edits_applied
              << " erefused=" << after_newer.edits_refused
              << " hr=" << std::hex << after_newer.last_edit_request_hr << "/"
              << after_newer.last_edit_session_hr << "/"
              << after_newer.last_edit_operation_hr << std::dec
              << " cb=" << after_newer.target_callback_checks << "/"
              << after_newer.target_rejections
              << " perm=" << after_newer.permission_rechecks
              << " repl=" << after_newer.last_replacement_outcome << "/"
              << after_newer.last_replacement_edit_status << "/"
              << after_newer.last_replacement_edit_reason << "/"
              << after_newer.last_replacement_expected_units
              << " plan_caret=" << after_newer.last_suggestion_expected_caret
              << " live_caret=" << after_newer.last_edit_observed_caret
              << " last_edit=" << after_newer.last_edit_status << "/"
              << after_newer.last_edit_reason << std::endl;
  }
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "candidate: the current selection replaced the token exactly once");
  bool separator3_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &separator3_eaten, &error) &&
                 !separator3_eaten,
             "candidate: the third separator space stays with the host");
  reference.HostInsert(L" ");
  std::cout << "  [debug] after separator3 doc=" << test_case.doc->text_utf16().size()
            << " ref=" << reference.text.size()
            << " caret=" << test_case.doc->selection_start_utf16() << std::endl;

  // 6. A genuinely external edit (not one of our passed-through keys) ends the suggestion
  // stream; a selection from before it must write nothing.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "candidate: ready for the external-edit case");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "candidate: the external-edit token stays with the host: " + error);
  reference.HostInsert(L"hel");
  std::cout << "  [debug] after external token doc=" << test_case.doc->text_utf16().size()
            << " ref=" << reference.text.size()
            << " caret=" << test_case.doc->selection_start_utf16() << std::endl;
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the external-edit menu opened");
  const unsigned long long pre_edit_version = counters.candidate_version;
  SeamCounters before_external = {};
  (void)seam.query(&before_external);
  std::string client_error;
  const TfClientId external_client =
      epocanva::tsf::context::test::ExternalClientId(
          test_case.host->thread_manager_for_test(), &client_error);
  const LONG caret = test_case.doc->selection_start_utf16();
  ExternalEditResult external;
  LocalCheck(external_client != 0 &&
                 RunExternalEdit(*test_case.host, test_case.doc, external_client,
                                 ExternalEditMode::kInsertText, caret, caret, L"X",
                                 &external, &error),
             "candidate: the external edit ran: " + error);
  std::cout << "  [debug] external caret=" << caret
            << " doc_len=" << test_case.doc->text_utf16().size()
            << " ref_len=" << reference.text.size() << std::endl;
  reference.text.insert(static_cast<std::size_t>(caret), L"X");
  PumpSlice(*test_case.host, 600);
  const std::wstring after_external = test_case.doc->text_utf16();
  std::cout << "  [debug] after_external=" << after_external.size()
            << " ref_after=" << reference.text.size() << std::endl;
  CheckDocExact(after_external, reference.text,
                "candidate: the external edit added its own text");
  (void)seam.select(pre_edit_version, 0);
  PumpSlice(*test_case.host, 300);
  SeamCounters after_external_counters;
  (void)seam.query(&after_external_counters);
  LocalCheck(test_case.doc->text_utf16() == after_external,
             "candidate: a selection from before the external edit wrote nothing");
  LocalCheck(after_external_counters.candidate_stale > before_external.candidate_stale ||
                 after_external_counters.replacement_refused >
                     before_external.replacement_refused,
             "candidate: the pre-edit selection is reported stale/refused");
  bool separator4_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &separator4_eaten, &error) &&
                 !separator4_eaten,
             "candidate: the space after the external edit stays with the host");
  reference.HostInsert(L" ");

  // 7. Spelling suggestions are labelled distinctly, and the literal row stays index 0.
  const bool spelling_ready =
      ReadyToType(*test_case.host, sink, context, 15000);
  if (!spelling_ready) {
    SeamCounters blocked = {};
    (void)seam.query(&blocked);
    PrintSeamCounters(blocked, "spelling readiness timeout");
  }
  LocalCheck(spelling_ready, "candidate: ready for the spelling classification");
  LocalCheck(SendTextHost(&route, L"heloo", &error),
             "candidate: the misspelling token stays with the host: " + error);
  reference.HostInsert(L"heloo");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 8000),
             "candidate: the spelling menu opened");
  PrintSeamCounters(counters, "spelling menu");
  std::cout << "  [debug] spelling host_edits matched=" << counters.host_edits_matched
            << " unexpected=" << counters.host_edits_unexpected
            << " losses=" << counters.target_losses
            << " awaiting=" << counters.observer_awaiting_bind
            << " published=" << counters.observer_published
            << " last_drop=" << counters.observer_last_drop
            << " rebind_pending=" << counters.rebind_pending
            << " bind_status=" << counters.last_bind_status
            << " refused=" << counters.binds_refused
            << " paused=" << counters.binds_paused_retiring
            << " reset(boundary=" << counters.reset_boundary
            << ",owed=" << counters.reset_owed
            << ",submit=" << counters.reset_submit_refusal
            << ",replace=" << counters.reset_replacement << ")"
            << " obs(refusals=" << counters.observer_selection_refusals
            << ",reads=" << counters.observer_selection_reads
            << ",pending=" << counters.observer_selection_pending
            << ",ctx=" << counters.observer_has_context
            << ",sub=" << counters.observer_subscribed << ")"
            << " retire=" << counters.retiring_bindings
            << " paused=" << counters.admission_paused
            << " retr(comps=" << counters.retiring_compositions
            << ",settled=" << counters.retired_settled
            << ",ordinary=" << counters.retired_already_ordinary
            << ",blocked=" << counters.retired_blocked
            << ",close_req=" << counters.close_requests
            << ",close_res=" << counters.close_results
            << ",close_fail=" << counters.close_failures
            << ",unres_close=" << counters.unresolved_close
            << ",unres_eng=" << counters.unresolved_engine
            << ",edit_evt=" << counters.pending_edit_event << ")" << std::endl;
  bool spelling_seen = false;
  for (int index = 0; index < counters.candidate_items; ++index) {
    const int kind = seam.kind(index);
    const std::wstring item = ReadCandidateText(seam, static_cast<unsigned>(index));
    std::cout << "  [debug] spelling item " << index << " kind=" << kind
              << " text=" << std::string(item.begin(), item.end()) << std::endl;
    if (kind == 2) {
      spelling_seen = true;
    }
  }
  LocalCheck(seam.kind(0) == 0, "candidate: the literal row is still index 0");
  LocalCheck(spelling_seen,
             "candidate: the engine's spelling suggestion is labelled as such");
  bool spelling_escape_eaten = false;
  (void)SendVirtualKeyAdaptive(&route, VK_ESCAPE, L"", &spelling_escape_eaten, &error);
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "candidate: dismiss left the misspelled original intact");

  EndCase(&test_case);
  return true;
}

void DumpReadyState(const SeamCounters& c, const char* label) {
  std::cout << "  [ready-fail " << label << "] bound=" << c.coordinator_bound
            << " gate=" << c.gate << " perm=" << c.permission
            << " phase=" << c.coordinator_phase
            << " engine=" << c.engine_ready
            << " losses=" << c.target_losses
            << " rebinds=" << c.rebind_requests
            << " bind_status=" << c.last_bind_status
            << " refused=" << c.binds_refused
            << " observer_drop=" << c.observer_last_drop
            << " awaiting=" << c.observer_awaiting_bind
            << " sel_reads=" << c.observer_selection_reads
            << " sel_refusals=" << c.observer_selection_refusals
            << std::endl;
}

bool RequireReady(TestHost& host, ITfKeyEventSink* sink, ITfContext* context,
                  const char* label) {
  SeamCounters counters = {};
  const bool ready = ReadyToType(host, sink, context, 20000, &counters);
  LocalCheck(ready, label);
  if (!ready) {
    DumpReadyState(counters, label);
  }
  return ready;
}

// Waits for a submitted replacement to reach a terminal state without guessing: the
// controller reports pending work, the coordinator phase and the queue; all must be idle.
bool WaitForReplacementSettled(TestHost& host, const ProductSeam& seam,
                               DWORD timeout_ms, SeamCounters* counters) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    SeamCounters current = {};
    if (seam.query != nullptr && seam.query(&current) == 0) {
      if (counters != nullptr) {
        *counters = current;
      }
      if (current.replacement_pending == 0 && current.coordinator_phase == 0 &&
          current.queue_pending == 0 && current.queue_in_flight == 0 &&
          current.suggestion_pending == 0 && current.suggestion_in_flight == 0) {
        return true;
      }
    }
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    PumpSlice(host, 10);
  }
}

// B-layer fault injection for the only product document write: an explicit candidate
// replacement. The oracle is the test's own ReferenceDoc plus the controlled store's
// backing text; product counters are printed as evidence, never used to trim expectations.
bool RunReplacementFaultCase(const std::filesystem::path& stage,
                             const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "faults: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  LocalCheck(seam.usable(), "faults: controlled seam available");
  if (!seam.usable()) {
    EndCase(&test_case);
    return false;
  }
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  ReferenceDoc reference;
  std::string error;
  SeamCounters counters = {};
  const TfClientId app_client = test_case.host->client_id();

  LocalCheck(ReadyToType(*test_case.host, sink, context, 20000),
             "faults: the engine path is ready");

  // A. The same token text exists earlier in the document. The menu is drawn for the last
  // occurrence; after a real caret move the old selection must write nothing.
  LocalCheck(SendTextHost(&route, L"hel ", &error), "faults A: seed text: " + error);
  reference.HostInsert(L"hel ");
  LocalCheck(SendTextHost(&route, L"x ", &error), "faults A: separator: " + error);
  reference.HostInsert(L"x ");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults A: token stays with host: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults A: the duplicate token is exactly in the document");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults A: the menu is stable");
  const unsigned long long duplicate_version = counters.candidate_version;
  const std::wstring duplicate_completion = ReadCandidateText(seam, 1);
  const std::size_t duplicate_before = test_case.doc->text_utf16().size();
  const std::uint64_t duplicate_set_text =
      test_case.doc->diagnostics().store.set_text_calls;
  LocalCheck(SUCCEEDED(test_case.doc->SetSelectionForTest(app_client, 3)),
             "faults A: the caret moved to the earlier occurrence");
  PumpSlice(*test_case.host, 300);
  (void)seam.select(duplicate_version, 1);
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults A: the selection from the old caret wrote nothing");
  LocalCheck(test_case.doc->text_utf16().size() == duplicate_before,
             "faults A: the document length is unchanged");
  LocalCheck(test_case.doc->diagnostics().store.set_text_calls == duplicate_set_text,
             "faults A: no SetText was issued");
  LocalCheck(!duplicate_completion.empty(), "faults A: the menu had a real completion");
  LocalCheck(SUCCEEDED(test_case.doc->SetSelectionForTest(
                 app_client, static_cast<LONG>(reference.text.size()))),
             "faults A: the caret returned to the end");
  PumpSlice(*test_case.host, 200);
  bool a_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &a_space, &error) &&
                 !a_space,
             "faults A: boundary stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults A: the boundary text is exact");

  // B. A replacement is queued (deferred lock) while the host keeps editing. The queued
  // plan must not overwrite the host's newer input, and must not issue SetText.
  RequireReady(*test_case.host, sink, context, "faults B: ready");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults B: token: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults B: the menu is stable");
  const unsigned long long queued_version = counters.candidate_version;
  const std::uint64_t queued_set_text = test_case.doc->diagnostics().store.set_text_calls;
  test_case.doc->store()->SetAsyncLocks(true);
  (void)seam.select(queued_version, 1);
  LocalCheck(SendTextHost(&route, L"ab", &error),
             "faults B: the host keeps typing while the write is queued: " + error);
  reference.HostInsert(L"ab");
  SeamCounters after_queued = {};
  LocalCheck(WaitForReplacementSettled(*test_case.host, seam, 8000, &after_queued),
             "faults B: the queued replacement reached a terminal state");
  test_case.doc->store()->SetAsyncLocks(false);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults B: the newer host input was not overwritten");
  // The host wrote the two pending characters through ITfRange::SetText itself, so the
  // store must show exactly those two calls and no third (product) write.
  LocalCheck(test_case.doc->diagnostics().store.set_text_calls == queued_set_text + 2,
             "faults B: only the host's two SetText calls reached the store");
  LocalCheck(after_queued.replacement_refused >= 1,
             "faults B: the queued replacement is reported refused");
  bool b_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &b_space, &error) &&
                 !b_space,
             "faults B: boundary stays with the host: " + error);
  reference.HostInsert(L" ");

  // C. The token text changes after the menu was drawn. Read-back comparison must refuse,
  // and no SetText may run.
  RequireReady(*test_case.host, sink, context, "faults C: ready");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults C: token: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text, "faults C: token exact");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults C: the menu is stable");
  const unsigned long long mismatch_version = counters.candidate_version;
  const std::uint64_t mismatch_set_text =
      test_case.doc->diagnostics().store.set_text_calls;
  const LONG mismatch_caret = test_case.doc->selection_start_utf16();
  std::string client_error;
  const TfClientId external_client =
      epocanva::tsf::context::test::ExternalClientId(
          test_case.host->thread_manager_for_test(), &client_error);
  ExternalEditResult external;
  LocalCheck(external_client != 0 &&
                 RunExternalEdit(*test_case.host, test_case.doc, external_client,
                                 ExternalEditMode::kReplaceText, mismatch_caret - 3,
                                 mismatch_caret, L"hex", &external, &error),
             "faults C: the external token edit ran: " + error);
  reference.ReplaceToken(L"hel", L"hex");
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults C: the external edit owns the new text");
  (void)seam.select(mismatch_version, 1);
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults C: the read-back mismatch wrote nothing");
  // The external replacement is one host SetText; the stale product selection must add
  // none.
  LocalCheck(test_case.doc->diagnostics().store.set_text_calls == mismatch_set_text + 1,
             "faults C: only the host's external SetText reached the store");
  bool c_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &c_space, &error) &&
                 !c_space,
             "faults C: boundary stays with the host: " + error);
  reference.HostInsert(L" ");

  // D. Focus switches while the replacement is queued. Neither document may receive a
  // write; the old target's pending plan must become stale.
  RequireReady(*test_case.host, sink, context, "faults D: ready");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults D: token: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text, "faults D: token exact");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults D: the menu is stable");
  const unsigned long long focus_version = counters.candidate_version;
  const std::uint64_t focus_set_text = test_case.doc->diagnostics().store.set_text_calls;
  test_case.doc->store()->SetAsyncLocks(true);
  (void)seam.select(focus_version, 1);
  TestDocumentOptions child_options;
  child_options.initial_text_utf16 = L"";
  TestDocument* child =
      test_case.host->CreateChildContext(test_case.doc, child_options, &error);
  LocalCheck(child != nullptr, "faults D: a child context was pushed: " + error);
  if (child != nullptr) {
    PumpSlice(*test_case.host, 800);
    test_case.doc->store()->SetAsyncLocks(false);
    SeamCounters after_focus = {};
    (void)WaitForReplacementSettled(*test_case.host, seam, 8000, &after_focus);
    CheckDocExact(test_case.doc->text_utf16(), reference.text,
                  "faults D: the parent kept its text with no queued write");
    CheckDocExact(child->text_utf16(), L"",
                  "faults D: the child never received the parent's replacement");
    LocalCheck(test_case.doc->diagnostics().store.set_text_calls == focus_set_text,
               "faults D: the stale queued replacement issued no SetText");
    (void)child->Disconnect();
    PumpSlice(*test_case.host, 500);
    RequireReady(*test_case.host, sink, context, "faults D: the parent context is bound again");
  }
  bool d_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &d_space, &error) &&
                 !d_space,
             "faults D: boundary stays with the host: " + error);
  reference.HostInsert(L" ");

  // F. The caret becomes a non-collapsed selection after the menu was drawn. The old
  // selection must write nothing: the writer refuses a non-collapsed selection and the
  // controller refuses a menu whose token is no longer at the live caret.
  RequireReady(*test_case.host, sink, context, "faults F: ready");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults F: token: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text, "faults F: token exact");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults F: the menu is stable");
  const unsigned long long range_version = counters.candidate_version;
  const std::wstring range_before = test_case.doc->text_utf16();
  const std::uint64_t range_set_text = test_case.doc->diagnostics().store.set_text_calls;
  const LONG range_end = test_case.doc->selection_start_utf16();
  const LONG range_start = range_end - 3;
  LocalCheck(SUCCEEDED(test_case.doc->SetSelectionRangeForTest(app_client, range_start,
                                                              range_end)),
             "faults F: the selection became a real non-collapsed range");
  PumpSlice(*test_case.host, 300);
  (void)seam.select(range_version, 1);
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), range_before,
                "faults F: the selection from before the range wrote nothing");
  LocalCheck(test_case.doc->diagnostics().store.set_text_calls == range_set_text,
             "faults F: no SetText was issued for a non-collapsed selection");
  LocalCheck(SUCCEEDED(test_case.doc->SetSelectionForTest(
                 app_client, static_cast<LONG>(reference.text.size()))),
             "faults F: the caret returned to the end");
  PumpSlice(*test_case.host, 500);
  RequireReady(*test_case.host, sink, context, "faults F: the binding recovered");
  bool f_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &f_space, &error) &&
                 !f_space,
             "faults F: boundary stays with the host: " + error);
  reference.HostInsert(L" ");

  // E. The write applies and then reports failure: the product must not roll back, must
  // count an unverifiable loss, must stop enhancement writes on this context, and must keep
  // the host's ordinary input working. A fresh context is the explicit recovery path.
  RequireReady(*test_case.host, sink, context, "faults E: ready");
  LocalCheck(SendTextHost(&route, L"hel", &error), "faults E: token: " + error);
  reference.HostInsert(L"hel");
  CheckDocExact(test_case.doc->text_utf16(), reference.text, "faults E: token exact");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 10000),
             "faults E: the menu is stable");
  const unsigned long long uncertain_version = counters.candidate_version;
  const std::wstring uncertain_completion = ReadCandidateText(seam, 1);
  const std::uint64_t uncertain_set_text = test_case.doc->diagnostics().store.set_text_calls;
  const std::uint64_t uncertain_losses = counters.internal_text_losses;
  test_case.doc->store()->SetMutationFault(
      epocanva::tsf::edit::test::MutationFault::kEFailAfterMutation);
  (void)seam.select(uncertain_version, 1);
  SeamCounters after_uncertain = {};
  LocalCheck(WaitForReplacementSettled(*test_case.host, seam, 10000, &after_uncertain),
             "faults E: the uncertain write reached a terminal state");
  test_case.doc->store()->SetMutationFault(
      epocanva::tsf::edit::test::MutationFault::kNone);
  reference.ReplaceToken(L"hel", uncertain_completion);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults E: the possibly-successful write is not rolled back or duplicated");
  LocalCheck(test_case.doc->diagnostics().store.set_text_calls == uncertain_set_text + 1,
             "faults E: exactly one write reached the store");
  LocalCheck(after_uncertain.internal_text_losses == uncertain_losses + 3,
             "faults E: the unverifiable token is counted as an internal loss");
  LocalCheck(after_uncertain.replacement_refused >= 1,
             "faults E: the uncertain replacement is reported refused");
  LocalCheck(after_uncertain.gate != 0,
             "faults E: the context gate is closed for further enhancements");

  // The host keeps its ordinary input even while the enhancement path is locked down.
  const std::uint64_t uncertain_losses_after = after_uncertain.internal_text_losses;
  LocalCheck(SendTextHost(&route, L"ok", &error),
             "faults E: ordinary text still reaches the host: " + error);
  reference.HostInsert(L"ok");
  bool e_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &e_space, &error) &&
                 !e_space,
             "faults E: boundary stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults E: the host's text is complete after the uncertain write");
  CheckNoSuppressedText(route, "faults E host input");
  PumpSlice(*test_case.host, 300);
  SeamCounters after_host_text = {};
  (void)seam.query(&after_host_text);
  LocalCheck(after_host_text.candidate_menu_open == 0,
             "faults E: no new enhancement menu opened on the locked-down context");
  LocalCheck(after_host_text.internal_text_losses == uncertain_losses_after,
             "faults E: ordinary host input was not counted as a product loss");
  (void)seam.select(uncertain_version, 1);
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "faults E: a later selection on the locked context wrote nothing");

  // Recovery: a new context has its own permission and uncertainty state. The parent stays
  // latched; the child must bind, accept host text and apply a real candidate.
  TestDocumentOptions recover_options;
  recover_options.initial_text_utf16 = L"";
  TestDocument* recovered =
      test_case.host->CreateChildContext(test_case.doc, recover_options, &error);
  LocalCheck(recovered != nullptr, "faults E: recovery child context: " + error);
  if (recovered != nullptr) {
    LocalCheck(ReadyToType(*test_case.host, sink, recovered->context(), 20000),
               "faults E: the new context is bound and suggestible");
    InputRoute recovered_route{test_case.host, recovered, sink, recovered->context()};
    ReferenceDoc recovered_reference;
    LocalCheck(SendTextHost(&recovered_route, L"hel", &error),
               "faults E: recovery token stays with the host: " + error);
    recovered_reference.HostInsert(L"hel");
    SeamCounters recovery_menu = {};
    LocalCheck(WaitForStableMenu(*test_case.host, seam, &recovery_menu, 10000),
               "faults E: the recovery menu opened");
    const std::wstring recovery_completion = ReadCandidateText(seam, 1);
    (void)seam.select(recovery_menu.candidate_version, 1);
    recovered_reference.ReplaceToken(L"hel", recovery_completion);
    (void)WaitForText(*test_case.host, recovered, recovered_reference.text, 10000);
    CheckDocExact(recovered->text_utf16(), recovered_reference.text,
                  "faults E: the new context applied its own candidate");
    CheckNoSuppressedText(recovered_route, "faults E recovery");
    (void)recovered->Disconnect();
  }

  EndCase(&test_case);
  LocalCheck(test_case.product.module == nullptr,
             "faults: the product module was unloaded after the host teardown");
  return true;
}

// After a suggestion drop the remaining keys of the same token must not be presented as a
// complete prefix. The controller abandons that token until a boundary, then resynchronises
// from the current caret and accepts a fresh token normally.
bool RunSuggestionResyncCase(const std::filesystem::path& stage,
                             const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "resync: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  ReferenceDoc reference;
  std::string error;
  SeamCounters counters = {};
  LocalCheck(seam.usable(), "resync: controlled seam available");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 20000),
             "resync: the engine path is ready");

  // One long token, sent without pumping, overflows the bounded suggestion queue and makes
  // the stream stale. Every character still belongs to the host.
  std::wstring burst;
  for (int repeat = 0; repeat < 12; ++repeat) {
    burst += L"abcdefghij";
  }
  LocalCheck(SendTextHost(&route, burst, &error),
             "resync: the burst token stays with the host: " + error);
  reference.HostInsert(burst);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "resync: the burst is exactly in the document");
  CheckNoSuppressedText(route, "resync burst");

  // The reset owed after the drop must settle; the abandoned flag must remain set.
  bool settled = false;
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  for (;;) {
    if (seam.query(&counters) == 0 && counters.token_abandoned != 0 &&
        counters.suggestion_stale_now == 0 && counters.suggestion_in_flight == 0 &&
        counters.queue_pending == 0 && counters.queue_in_flight == 0) {
      settled = true;
      break;
    }
    if (::GetTickCount64() >= deadline) {
      break;
    }
    PumpSlice(*test_case.host, 10);
  }
  LocalCheck(settled, "resync: the dropped token settled into the abandoned state");
  LocalCheck(counters.suggestion_dropped > 0,
             "resync: the bounded stream really dropped suggestions");
  LocalCheck(counters.candidate_menu_open == 0,
             "resync: no menu is offered for the incomplete prefix");

  // More characters of the same token must not start a suffix-only menu.
  LocalCheck(SendTextHost(&route, L"tail", &error),
             "resync: the continuation stays with the host: " + error);
  reference.HostInsert(L"tail");
  PumpSlice(*test_case.host, 800);
  SeamCounters after_suffix = {};
  (void)seam.query(&after_suffix);
  LocalCheck(after_suffix.token_abandoned != 0,
             "resync: the token stays abandoned after the suffix keys");
  LocalCheck(after_suffix.candidate_menu_open == 0,
             "resync: the suffix is not presented as a complete prefix");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "resync: the suffix text is intact");

  // The boundary clears the abandonment; the next token is a completely new prefix.
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "resync: the boundary stays with the host: " + error);
  reference.HostInsert(L" ");
  SeamCounters after_boundary = {};
  (void)seam.query(&after_boundary);
  LocalCheck(after_boundary.token_abandoned == 0,
             "resync: the boundary clears the abandoned token");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "resync: the fresh token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 15000),
             "resync: a real menu opens for the fresh token");
  const std::wstring completion = ReadCandidateText(seam, 1);
  (void)seam.select(counters.candidate_version, 1);
  reference.ReplaceToken(L"hel", completion);
  (void)WaitForText(*test_case.host, test_case.doc, reference.text, 10000);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "resync: the fresh token accepted its own completion");
  CheckNoSuppressedText(route, "resync total");
  EndCase(&test_case);
  return true;
}

bool RunContinuousCase(const std::filesystem::path& stage,
                        const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "continuous: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = LoadSeam(test_case.product);
  LocalCheck(seam.usable(), "continuous: controlled seam available");
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  std::string error;
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "continuous: ready exactly once before the whole burst");

  // One readiness check above covers the whole run. Words are never checked for readiness
  // again; the only pump between words lets TSF deliver its normal notifications. The first
  // segment is sent with every grant held, which forces a real raw catch-up and a binding
  // rebuild; the rest follows immediately.
  const std::wstring first = L"Hello world email user@example.com ";
  ReferenceDoc reference;

  test_case.host->SetHoldGrantMessages(true);
  LocalCheck(SendTextAdaptive(&route, first, nullptr, &error),
             "continuous: the blocked segment was accepted: " + error);
  reference.HostInsert(first);
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  PumpSlice(*test_case.host, 200);

  const wchar_t* words[] = {L"path ", L"C:\\temp\\file_1.txt ", L"URL ",
                            L"https://example.com/a?b=1 ", L"mixedCase ",
                            L"9x_Z-7 ", L"sustained ", L"qwerty ",
                            L"asdfgh ", L"zxcvbn ", L"technical ",
                            L"a.b_c@d/e ", L"tail ", L"token ", L"final "};
  bool words_ok = true;
  for (const wchar_t* word : words) {
    if (!SendTextAdaptive(&route, word, nullptr, &error)) {
      words_ok = false;
      break;
    }
    reference.HostInsert(word);
    // Not a readiness check: just let TSF dispatch its own notifications between words.
    PumpSlice(*test_case.host, 2);
  }
  LocalCheck(words_ok, "continuous: every following word was accepted: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 25000),
             "continuous: the whole run is in the document exactly once, in order");
  const std::wstring after_burst = test_case.doc->text_utf16();
  PumpSlice(*test_case.host, 300);
  LocalCheck(test_case.doc->text_utf16() == after_burst,
             "continuous: no late write changed the document after the run");
  {
    SeamCounters counters = {};
    (void)seam.query(&counters);
    LocalCheck(counters.internal_text_losses == 0,
               "continuous: the product reports zero internal text losses");
  }
  CheckNoSuppressedText(route, "continuous");
  EndCase(&test_case);
  return true;
}

bool RunCapacityCase(const std::filesystem::path& stage,
                     const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  options.async_locks = true;
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "capacity: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  LocalCheck(seam.usable(), "capacity: controlled seam available");
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  std::string error;
  SeamCounters counters = {};
  ReferenceDoc reference;
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "capacity: the engine path is ready");

  // A real deferred document lock: the product's asynchronous requests are not granted
  // while the whole burst is typed. Original text does not depend on those requests.
  test_case.host->SetHoldGrantMessages(true);
  std::wstring burst = L"ab";
  burst.append(1200, L'x');
  CrashPhase("capacity: typing the 1202-unit burst while the document lock is held");
  LocalCheck(SendTextHost(&route, burst, &error),
             "capacity: every burst key stays with the host: " + error);
  reference.HostInsert(burst);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity: the whole burst is in the host document before any grant");
  (void)seam.query(&counters);
  LocalCheck(counters.host_text_keys >= burst.size(),
             "capacity: every burst key is accounted as host-owned");
  LocalCheck(counters.internal_text_losses == 0,
             "capacity: the product reports zero internal text losses");
  CheckNoSuppressedText(route, "capacity");
  // The suggestion stream is allowed to fall behind under this burst; that must not affect
  // a single original character.
  LocalCheck(counters.suggestion_queued + counters.suggestion_dropped >= 1000,
             "capacity: the engine saw (or explicitly dropped) the suggestion backlog");

  test_case.host->SetHoldGrantMessages(false);
  const std::size_t released = test_case.host->ReleaseHeldGrantMessages();
  std::cout << "  [capacity] released held grants=" << released << std::endl;
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity: releasing the lock changed no original character");

  // Boundary sweep around the retired 1024-unit raw bound: one unit below, at, and above it
  // must all be host text, with no product-side capacity account involved.
  for (const std::size_t units : {std::size_t{1023}, std::size_t{1024},
                                  std::size_t{1025}}) {
    std::wstring boundary;
    boundary.append(units, L'b');
    test_case.host->SetHoldGrantMessages(true);
    LocalCheck(SendTextHost(&route, boundary, &error),
               "capacity: the " + std::to_string(units) +
                   "-unit boundary burst stays with the host: " + error);
    reference.HostInsert(boundary);
    CheckDocExact(test_case.doc->text_utf16(), reference.text,
                  "capacity: the " + std::to_string(units) +
                      "-unit boundary burst is exact while the lock is held");
    test_case.host->SetHoldGrantMessages(false);
    (void)test_case.host->ReleaseHeldGrantMessages();
    PumpSlice(*test_case.host, 50);
    CheckDocExact(test_case.doc->text_utf16(), reference.text,
                  "capacity: the " + std::to_string(units) +
                      "-unit boundary burst survived the grant");
  }
  CheckNoSuppressedText(route, "capacity boundary sweep");
  {
    SeamCounters boundary_counters = {};
    (void)seam.query(&boundary_counters);
    LocalCheck(boundary_counters.internal_text_losses == 0,
               "capacity: zero internal text losses across the boundary sweep");
  }

  // Continue in the same activation, with no reactivation or settle wait beyond the host
  // edit itself.
  LocalCheck(SendTextHost(&route, L"after", &error),
             "capacity: the continuation stays with the host: " + error);
  reference.HostInsert(L"after");
  bool space_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "capacity: the continuation Space stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity: the same session kept the complete original text");

  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "capacity: the engine suggestion path is ready again in this session");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "capacity: a fresh token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 15000),
             "capacity: the recovered engine opened a real menu");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 8000),
             "capacity: the recovered menu is stable");
  const std::wstring completion = ReadCandidateText(seam, 1);
  (void)seam.select(counters.candidate_version, 1);
  reference.ReplaceToken(L"hel", completion);
  (void)WaitForText(*test_case.host, test_case.doc, reference.text, 15000);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity: an explicit completion replaced only the new token");

  EndCase(&test_case);
  return true;
}

bool RunCapacityContinueCase(const std::filesystem::path& stage,
                             const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  options.async_locks = true;
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "capacity-continue: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  LocalCheck(seam.usable(), "capacity-continue: controlled seam available");
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  std::string error;
  SeamCounters counters = {};
  ReferenceDoc reference;
  CrashPhase("capacity-continue: waiting for the engine path");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "capacity-continue: the engine path is ready");

  // 1. The original failure shape: a real held lock, a long burst, then immediate
  // continuation in the same activation. The host owns every character, so the lock can
  // delay suggestions but never the text.
  CrashPhase("capacity-continue: sending the 1202-unit burst under a held lock");
  test_case.host->SetHoldGrantMessages(true);
  std::wstring burst = L"ab";
  burst.append(1200, L'x');
  LocalCheck(SendTextHost(&route, burst, &error),
             "capacity-continue: the burst stays with the host: " + error);
  reference.HostInsert(burst);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity-continue: the full burst is present while the lock is held");
  (void)seam.query(&counters);
  LocalCheck(counters.internal_text_losses == 0,
             "capacity-continue: zero internal text losses");
  CheckNoSuppressedText(route, "capacity-continue burst");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();

  CrashPhase("capacity-continue: typing a new token in the same activation");
  std::vector<bool> continued_eaten;
  LocalCheck(SendTextAdaptive(&route, L"after", &continued_eaten, &error),
             "capacity-continue: the continued token was delivered: " + error);
  for (const bool key_eaten : continued_eaten) {
    LocalCheck(!key_eaten, "capacity-continue: a continued key stayed with the host");
  }
  reference.HostInsert(L"after");
  bool space_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "capacity-continue: Space stayed with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity-continue: the continuation follows the burst exactly");
  const std::wstring before_settle = test_case.doc->text_utf16();
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), before_settle,
                "capacity-continue: no late write changed the document");

  // 2. The engine path recovers in the same session; a fresh token produces a real menu and
  // an explicit replacement.
  CrashPhase("capacity-continue: waiting for the engine path to recover");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "capacity-continue: the same session recovered");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "capacity-continue: the fresh token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 15000),
             "capacity-continue: the recovered engine opened a real menu");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 8000),
             "capacity-continue: the recovered menu is stable");
  const std::wstring completion = ReadCandidateText(seam, 1);
  (void)seam.select(counters.candidate_version, 1);
  reference.ReplaceToken(L"hel", completion);
  (void)WaitForText(*test_case.host, test_case.doc, reference.text, 15000);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity-continue: the explicit completion replaced only the new token");

  // 3. A second real backlog in the same session: text and control keys remain exact.
  CrashPhase("capacity-continue: a second held burst with control keys");
  test_case.host->SetHoldGrantMessages(true);
  std::wstring second = L"second";
  second.append(2048, L'y');
  LocalCheck(SendTextHost(&route, second, &error),
             "capacity-continue: the second burst stays with the host: " + error);
  reference.HostInsert(second);
  bool back_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_BACK, L"", &back_eaten, &error) &&
                 !back_eaten,
             "capacity-continue: BackSpace stays with the host: " + error);
  reference.HostBackspace();
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_RETURN, L"", &back_eaten, &error) &&
                 !back_eaten,
             "capacity-continue: Enter stays with the host: " + error);
  reference.HostInsert(L"\n");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "capacity-continue: the second burst and control keys are exact");
  (void)seam.query(&counters);
  LocalCheck(counters.internal_text_losses == 0,
             "capacity-continue: zero internal text losses after two stress bursts");
  CheckNoSuppressedText(route, "capacity-continue total");

  CrashPhase("capacity-continue: deactivating");
  EndCase(&test_case);
  LocalCheck(test_case.product.module == nullptr,
             "capacity-continue: the product module was unloaded after the host teardown");
  return true;
}

bool RunStartupLifecycleCase(const std::filesystem::path& stage,
                             const std::filesystem::path& user_root) {
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  std::string error;
  std::unique_ptr<TestHost> host_a = TestHost::Create(&error);
  if (!host_a) {
    LocalCheck(false, "startup-lifecycle: first host created: " + error);
    return false;
  }
  TestDocument* document_a = host_a->CreateDocument(TestDocumentOptions{}, &error);
  if (document_a == nullptr ||
      !epocanva::tsf::context::test::EnsureTestThreadFocus(&error)) {
    LocalCheck(false, "startup-lifecycle: first document and focus: " + error);
    return false;
  }
  epocanva::tsf::context::test::FocusCallResult focus;
  SetThreadFocus(*host_a, document_a, &focus, &error);

  LoadedProduct product_a;
  std::wstring why;
  if (!PrepareProduct(&product_a, host_a->thread_manager_for_test(), stage, &why)) {
    LocalCheck(false, "startup-lifecycle: first product prepared: " +
                          std::string(why.begin(), why.end()));
    return false;
  }
  ProductSeam seam_a = LoadSeam(product_a);
  LocalCheck(seam_a.startup_usable(), "startup-lifecycle: startup seam available");
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE released = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LocalCheck(entered != nullptr && released != nullptr,
             "startup-lifecycle: events created");
  LocalCheck(seam_a.begin_start(entered, released) == 0,
             "startup-lifecycle: the blocking start was installed");

  CrashPhase("startup-lifecycle: activating the first TIP");
  const ULONGLONG activate_a_start = ::GetTickCount64();
  const HRESULT activated_a =
      product_a.tip->Activate(host_a->thread_manager_for_test(), product_a.client_id);
  const ULONGLONG activate_a_ms = ::GetTickCount64() - activate_a_start;
  LocalCheck(SUCCEEDED(activated_a),
             "startup-lifecycle: the first activation returns immediately");
  LocalCheck(activate_a_ms < 1500, "startup-lifecycle: the first Activate did not wait");
  LocalCheck(::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0,
             "startup-lifecycle: the background start is really in progress");

  CrashPhase("startup-lifecycle: deactivating while the start is blocked");
  const ULONGLONG deactivate_a_start = ::GetTickCount64();
  (void)product_a.tip->Deactivate();
  const ULONGLONG deactivate_a_ms = ::GetTickCount64() - deactivate_a_start;
  LocalCheck(deactivate_a_ms < 1500,
             "startup-lifecycle: Deactivate does not wait for the blocked start");
  product_a.tip->Release();
  product_a.tip = nullptr;
  product_a.sink->Release();
  product_a.sink = nullptr;
  product_a.factory->Release();
  product_a.factory = nullptr;
  const auto can_unload = reinterpret_cast<CanUnloadNowFn>(
      ::GetProcAddress(product_a.module, "DllCanUnloadNow"));
  LocalCheck(can_unload != nullptr && can_unload() == S_FALSE,
             "startup-lifecycle: the blocked start keeps the module from unloading");

  // A second activation while the first start is still blocked. It must stay literal and
  // must not wait on the old start.
  CrashPhase("startup-lifecycle: activating a second TIP while the old start is blocked");
  std::unique_ptr<TestHost> host_b = TestHost::Create(&error);
  if (!host_b) {
    LocalCheck(false, "startup-lifecycle: second host created: " + error);
    ::SetEvent(released);
    return false;
  }
  TestDocument* document_b = host_b->CreateDocument(TestDocumentOptions{}, &error);
  if (document_b == nullptr) {
    LocalCheck(false, "startup-lifecycle: second document: " + error);
    ::SetEvent(released);
    return false;
  }
  SetThreadFocus(*host_b, document_b, &focus, &error);
  LoadedProduct product_b;
  if (!PrepareProduct(&product_b, host_b->thread_manager_for_test(), stage, &why)) {
    LocalCheck(false, "startup-lifecycle: second product prepared: " +
                          std::string(why.begin(), why.end()));
    ::SetEvent(released);
    return false;
  }
  ProductSeam seam_b = LoadSeam(product_b);
  const ULONGLONG activate_b_start = ::GetTickCount64();
  const HRESULT activated_b =
      product_b.tip->Activate(host_b->thread_manager_for_test(), product_b.client_id);
  const ULONGLONG activate_b_ms = ::GetTickCount64() - activate_b_start;
  LocalCheck(SUCCEEDED(activated_b) && activate_b_ms < 1500,
             "startup-lifecycle: the second Activate returns without joining the old start");
  InputRoute route_b{host_b.get(), document_b, product_b.sink, document_b->context()};
  std::string input_error;
  LocalCheck(SendText(&route_b, L"abc", false, &input_error) &&
                 document_b->text_utf16() == L"abc",
             "startup-lifecycle: the second session stays literal: " + input_error);
  SeamCounters counters_b = {};
  (void)seam_b.query(&counters_b);
  LocalCheck(counters_b.engine_ready == 0 && counters_b.literal_key_downs >= 3,
             "startup-lifecycle: no engine pipeline is claimed while the old start runs");

  CrashPhase("startup-lifecycle: releasing the old start");
  ::SetEvent(released);
  const ULONGLONG state_deadline = ::GetTickCount64() + 10000;
  int state = -1;
  for (;;) {
    state = seam_b.state();
    if (state == 3 || ::GetTickCount64() >= state_deadline) {
      break;
    }
    if (host_b != nullptr) {
      PumpSlice(*host_b, 20);
    } else {
      ::Sleep(20);
    }
  }
  LocalCheck(state == 3,
             "startup-lifecycle: the old, late completion is reported as failed");
  LocalCheck(SendText(&route_b, L"def", false, &input_error) &&
                 document_b->text_utf16() == L"abcdef",
             "startup-lifecycle: the second session remains literal after the old completion");
  (void)seam_b.query(&counters_b);
  LocalCheck(counters_b.engine_ready == 0,
             "startup-lifecycle: the stale completion was not adopted");

  // Tear the second session down too. The gate can only open once the background start
  // exited AND both TIPs/factories are released; checking before this would blame the
  // background thread for a live activation.
  CrashPhase("startup-lifecycle: tearing both sessions down");
  (void)product_b.tip->Deactivate();
  product_b.tip->Release();
  product_b.tip = nullptr;
  product_b.sink->Release();
  product_b.sink = nullptr;
  product_b.factory->Release();
  product_b.factory = nullptr;
  host_b.reset();
  host_a.reset();
  const ULONGLONG gate_deadline = ::GetTickCount64() + 10000;
  HRESULT gate = S_FALSE;
  for (;;) {
    gate = can_unload != nullptr ? can_unload() : E_FAIL;
    if (gate == S_OK || ::GetTickCount64() >= gate_deadline) {
      break;
    }
    ::Sleep(20);
  }
  LocalCheck(gate == S_OK,
             "startup-lifecycle: the gate opens after the background start exits");
  HMODULE module_b = product_b.module;
  HMODULE module_a = product_a.module;
  product_a.module = nullptr;
  product_b.module = nullptr;
  if (module_b != nullptr) {
    (void)::FreeLibrary(module_b);
  }
  if (module_a != nullptr) {
    (void)::FreeLibrary(module_a);
  }
  HMODULE still = nullptr;
  const bool image_loaded =
      module_a != nullptr &&
      ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(module_a), &still) != FALSE;
  LocalCheck(!image_loaded,
             "startup-lifecycle: both load references were released and the image is gone");
  ::CloseHandle(released);
  ::CloseHandle(entered);
  return true;
}

// W2-B-R degraded control keys: Enter, Escape and BackSpace through the real controller,
// coordinator and TSF edit sessions while the bounded raw run owns the caret order. Every
// expectation is derived from the documented contract, not from the product's current text.
bool RunDegradedKeysCase(const std::filesystem::path& stage,
                         const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  options.async_locks = true;
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "degraded-keys: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = test_case.seam;
  LocalCheck(seam.usable(), "degraded-keys: controlled seam available");
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  std::string error;
  SeamCounters counters = {};
  ReferenceDoc reference;
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "degraded-keys: the engine path is ready");

  // Phase A: a real backlog plus a held write authorization. Ordinary text and BackSpace
  // are host-owned, so the final document is the exact host reference.
  CrashPhase("degraded-keys: typing a backlog and BackSpace under a held lock");
  test_case.host->SetHoldGrantMessages(true);
  std::wstring burst = L"q";
  burst.append(29, L'w');
  burst += L"abc";
  LocalCheck(SendTextHost(&route, burst, &error),
             "degraded-keys: the backlog stays with the host: " + error);
  reference.HostInsert(burst);
  bool backspace_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_BACK, L"", &backspace_eaten, &error) &&
                 !backspace_eaten,
             "degraded-keys: BackSpace stays with the host and deletes: " + error);
  reference.HostBackspace();
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "degraded-keys: Space stays with the host: " + error);
  reference.HostInsert(L" ");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: BackSpace deleted exactly the last host code point");
  (void)seam.query(&counters);
  LocalCheck(counters.internal_text_losses == 0,
             "degraded-keys: zero internal text losses after the backlog");
  CheckNoSuppressedText(route, "degraded-keys backlog");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "degraded-keys: the engine path is ready after the backlog");

  // Phase B: Enter is host input. Its default newline is the host's, and no original
  // character is lost around it.
  CrashPhase("degraded-keys: Enter under a held lock");
  test_case.host->SetHoldGrantMessages(true);
  LocalCheck(SendTextHost(&route, L"cd", &error),
             "degraded-keys: the Enter prefix stays with the host: " + error);
  reference.HostInsert(L"cd");
  bool enter_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_RETURN, L"", &enter_eaten, &error) &&
                 !enter_eaten,
             "degraded-keys: Enter stays with the host: " + error);
  reference.HostInsert(L"\n");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: Enter kept the host's complete text and newline");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "degraded-keys: the engine path is ready after Enter");

  // Phase C: Escape dismisses the suggestion menu without removing the typed token.
  CrashPhase("degraded-keys: Escape dismisses only suggestions");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "degraded-keys: the Escape token stays with the host: " + error);
  reference.HostInsert(L"hel");
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 15000),
             "degraded-keys: the Escape menu opened");
  bool escape_eaten = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_ESCAPE, L"", &escape_eaten, &error) &&
                 escape_eaten,
             "degraded-keys: Escape is eaten to dismiss the menu");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: Escape kept every typed character");
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: no late write followed the dismiss");
  bool escape_space = false;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &escape_space, &error) &&
                 !escape_space,
             "degraded-keys: the Space after Escape stays with the host: " + error);
  reference.HostInsert(L" ");

  // Phase D: BackSpace at an empty token deletes the host's own preceding character; the
  // product neither eats it nor ignores it.
  CrashPhase("degraded-keys: BackSpace with an empty token suffix");
  bool empty_backspace_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_BACK, L"", &empty_backspace_eaten,
                                    &error) &&
                 !empty_backspace_eaten,
             "degraded-keys: the empty-suffix BackSpace stays with the host: " + error);
  reference.HostBackspace();
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: the host BackSpace deleted its own preceding space");
  LocalCheck(SendTextHost(&route, L"hi", &error),
             "degraded-keys: the following text stays with the host: " + error);
  reference.HostInsert(L"hi");
  bool tail_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &tail_space, &error) &&
                 !tail_space,
             "degraded-keys: the tail Space stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "degraded-keys: the document is exact after all control keys");

  // Phase E: focus change while suggestion work is pending. Original text already belongs
  // to its document; nothing may be replayed into the child, and the child gets only its
  // own input.
  CrashPhase("degraded-keys: focus change with pending suggestions");
  LocalCheck(SendTextHost(&route, L"mn", &error),
             "degraded-keys: the parent token stays with the host: " + error);
  reference.HostInsert(L"mn");
  const std::wstring parent_before_switch = reference.text;
  TestDocumentOptions child_options;
  child_options.initial_text_utf16 = L"";
  TestDocument* child =
      test_case.host->CreateChildContext(test_case.doc, child_options, &error);
  LocalCheck(child != nullptr, "degraded-keys: a child context is pushed: " + error);
  if (child != nullptr) {
    PumpSlice(*test_case.host, 500);
    ITfContext* child_context = child->context();
    LocalCheck(ReadyToType(*test_case.host, sink, child_context, 30000),
               "degraded-keys: the child context became the bound target");
    InputRoute child_route{test_case.host, child, sink, child_context};
    LocalCheck(SendTextHost(&child_route, L"z", &error),
               "degraded-keys: the child token stays with the host: " + error);
    bool child_space = true;
    LocalCheck(SendVirtualKeyAdaptive(&child_route, VK_SPACE, L" ", &child_space,
                                      &error) &&
                   !child_space,
               "degraded-keys: the child Space stays with the host: " + error);
    CheckDocExact(child->text_utf16(), L"z ",
                  "degraded-keys: the child document holds only its own token");
    CheckDocExact(test_case.doc->text_utf16(), parent_before_switch,
                  "degraded-keys: the old document kept its own complete text");
    (void)seam.query(&counters);
    LocalCheck(counters.internal_text_losses == 0,
               "degraded-keys: no internal loss across the focus change");
    CheckNoSuppressedText(route, "degraded-keys parent");
    CheckNoSuppressedText(child_route, "degraded-keys child");
  }

  EndCase(&test_case);
  return true;
}

bool RunUnloadContractCase(const std::filesystem::path& stage,
                           const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"";
  options.async_locks = true;
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "unload-contract: host and activation");
    EndCase(&test_case);
    return false;
  }
  ProductSeam seam = LoadSeam(test_case.product);
  LocalCheck(seam.usable(), "unload-contract: controlled seam available");
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  std::string error;
  SeamCounters counters = {};
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "unload-contract: the engine path is ready");

  const auto can_unload = reinterpret_cast<CanUnloadNowFn>(
      ::GetProcAddress(test_case.product.module, "DllCanUnloadNow"));
  LocalCheck(can_unload != nullptr, "unload-contract: DllCanUnloadNow is exported");

  // Queue a real document edit session and hold its grant. In host-owned mode the only
  // document edit is an explicit candidate selection, so the token and its menu come first.
  CrashPhase("unload-contract: opening a menu for an explicit replacement");
  LocalCheck(SendTextHost(&route, L"hel", &error),
             "unload-contract: the token stays with the host: " + error);
  LocalCheck(WaitForCandidateMenu(*test_case.host, seam, &counters, 2, 15000),
             "unload-contract: the replacement menu opened");
  LocalCheck(WaitForStableMenu(*test_case.host, seam, &counters, 8000),
             "unload-contract: the replacement menu is stable");
  const std::wstring before_selection = test_case.doc->text_utf16();
  CrashPhase("unload-contract: queueing a replacement edit session behind a held grant");
  test_case.host->SetHoldGrantMessages(true);
  (void)seam.select(counters.candidate_version, 1);
  // The session's lock request answers TF_S_ASYNC and its grant message is held, so the
  // coordinator owns a queued ITfEditSession that TSF may still call after Deactivate.
  const ULONGLONG queue_deadline = ::GetTickCount64() + 20000;
  for (;;) {
    (void)seam.query(&counters);
    if (counters.session_pending != 0 || ::GetTickCount64() >= queue_deadline) {
      break;
    }
    PumpSlice(*test_case.host, 20);
  }
  LocalCheck(counters.session_pending != 0,
             "unload-contract: an edit session really is queued inside TSF");
  long long kinds_before[6] = {0, 0, 0, 0, 0, 0};
  if (seam.guarded_objects != nullptr) {
    (void)seam.guarded_objects(kinds_before, 6);
  }
  LocalCheck(kinds_before[0] >= 1,
             "unload-contract: TSF owns a live edit-session callback");
  PrintModuleState(seam, "with a queued session");

  CrashPhase("unload-contract: deactivating while TSF owns the callback");
  ReleaseProduct(&test_case.product, true);
  const HRESULT while_queued = can_unload != nullptr ? can_unload() : E_FAIL;
  LocalCheck(while_queued == S_FALSE,
             "unload-contract: the DLL refuses to unload while TSF owns its callback");
  PrintModuleState(seam, "after deactivate");

  // The application grants the deferred lock. TSF runs the now-stale session and releases
  // it; the observer must publish nothing and the document must not change.
  const std::wstring before_grant = test_case.doc->text_utf16();
  test_case.host->SetHoldGrantMessages(false);
  const std::size_t granted = test_case.host->ReleaseHeldGrantMessages();
  std::cout << "  [unload-contract] granted held locks=" << granted << std::endl;
  PumpSlice(*test_case.host, 1000);
  const ULONGLONG gate_deadline = ::GetTickCount64() + 10000;
  HRESULT settled = while_queued;
  for (;;) {
    settled = can_unload != nullptr ? can_unload() : E_FAIL;
    if (settled == S_OK || ::GetTickCount64() >= gate_deadline) {
      break;
    }
    PumpSlice(*test_case.host, 50);
  }
  LocalCheck(settled == S_OK,
             "unload-contract: the gate opens once TSF released the callback");
  LocalCheck(test_case.doc->text_utf16() == before_grant,
             "unload-contract: the stale session wrote nothing after deactivation");
  PrintModuleState(seam, "after the grant");

  CrashPhase("unload-contract: tearing the host down and freeing the module");
  delete test_case.host;
  test_case.host = nullptr;
  test_case.doc = nullptr;
  const bool freed = FreeProductModule(&test_case.product, 10000, "unload-contract");
  LocalCheck(freed && test_case.product.module == nullptr,
             "unload-contract: the module is freed after the host released its objects");
  return true;
}


bool RunStartupCase(const std::filesystem::path& stage,
                    const std::filesystem::path& user_root) {
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  if (!host) {
    LocalCheck(false, "startup: host created");
    return false;
  }
  TestDocumentOptions options;
  TestDocument* document = host->CreateDocument(options, &error);
  if (document == nullptr) {
    LocalCheck(false, "startup: document created");
    return false;
  }
  epocanva::tsf::context::test::EnsureTestThreadFocus(&error);
  epocanva::tsf::context::test::FocusCallResult focus;
  SetThreadFocus(*host, document, &focus, &error);

  LoadedProduct product;
  std::wstring why;
  if (!PrepareProduct(&product, host->thread_manager_for_test(), stage, &why)) {
    LocalCheck(false, "startup: product prepared");
    return false;
  }
  ProductSeam seam = LoadSeam(product);
  LocalCheck(seam.startup_usable(), "startup: controlled seam available");
  if (!seam.startup_usable()) {
    ReleaseProduct(&product, false);
    return false;
  }
  LocalCheck(seam.reset() == 0, "startup: the bootstrap state was reset");
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE released = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LocalCheck(entered != nullptr && released != nullptr,
             "startup: test events created");
  LocalCheck(seam.begin_start(entered, released) == 0,
             "startup: the blocking start was installed");
  const ULONGLONG activate_start = ::GetTickCount64();
  const HRESULT activated =
      product.tip->Activate(host->thread_manager_for_test(), product.client_id);
  const ULONGLONG activate_elapsed = ::GetTickCount64() - activate_start;
  LocalCheck(SUCCEEDED(activated), "startup: activation succeeded");
  LocalCheck(activate_elapsed < 1500,
             "startup: Activate returned without waiting for the engine");
  LocalCheck(::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0,
             "startup: the background start really is in progress");
  InputRoute route{host.get(), document, product.sink, document->context()};
  SeamCounters counters;
  LocalCheck(SendText(&route, L"abc", false, &error),
             "startup: every key stays literal while the start is blocked: " + error);
  LocalCheck(document->text_utf16() == L"abc",
             "startup: the literal path is exact");
  (void)seam.query(&counters);
  LocalCheck(counters.engine_ready == 0 && counters.literal_key_downs >= 3,
             "startup: the controller reports literal mode, not a working engine");
  LocalCheck(counters.engine_state == 1,
             "startup: the bootstrap state is starting");
  const ULONGLONG deactivate_start = ::GetTickCount64();
  (void)product.tip->Deactivate();
  const ULONGLONG deactivate_elapsed = ::GetTickCount64() - deactivate_start;
  LocalCheck(deactivate_elapsed < 1500,
             "startup: deactivation does not wait for the blocked start");

  using CanUnloadNowFn = HRESULT(WINAPI*)();
  const auto can_unload = reinterpret_cast<CanUnloadNowFn>(
      ::GetProcAddress(product.module, "DllCanUnloadNow"));
  LocalCheck(can_unload != nullptr, "startup: DllCanUnloadNow is exported");
  ReleaseProduct(&product, false);
  LocalCheck(can_unload != nullptr && can_unload() == S_FALSE,
             "startup: the blocked start keeps the module from unloading");
  ::SetEvent(released);
  const ULONGLONG unload_deadline = ::GetTickCount64() + 8000;
  bool unloadable = false;
  int state = -1;
  for (;;) {
    state = seam.state();
    unloadable = can_unload != nullptr && can_unload() == S_OK;
    if (state == 3 && unloadable) {
      break;
    }
    if (::GetTickCount64() >= unload_deadline) {
      break;
    }
    ::Sleep(20);
  }
  LocalCheck(state == 3,
             "startup: the abandoned start ends in a named failure");
  LocalCheck(unloadable,
             "startup: the module becomes unloadable after the start thread exits");
  if (entered != nullptr) {
    ::CloseHandle(entered);
  }
  if (released != nullptr) {
    ::CloseHandle(released);
  }
  if (product.module != nullptr) {
    ::FreeLibrary(product.module);
    product.module = nullptr;
  }
  return true;
}

struct DeployChildOutcome {
  bool launched = false;
  bool exited = false;
  bool requested = false;
  bool completed = false;
  DWORD code = 1;
  std::string fingerprint;
};

DeployChildOutcome RunDeployChild(const std::filesystem::path& exe,
                                  const std::filesystem::path& user_root,
                                  const std::filesystem::path& artifact,
                                  const std::filesystem::path& result) {
  DeployChildOutcome outcome;
  std::wstring command = L"\"" + exe.wstring() + L"\" --child";
  command += L" --user-root \"" + user_root.wstring() + L"\"";
  command += L" --artifact \"" + artifact.wstring() + L"\"";
  command += L" --result \"" + result.wstring() + L"\"";
  command += L" --hold-ms 0";
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  outcome.launched = ::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr,
                                      FALSE, 0, nullptr, nullptr, &startup,
                                      &process) != FALSE;
  if (!outcome.launched) {
    return outcome;
  }
  outcome.exited = ::WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0;
  if (outcome.exited) {
    ::GetExitCodeProcess(process.hProcess, &outcome.code);
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  const std::string text = ReadNarrow(result);
  outcome.requested = text.find("requested=1") != std::string::npos;
  outcome.completed = text.find("completed=1") != std::string::npos;
  const std::size_t marker = text.find("fingerprint=");
  if (marker != std::string::npos) {
    const std::size_t end = text.find('\n', marker);
    outcome.fingerprint =
        text.substr(marker + 12, end == std::string::npos ? std::string::npos
                                                          : end - marker - 12);
  }
  return outcome;
}

// The product's runtime lease must hold the shared engine.lock across processes. While a
// live engine exists, a changed settings bundle from another host defers and keeps both the
// marker and the compiled output; a new host without a change still activates. After the
// product deactivates, the deferred deployment applies.
bool RunLeaseLockCase(const std::filesystem::path& stage,
                      const std::filesystem::path& user_root,
                      const std::filesystem::path& deployment_exe,
                      const std::filesystem::path& run_root) {
  Case test_case;
  TestDocumentOptions options;
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "lease: host and activation");
    EndCase(&test_case);
    return false;
  }
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "lease: the product engine is running");
  PumpSlice(*test_case.host, 300);

  using LeaseCountFn = unsigned long long (*)();
  const auto lease_count = reinterpret_cast<LeaseCountFn>(
      ::GetProcAddress(test_case.product.module, "EpoCanvaTestEngineHostLeaseCount"));
  auto wait_leases = [lease_count](unsigned long long expected) {
    const ULONGLONG deadline = ::GetTickCount64() + 10000;
    while (lease_count != nullptr && lease_count() != expected &&
           ::GetTickCount64() < deadline) {
      ::Sleep(10);
    }
    return lease_count != nullptr && lease_count() == expected;
  };
  HANDLE ready_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LocalCheck(ready_event != nullptr && release_event != nullptr,
             "lease: second-attachment ordering events exist");
  if (ready_event == nullptr || release_event == nullptr) {
    if (ready_event != nullptr) ::CloseHandle(ready_event);
    if (release_event != nullptr) ::CloseHandle(release_event);
    EndCase(&test_case);
    return false;
  }
  HoldSessionResult second;
  std::thread waiter(HoldSessionWorker, stage, ready_event, release_event, &second);
  const bool ready = ::WaitForSingleObject(ready_event, 25000) == WAIT_OBJECT_0;
  LocalCheck(ready && second.ok.load() && wait_leases(2),
             "lease: two real attachments share the running engine");

  const std::filesystem::path marker = user_root / L"engine" / L"deploy-required";
  const std::filesystem::path compiled =
      user_root / L"engine" / L"build" / L"epocanva_english.schema.yaml";
  const std::filesystem::path state = user_root / L"engine" / L"deployed-state";
  const std::filesystem::path result = run_root / L"lease-child.txt";
  std::error_code ec;
  std::filesystem::remove(result, ec);
  LocalCheck(std::filesystem::is_regular_file(compiled),
             "lease: the running engine has a compiled schema");

  epocanva::product::ProductSettings changed;
  changed.completion_enabled = true;
  changed.completion_candidates = 6;
  changed.spelling_enabled = true;
  changed.spelling_candidates = 3;
  std::wstring why;
  LocalCheck(WriteSettingsOnly(user_root, changed, &why),
             "lease: a changed settings bundle is written for another host");
  LocalCheck(std::filesystem::is_regular_file(marker),
             "lease: the settings marker exists while the engine runs");

  const ULONGLONG start = ::GetTickCount64();
  const DeployChildOutcome deferred =
      RunDeployChild(deployment_exe, user_root, compiled, result);
  const ULONGLONG elapsed = ::GetTickCount64() - start;
  LocalCheck(deferred.launched && deferred.exited && deferred.code == 0 &&
                 deferred.completed,
             "lease: the second host activation completes");
  LocalCheck(!deferred.requested,
             "lease: the second host defers instead of deploying under the live engine");
  LocalCheck(elapsed < 15000,
             "lease: the shared runtime lock does not block the new host");
  LocalCheck(std::filesystem::is_regular_file(compiled),
             "lease: the compiled output in use was not deleted");
  LocalCheck(std::filesystem::is_regular_file(marker),
             "lease: the deferred marker survives for the next real start");
  const std::string during = ReadNarrow(state);
  LocalCheck(during.find(deferred.fingerprint) == std::string::npos,
             "lease: no new fingerprint was recorded during the deferral");

  // The first attachment originally owned the only file handle. Retire it first,
  // and observe its asynchronous lease release before asking another process to deploy.
  ReleaseProduct(&test_case.product, true);
  delete test_case.host;
  test_case.host = nullptr;
  test_case.doc = nullptr;
  LocalCheck(wait_leases(1), "lease: first attachment released while second stays live");
  std::filesystem::remove(result, ec);
  const DeployChildOutcome one_remaining =
      RunDeployChild(deployment_exe, user_root, compiled, result);
  LocalCheck(one_remaining.launched && one_remaining.exited && one_remaining.code == 0 &&
                 one_remaining.completed && !one_remaining.requested,
             "lease: second attachment alone still prevents cross-process deployment");
  LocalCheck(std::filesystem::is_regular_file(compiled) &&
                 std::filesystem::is_regular_file(marker) && ReadNarrow(state) == during,
             "lease: surviving attachment keeps compiled output and deferred marker intact");

  ::SetEvent(release_event);
  waiter.join();
  ::CloseHandle(release_event);
  ::CloseHandle(ready_event);
  LocalCheck(wait_leases(0), "lease: final attachment released its engine lease");
  DeferProductModule(&test_case.product);
  LocalCheck(FreeDeferredProductModules(10000, "lease-final"),
             "lease: the engine stopped before both module references were released");

  std::filesystem::remove(result, ec);
  const DeployChildOutcome applied =
      RunDeployChild(deployment_exe, user_root, compiled, result);
  LocalCheck(applied.launched && applied.exited && applied.code == 0 &&
                 applied.requested && applied.completed,
             "lease: the next activation after deactivation deploys");
  LocalCheck(!std::filesystem::is_regular_file(marker),
             "lease: the marker is retired after the applied deployment");
  const std::string after = ReadNarrow(state);
  LocalCheck(after.find(applied.fingerprint) != std::string::npos,
             "lease: the new fingerprint is recorded after the deployment");
  return true;
}

// Independent reference edit model. It is written from the documented default English
// behaviour (raw characters preserved, Space commits and inserts one space) and never calls
// product code. The final controlled-store text must equal its text.

bool RunColdTokenCase(const std::filesystem::path& stage,
                      const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  // The field is first classified as Unknown (the metadata cannot be read), which is the
  // cold-start shape: the first character must be the host's. After that first insertion the
  // test flips the metadata to a readable Allowed field.
  options.scope_answer = epocanva::tsf::edit::test::TestScopeAnswer::kWrongType;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "token: host and activation");
    EndCase(&test_case);
    return false;
  }
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  ReferenceDoc reference;
  std::string error;
  bool first_eaten = true;
  LocalCheck(SendCharAdaptive(&route, L'a', &first_eaten, &error),
             "token: the first character was typed: " + error);
  LocalCheck(!first_eaten,
             "token: the unknown-permission first character stayed with the host");
  reference.HostInsert(L"a");
  // The field becomes readable. The host insertion above already updated the caret; the
  // rebind now resolves Allowed and the suggestion path is available.
  test_case.doc->SetScopeAnswer(epocanva::tsf::edit::test::TestScopeAnswer::kScopes,
                               {IS_DEFAULT});
  PumpSlice(*test_case.host, 1000);
  std::vector<bool> eaten;
  LocalCheck(SendTextAdaptive(&route, L"bc", &eaten, &error),
             "token: the rest of the token was typed: " + error);
  LocalCheck(eaten.size() == 2 && !eaten[0] && !eaten[1],
             "token: no character in the middle of the token was eaten");
  reference.HostInsert(L"bc");
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error),
             "token: the boundary space was typed: " + error);
  LocalCheck(!space_eaten, "token: the boundary space also stayed with the host");
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "token: the whole token is contiguous in the document");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "token: the engine is ready for the next token");
  LocalCheck(SendTextHost(&route, L"de", &error),
             "token: a fresh token stays with the host: " + error);
  reference.HostInsert(L"de");
  bool fresh_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &fresh_space, &error) &&
                 !fresh_space,
             "token: the fresh boundary stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "token: the reference document matches (host token + host token)");
  EndCase(&test_case);
  return true;
}

bool RunPressureCase(const std::filesystem::path& stage,
                     const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  options.async_locks = true;
  options.initial_text_utf16 = L"";
  if (!BeginCase(&test_case, stage, user_root, options, nullptr)) {
    LocalCheck(false, "pressure: host and activation");
    EndCase(&test_case);
    return false;
  }
  ITfKeyEventSink* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  ProductSeam seam = test_case.seam;
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "pressure: the engine path is ready");
  ReferenceDoc reference;
  std::string error;
  SeamCounters counters = {};

  // Phase A -- a real backlog with the document lock held. The host inserts every character
  // synchronously; the engine suggestion stream is bounded and may drop suggestions. The
  // document must still equal the full reference, once.
  std::wstring burst;
  for (int repeat = 0; repeat < 20; ++repeat) {
    burst += L"Aa1@x.y_Z-9";
  }
  test_case.host->SetHoldGrantMessages(true);
  LocalCheck(SendTextHost(&route, burst, &error),
             "pressure: every burst key stayed with the host: " + error);
  reference.HostInsert(burst);
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error) &&
                 !space_eaten,
             "pressure: the closing space stayed with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "pressure: the exact burst text is present while the lock is held");
  (void)seam.query(&counters);
  LocalCheck(counters.internal_text_losses == 0,
             "pressure: zero internal text losses");
  CheckNoSuppressedText(route, "pressure burst");
  LocalCheck(counters.suggestion_queued + counters.suggestion_dropped >= 100,
             "pressure: the suggestion stream really accumulated and/or dropped work");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  const std::wstring after_burst = test_case.doc->text_utf16();
  PumpSlice(*test_case.host, 300);
  CheckDocExact(test_case.doc->text_utf16(), after_burst,
                "pressure: no late duplicate edit changed the document");

  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "pressure: the engine is ready after the backlog");
  LocalCheck(SendTextHost(&route, L"tail", &error),
             "pressure: the next token stays with the host: " + error);
  reference.HostInsert(L"tail");
  bool tail_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &tail_space, &error) &&
                 !tail_space,
             "pressure: the next boundary stays with the host: " + error);
  reference.HostInsert(L" ");
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "pressure: the next token follows the burst in order");

  // Phase B -- sustained typing with a stalled write authorization, group by group. The
  // document must remain exact after every group.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 30000),
             "pressure: ready for the sustained phase");
  std::wstring sustained;
  for (int repeat = 0; repeat < 10; ++repeat) {
    sustained += L"Qq2#r_s-T7";
  }
  test_case.host->SetHoldGrantMessages(true);
  for (std::size_t offset = 0; offset < sustained.size(); offset += 5) {
    const std::wstring group = sustained.substr(offset, 5);
    LocalCheck(SendTextHost(&route, group, &error),
               "pressure: sustained group stayed with the host: " + error);
    reference.HostInsert(group);
    PumpSlice(*test_case.host, 10);
    CheckDocExact(test_case.doc->text_utf16(), reference.text,
                  "pressure: the sustained text is exact at this group");
  }
  bool sustained_space = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &sustained_space,
                                    &error) &&
                 !sustained_space,
             "pressure: the sustained closing space stayed with the host: " + error);
  reference.HostInsert(L" ");
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  CheckDocExact(test_case.doc->text_utf16(), reference.text,
                "pressure: the sustained text is complete and in order");
  (void)seam.query(&counters);
  LocalCheck(counters.internal_text_losses == 0,
             "pressure: zero internal losses across sustained typing");
  CheckNoSuppressedText(route, "pressure total");
  EndCase(&test_case);

  // Deactivation while suggestion work is queued and the document lock is held must return
  // without waiting for the engine. The process surviving its own teardown is the assertion.
  Case stopping;
  TestDocumentOptions stop_options;
  stop_options.async_locks = true;
  if (BeginCase(&stopping, stage, user_root, stop_options, nullptr)) {
    InputRoute stop_route{stopping.host, stopping.doc, stopping.product.sink,
                          stopping.doc->context()};
    if (ReadyToType(*stopping.host, stopping.product.sink, stopping.doc->context(),
                    15000)) {
      stopping.host->SetHoldGrantMessages(true);
      std::string stop_error;
      (void)SendTextHost(&stop_route, L"pending-degraded-run", &stop_error);
      LocalCheck(true, "pressure: suggestion work is pending at deactivation");
    }
    EndCase(&stopping);
    LocalCheck(true, "pressure: deactivation with pending work returned");
  } else {
    LocalCheck(false, "pressure: the stop-race case activated");
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path product_dll;
  std::filesystem::path rime_dll;
  std::filesystem::path data_dir;
  std::filesystem::path lexicon;
  std::filesystem::path run_root;
  std::filesystem::path deployment_exe;
  std::string mode = "input";
  int watchdog_ms = 0;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--product-dll" && index + 1 < argc) {
      product_dll = std::filesystem::path(argv[++index]);
    } else if (arg == "--rime-dll" && index + 1 < argc) {
      rime_dll = std::filesystem::path(argv[++index]);
    } else if (arg == "--data-dir" && index + 1 < argc) {
      data_dir = std::filesystem::path(argv[++index]);
    } else if (arg == "--lexicon" && index + 1 < argc) {
      lexicon = std::filesystem::path(argv[++index]);
    } else if (arg == "--run-root" && index + 1 < argc) {
      run_root = std::filesystem::path(argv[++index]);
    } else if (arg == "--mode" && index + 1 < argc) {
      mode = argv[++index];
    } else if (arg == "--deployment-exe" && index + 1 < argc) {
      deployment_exe = std::filesystem::path(argv[++index]);
    } else if (arg == "--watchdog-ms" && index + 1 < argc) {
      watchdog_ms = std::atoi(argv[++index]);
    }
  }
  if (product_dll.empty() || rime_dll.empty() || data_dir.empty() ||
      lexicon.empty() || run_root.empty() || !run_root.is_absolute()) {
    std::cout
        << "usage error: --product-dll --rime-dll --data-dir --lexicon "
           "--run-root <absolute> [--mode input|engine-missing|engine-mismatch|candidate|"
           "replacement-faults|suggestion-resync|continuous|capacity|capacity-continue|"
           "unload-contract|degraded-keys|startup|startup-lifecycle] "
           "[--watchdog-ms N]\n";
    return 2;
  }

  setvbuf(stdout, nullptr, _IONBF, 0);
  SaveKeyStateOnce();
  std::error_code ec;
  std::filesystem::create_directories(run_root, ec);
  const std::filesystem::path stage = run_root / L"stage";
  {
    wchar_t exe_path[32768] = {};
    ::GetModuleFileNameW(nullptr, exe_path, 32768);
    const std::wstring symbol_path =
        std::filesystem::path(exe_path).parent_path().wstring() + L";" +
        stage.wstring() + L";" + product_dll.parent_path().wstring();
    epocanva::tsf::product::test::InstallCrashCapture(run_root / L"diagnostics",
                                                      symbol_path);
    epocanva::tsf::product::test::SetCrashCounterCallback(&CrashCountersForReport);
  }
  if (watchdog_ms > 0) {
    std::thread([watchdog_ms]() {
      ::Sleep(static_cast<DWORD>(watchdog_ms));
      (void)epocanva::tsf::product::test::WriteHangDump(
          "watchdog", "the test exceeded --watchdog-ms; the dump describes the live hang");
      ::TerminateProcess(::GetCurrentProcess(), 3);
    }).detach();
  }
  const std::filesystem::path user_root = run_root / L"鐢ㄦ埛 鏁版嵁";
  const std::filesystem::path marker = user_root / L"engine" / L"deploy-required";
  const std::filesystem::path state = user_root / L"engine" / L"deployed-state";
  const std::filesystem::path compiled =
      user_root / L"engine" / L"build" / L"epocanva_english.schema.yaml";

  epocanva::product::ProductSettings settings;
  settings.completion_enabled = true;
  settings.completion_candidates = 4;
  settings.spelling_enabled = true;
  settings.spelling_candidates = 2;

  if (mode == "lease") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "lease: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "lease: settings and marker written");
    LocalCheck(std::filesystem::is_regular_file(deployment_exe),
               "lease: the deployment test executable is available");
    RunLeaseLockCase(stage, user_root, deployment_exe, run_root);
  } else if (mode == "token") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "token: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "token: settings and marker written");
    RunColdTokenCase(stage, user_root);
  } else if (mode == "pressure") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "pressure: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "pressure: settings and marker written");
    RunPressureCase(stage, user_root);
  } else if (mode == "focus") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                           &why),
               "focus: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "focus: settings and marker written");
    Paths focus_paths;
    focus_paths.stage = stage;
    focus_paths.user_root = user_root;
    RunFocusSwitchCase(focus_paths);
  } else if (mode == "timing") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                           &why),
               "timing: product staged");
    RunSettingsTimingCase(stage, user_root, marker, compiled, &why);
  } else if (mode == "multi") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                           &why),
               "multi: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "multi: settings and marker written");
    RunMultiSessionCase(stage, user_root, settings, &why);
  } else if (mode == "strict-unregistered") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                           &why),
               "strict: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "strict: settings and marker written");
    RunStrictUnregisteredCase(stage, user_root);
  } else if (mode == "candidate") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "candidate: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "candidate: settings and marker written");
    RunCandidateCase(stage, user_root);
  } else if (mode == "suggestion-resync") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "resync: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "resync: settings and marker written");
    RunSuggestionResyncCase(stage, user_root);
  } else if (mode == "replacement-faults") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "faults: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "faults: settings and marker written");
    RunReplacementFaultCase(stage, user_root);
  } else if (mode == "continuous") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "continuous: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "continuous: settings and marker written");
    RunContinuousCase(stage, user_root);
  } else if (mode == "capacity") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "capacity: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "capacity: settings and marker written");
    RunCapacityCase(stage, user_root);
  } else if (mode == "capacity-continue") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "capacity-continue: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "capacity-continue: settings and marker written");
    RunCapacityContinueCase(stage, user_root);
  } else if (mode == "unload-contract") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "unload-contract: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "unload-contract: settings and marker written");
    RunUnloadContractCase(stage, user_root);
  } else if (mode == "degraded-keys") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "degraded-keys: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "degraded-keys: settings and marker written");
    RunDegradedKeysCase(stage, user_root);
  } else if (mode == "startup") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "startup: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "startup: settings and marker written");
    RunStartupCase(stage, user_root);
  } else if (mode == "startup-lifecycle") {
    std::wstring why;
    LocalCheck(StageProduct(stage, product_dll, rime_dll, data_dir, lexicon, true,
                            &why),
               "startup-lifecycle: product staged");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "startup-lifecycle: settings and marker written");
    RunStartupLifecycleCase(stage, user_root);
  } else if (mode == "engine-missing" || mode == "engine-mismatch") {
    std::wstring why;
    const bool mismatch = mode == "engine-mismatch";
    const bool staged = StageProduct(stage, product_dll, rime_dll, data_dir, lexicon,
                                     mismatch, &why);
    LocalCheck(staged, "unusable engine: product staged");
    if (mismatch) {
      std::ofstream changed(stage / L"rime.dll", std::ios::binary | std::ios::app);
      changed.put('\0');
      changed.close();
      LocalCheck(changed.good(), "engine mismatch: appended one byte to the copied DLL");
    }
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "unusable engine: settings and marker written");
    RunEngineMissingCase(stage, user_root, marker, compiled);
  } else {
    std::wstring why;
    const bool staged = StageProduct(stage, product_dll, rime_dll, data_dir, lexicon,
                                     true, &why);
    LocalCheck(staged, "product staged for the real-engine cases");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "settings and marker written");
    Paths paths;
    paths.stage = stage;
    paths.user_root = user_root;
    paths.marker = marker;
    paths.state = state;
    paths.compiled = compiled;
    RunRawInputCases(paths);
    std::error_code cleanup;
    std::filesystem::remove_all(user_root / L"engine" / L"build", cleanup);
    std::filesystem::remove(state, cleanup);
    PrepareUserRoot(user_root, settings, &why);
    TestDocumentOptions read_only;
    read_only.read_only = true;
    RunPermissionCase(paths, read_only, "read-only");
    TestDocumentOptions password;
    password.scope_answer = epocanva::tsf::edit::test::TestScopeAnswer::kScopes;
    password.input_scopes = {IS_PASSWORD};
    RunPermissionCase(paths, password, "password scope");
    TestDocumentOptions unknown;
    unknown.scope_answer = epocanva::tsf::edit::test::TestScopeAnswer::kWrongType;
    RunPermissionCase(paths, unknown, "unknown permission");
    std::filesystem::remove_all(user_root / L"engine" / L"build", cleanup);
    std::filesystem::remove(state, cleanup);
    PrepareUserRoot(user_root, settings, &why);
    RunExternalEditCase(paths);
  }

  RestoreKeyState();
  if (g_pump_event != nullptr) {
    ::CloseHandle(g_pump_event);
    g_pump_event = nullptr;
  }
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
