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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "identifiers.h"
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

bool ActivateProduct(LoadedProduct* product, ITfThreadMgr* thread_manager,
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

void ReleaseProduct(LoadedProduct* product, bool deactivate) {
  if (product->tip != nullptr) {
    if (deactivate) {
      (void)product->tip->Deactivate();
    }
    product->tip->Release();
    product->tip = nullptr;
  }
  if (product->sink != nullptr) {
    product->sink->Release();
    product->sink = nullptr;
  }
  if (product->factory != nullptr) {
    product->factory->Release();
    product->factory = nullptr;
  }
  if (product->module != nullptr) {
    ::FreeLibrary(product->module);
    product->module = nullptr;
  }
}

HANDLE g_pump_event = nullptr;

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
};

bool ReadyToType(TestHost& host, ITfKeyEventSink* sink, ITfContext* context,
                 DWORD timeout_ms);

bool ApplyHostDefault(InputRoute* route, const std::wstring& text, std::string* error) {
  if (text.empty()) {
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
  const LONG caret = route->doc->selection_start_utf16();
  if (!RunExternalEdit(*route->host, route->doc, client, ExternalEditMode::kInsertText,
                       caret, caret, text, &edit, error)) {
    *error = "the host default insert did not run: " + *error;
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
  if (!*eaten_out && !ApplyHostDefault(route, host_text, error)) {
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
                 DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  const UINT vk = static_cast<UINT>(LOBYTE(::VkKeyScanW(L'a')));
  for (;;) {
    std::array<std::uint8_t, 256> state = {};
    ::SetKeyboardState(state.data());
    BOOL eaten = FALSE;
    if (SUCCEEDED(sink->OnTestKeyDown(context, vk, KeyLparam(vk, true, false),
                                      &eaten)) &&
        eaten != FALSE) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    PumpSlice(host, 20);
  }
}

struct Case {
  TestHost* host = nullptr;
  TestDocument* doc = nullptr;
  LoadedProduct product;
  std::string error;
};

bool BeginCase(Case* test_case, const std::filesystem::path& stage,
               const std::filesystem::path& user_root,
               const TestDocumentOptions& options, std::wstring* why) {
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
  if (!ActivateProduct(&test_case->product, test_case->host->thread_manager_for_test(),
                       stage, &activate_why)) {
    std::cout << "  activate: "
              << std::string(activate_why.begin(), activate_why.end()) << "\n";
    if (why != nullptr) {
      *why = activate_why;
    }
    return false;
  }
  return true;
}

void EndCase(Case* test_case) {
  if (test_case->host != nullptr) {
    ReleaseProduct(&test_case->product, true);
    delete test_case->host;
    test_case->host = nullptr;
    test_case->doc = nullptr;
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
    LocalCheck(false, "case host and activation");
    EndCase(&test_case);
    return false;
  }
  auto* sink = test_case.product.sink;
  ITfContext* context = test_case.doc->context();
  InputRoute route{test_case.host, test_case.doc, sink, context};
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "the product controller bound and resolved permission");
  LocalCheck(::GetFileAttributesW(paths.marker.c_str()) == INVALID_FILE_ATTRIBUTES,
             "the deployment marker was retired after the successful start");
  LocalCheck(std::filesystem::is_regular_file(paths.state),
             "the deployment snapshot was recorded");
  LocalCheck(std::filesystem::is_regular_file(paths.compiled),
             "the compiled schema exists in the user engine directory");
  const std::string compiled_text = ReadNarrow(paths.compiled);
  LocalCheck(compiled_text.find("max_completions: 4") != std::string::npos,
             "the compiled schema carries the configured completion count");
  LocalCheck(compiled_text.find("max_suggestions: 2") != std::string::npos,
             "the compiled schema carries the configured suggestion count (2, not 1)");

  std::string error;
  LocalCheck(SendTextWhenReady(&route, L"Hello", true, &error), "type Hello: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space while composing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello ", 5000),
             "Hello + Space commits the preserved case");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"helo", true, &error), "type helo: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after helo is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello helo ", 5000),
             "default raw input does not auto-correct helo");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"helo", true, &error), "type helo again: " + error);
  LocalCheck(SendVirtualKey(&route, VK_BACK, true, &error),
             "BackSpace while composing is eaten: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after BackSpace is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello helo hel ", 5000),
             "BackSpace edits the composition");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"123-45_67@x.y", true, &error),
             "digits and technical marks are forwarded: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after marks is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y ", 5000),
             "digits and marks are committed unchanged");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"user@example.com", true, &error),
             "email is accepted: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after email is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com ", 5000),
             "email is committed unchanged");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"https://example.com/a?x=1&y=2#z", true, &error),
             "URL is accepted: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after URL is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z ",
                         5000),
             "URL is committed unchanged");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"C:\\tmp\\file.txt", true, &error),
             "path is accepted: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after path is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt ",
                         5000),
             "path is committed unchanged");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"foo_bar123", true, &error),
             "identifier is accepted: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after identifier is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt "
                         L"foo_bar123 ",
                         5000),
             "identifier is committed unchanged");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"enter", true, &error),
             "type enter: " + error);
  LocalCheck(SendVirtualKey(&route, VK_RETURN, true, &error),
             "Enter while composing is eaten: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after Enter is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt "
                         L"foo_bar123 enter ",
                         5000),
             "Enter does not lose the composition and Space commits it");

  error.clear();
  LocalCheck(SendShortcut(&route, 'C', true, false, false, &error),
             "Ctrl+C passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'V', true, false, false, &error),
             "Ctrl+V passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'Z', true, false, false, &error),
             "Ctrl+Z passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'X', false, true, false, &error),
             "Alt+X passes to the host: " + error);
  LocalCheck(SendShortcut(&route, 'R', false, false, true, &error),
             "Win+R passes to the host: " + error);
  LocalCheck(test_case.doc->text_utf16() ==
                 L"Hello helo hel 123-45_67@x.y user@example.com "
                 L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt "
                 L"foo_bar123 enter ",
             "shortcut pass-through changed no text");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"quickbrownfox", true, &error),
             "fast typing is accepted back to back: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "space after fast typing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt "
                         L"foo_bar123 enter quickbrownfox ",
                         8000),
             "fast typing keeps order and commits once");

  error.clear();
  LocalCheck(SendTextWhenReady(&route, L"zzz", true, &error), "type zzz: " + error);
  LocalCheck(SendVirtualKey(&route, VK_ESCAPE, true, &error),
             "Escape while composing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com "
                         L"https://example.com/a?x=1&y=2#z C:\\tmp\\file.txt "
                         L"foo_bar123 enter quickbrownfox ",
                         5000),
             "Escape cancels the composition without changing committed text");

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
      (void)ApplyHostDefault(&route, L"a", &error);
    } else {
      LocalCheck(ApplyHostDefault(&route, L"a", &error),
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
  LocalCheck(SendText(&route, L"abc", true, &error), "external edit: type abc: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"baseabc", 5000),
             "external edit: composition is visible");
  const TfClientId external =
      epocanva::tsf::context::test::ExternalClientId(
          test_case.host->thread_manager_for_test(), &error);
  ExternalEditResult edit;
  LocalCheck(RunExternalEdit(*test_case.host, test_case.doc, external,
                             ExternalEditMode::kInsertText, 0, 0, L"X", &edit, &error),
             "external edit: another client writes: " + error);
  LocalCheck(ReadyToType(*test_case.host, sink, context, 10000),
             "external edit: the new binding is ready");
  const std::wstring after_external = test_case.doc->text_utf16();
  error.clear();
  LocalCheck(SendText(&route, L"def", true, &error),
             "external edit: typing after the external write is accepted: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "external edit: space is eaten: " + error);
  const std::wstring expected = after_external + L"def ";
  LocalCheck(WaitForText(*test_case.host, test_case.doc, expected, 8000),
             "external edit: the new binding commits after rebinding");
  EndCase(&test_case);
  return true;
}


struct SessionResult {
  bool ok = false;
  std::wstring text;
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
  activated->fetch_add(1);
  const ULONGLONG deadline = ::GetTickCount64() + 15000;
  while (activated->load() < 2 && ::GetTickCount64() < deadline) {
    ::Sleep(5);
  }
  ITfContext* context = document->context();
  InputRoute route{host, document, product.sink, context};
  if (!ReadyToType(*host, product.sink, context, 15000)) {
    result->error = "the session never became ready";
  } else if (!SendText(&route, word, true, &result->error) ||
             !SendVirtualKey(&route, VK_SPACE, true, &result->error) ||
             !WaitForText(*host, document, word + L" ", 8000)) {
    result->error = "the session did not commit its word";
  } else {
    result->ok = true;
    result->text = document->text_utf16();
  }
  ReleaseProduct(&product, true);
  ::DestroyWindow(window);
  delete host;
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
  LocalCheck(first.ok && first.text == L"alpha ",
             "session A committed its own text");
  LocalCheck(second.ok && second.text == L"beta ",
             "session B committed its own text");
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
  return true;
}

struct HoldSessionResult {
  std::atomic<bool> ready{false};
  std::atomic<bool> ok{false};
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
  result->ready.store(true);
  result->ok.store(ReadyToType(*host, product.sink, document->context(), 15000));
  ::SetEvent(ready_event);
  (void)::WaitForSingleObject(release_event, 30000);
  ReleaseProduct(&product, true);
  ::DestroyWindow(window);
  delete host;
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
  LocalCheck(SendText(&parent_route, L"one", true, &error) &&
                 SendVirtualKey(&parent_route, VK_SPACE, true, &error) &&
                 WaitForText(*test_case.host, test_case.doc, L"parentone ", 5000),
             "focus: the parent context accepts text");

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
    LocalCheck(SendText(&child_route, L"child", true, &error) &&
                   SendVirtualKey(&child_route, VK_SPACE, true, &error) &&
                   WaitForText(*test_case.host, child, L"child ", 8000),
               "focus: the child context accepts text");
    LocalCheck(test_case.doc->text_utf16() == L"parentone ",
               "focus: the parent document was not written after the switch");
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

  LocalCheck(WriteSettingsOnly(user_root, second, why),
             "timing: new settings are saved while the engine runs");
  HANDLE ready_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HoldSessionResult deferred;
  std::thread waiter(HoldSessionWorker, stage, ready_event, release_event, &deferred);
  const bool ready_signalled = ::WaitForSingleObject(ready_event, 25000) == WAIT_OBJECT_0;
  LocalCheck(ready_signalled && deferred.ok.load(),
             "timing: the second session joins the running engine");
  LocalCheck(std::filesystem::is_regular_file(marker),
             "timing: the new settings marker survives while the engine runs");
  const std::string during = ReadNarrow(compiled);
  LocalCheck(during.find("max_completions: 4") != std::string::npos,
             "timing: the running engine is not redeployed underneath itself");
  ::SetEvent(release_event);
  waiter.join();
  ::CloseHandle(release_event);
  ::CloseHandle(ready_event);
  EndCase(&held);

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
  LocalCheck(SendText(&renewed_route, L"timing", true, &error) &&
                 SendVirtualKey(&renewed_route, VK_SPACE, true, &error) &&
                 WaitForText(*renewed.host, renewed.doc, L"timing ", 8000),
             "timing: the restarted engine accepts input");
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
  const std::wstring missing_reason;
  const bool activated =
      ActivateProduct(&product, host->thread_manager_for_test(), stage, nullptr);
  LocalCheck(!activated, "missing rime.dll: activation fails");
  ReleaseProduct(&product, false);
  LocalCheck(std::filesystem::is_regular_file(marker),
             "missing rime.dll: the marker is kept for a retry");
  LocalCheck(!std::filesystem::is_regular_file(compiled),
             "missing rime.dll: no compiled schema is claimed");
  (void)missing_reason;
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
  EndCase(&test_case);

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
struct ReferenceDoc {
  std::wstring text;
  void HostInsert(const std::wstring& units) { text += units; }
  void RawAppend(const std::wstring& units) { text += units; }
};

bool RunColdTokenCase(const std::filesystem::path& stage,
                      const std::filesystem::path& user_root) {
  Case test_case;
  TestDocumentOptions options;
  // The field is first classified as Unknown (the metadata cannot be read), which is the
  // cold-start shape: the first character must be the host's. After that first insertion the
  // test flips the metadata to a readable Allowed field, so the old binding is replaced and
  // a fresh binding would happily take the next character -- unless the host-owned token is
  // honoured.
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
  LocalCheck(!first_eaten, "token: the unknown-permission first character stayed with the host");
  reference.HostInsert(L"a");
  // The field becomes readable. The host insertion above already invalidated the old
  // binding; the rebind now resolves Allowed and the engine path is available.
  test_case.doc->SetScopeAnswer(epocanva::tsf::edit::test::TestScopeAnswer::kNone, {});
  PumpSlice(*test_case.host, 1000);
  std::vector<bool> eaten;
  LocalCheck(SendTextAdaptive(&route, L"bc", &eaten, &error),
             "token: the rest of the token was typed: " + error);
  LocalCheck(eaten.size() == 2 && !eaten[0] && !eaten[1],
             "token: the now-Allowed engine did not take over the middle of the token");
  reference.HostInsert(L"bc");
  bool space_eaten = true;
  LocalCheck(SendVirtualKeyAdaptive(&route, VK_SPACE, L" ", &space_eaten, &error),
             "token: the boundary space was typed: " + error);
  LocalCheck(!space_eaten, "token: the boundary space also stayed with the host");
  reference.HostInsert(L" ");
  LocalCheck(test_case.doc->text_utf16() == reference.text,
             "token: the host-owned token is contiguous in the document");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "token: the engine is ready for the next token");
  LocalCheck(SendText(&route, L"de", true, &error),
             "token: a fresh token is eaten: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "token: the fresh boundary is eaten: " + error);
  reference.RawAppend(L"de ");
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 8000),
             "token: the reference document matches (host token + engine token)");
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
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "pressure: the engine path is ready");
  ReferenceDoc reference;

  // Phase A -- burst with the first edit deliberately stalled. Every key is eaten while the
  // document lock is held, so the engine backlog crosses the high water mark and the raw
  // catch-up takes over. Releasing the locks must apply the exact same text, once.
  std::wstring burst;
  for (int repeat = 0; repeat < 5; ++repeat) {
    burst += L"Aa1@x.y_Z-9";
  }
  test_case.host->SetHoldGrantMessages(true);
  std::string error;
  LocalCheck(SendText(&route, burst, true, &error),
             "pressure: the burst was accepted with every key eaten: " + error);
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "pressure: the closing space was eaten: " + error);
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  reference.RawAppend(burst);
  reference.RawAppend(L" ");
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 25000),
             "pressure: the exact burst text was applied once, in order");
  const std::wstring after_burst = test_case.doc->text_utf16();
  PumpSlice(*test_case.host, 300);
  LocalCheck(test_case.doc->text_utf16() == after_burst,
             "pressure: no late duplicate edit changed the document");
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "pressure: the engine is fresh after the degraded run");
  LocalCheck(SendText(&route, L"tail", true, &error) &&
                 SendVirtualKey(&route, VK_SPACE, true, &error),
             "pressure: the engine path accepts the next token: " + error);
  reference.RawAppend(L"tail ");
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 10000),
             "pressure: the next token follows the degraded run in order");

  // Phase B -- sustained typing while the engine consumes results but every edit is
  // stalled. This exercises the same admission path with a delayed *edit* rather than only
  // an unpumped burst.
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "pressure: ready for the sustained phase");
  std::wstring sustained;
  for (int repeat = 0; repeat < 3; ++repeat) {
    sustained += L"Qq2#r_s-T7";
  }
  test_case.host->SetHoldGrantMessages(true);
  for (std::size_t offset = 0; offset < sustained.size(); offset += 5) {
    const std::wstring group = sustained.substr(offset, 5);
    LocalCheck(SendText(&route, group, true, &error),
               "pressure: sustained group accepted: " + error);
    PumpSlice(*test_case.host, 10);
  }
  LocalCheck(SendVirtualKey(&route, VK_SPACE, true, &error),
             "pressure: the sustained closing space was eaten: " + error);
  test_case.host->SetHoldGrantMessages(false);
  (void)test_case.host->ReleaseHeldGrantMessages();
  reference.RawAppend(sustained);
  reference.RawAppend(L" ");
  LocalCheck(WaitForText(*test_case.host, test_case.doc, reference.text, 25000),
             "pressure: the sustained text was applied once, in order");
  EndCase(&test_case);

  // Deactivation while a degraded run is pending and the document lock is held must return
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
      (void)SendText(&stop_route, L"pending-degraded-run", true, &stop_error);
      LocalCheck(true, "pressure: a degraded run is pending at deactivation");
    }
    EndCase(&stopping);
    LocalCheck(true, "pressure: deactivation with a pending run returned");
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
    }
  }
  if (product_dll.empty() || rime_dll.empty() || data_dir.empty() ||
      lexicon.empty() || run_root.empty() || !run_root.is_absolute()) {
    std::cout
        << "usage error: --product-dll --rime-dll --data-dir --lexicon "
           "--run-root <absolute> [--mode input|engine-missing]\n";
    return 2;
  }

  setvbuf(stdout, nullptr, _IONBF, 0);
  SaveKeyStateOnce();
  std::error_code ec;
  std::filesystem::create_directories(run_root, ec);
  const std::filesystem::path stage = run_root / L"stage";
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
  } else if (mode == "engine-missing") {
    std::wstring why;
    const bool staged = StageProduct(stage, product_dll, rime_dll, data_dir, lexicon,
                                     false, &why);
    LocalCheck(staged, "missing engine: product staged without rime.dll");
    LocalCheck(PrepareUserRoot(user_root, settings, &why),
               "missing engine: settings and marker written");
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
