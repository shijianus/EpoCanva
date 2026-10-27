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
                     std::wstring* why) {
  std::error_code ec;
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

bool WaitForText(TestHost& host, TestDocument* document,
                 const std::wstring& expected, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (document->text_utf16() == expected) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      return document->text_utf16() == expected;
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

bool SendVirtualKey(ITfKeyEventSink* sink, ITfContext* context, UINT vk,
                    bool expect_eaten, std::string* error) {
  const LPARAM down = KeyLparam(vk, true, false);
  BOOL test_eaten = FALSE;
  HRESULT hr = sink->OnTestKeyDown(context, vk, down, &test_eaten);
  if (FAILED(hr)) {
    *error = "OnTestKeyDown failed";
    return false;
  }
  BOOL eaten = FALSE;
  hr = sink->OnKeyDown(context, vk, down, &eaten);
  if (FAILED(hr)) {
    *error = "OnKeyDown failed";
    return false;
  }
  BOOL up_eaten = TRUE;
  hr = sink->OnKeyUp(context, vk, KeyLparam(vk, false, false), &up_eaten);
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
  if ((eaten != FALSE) != expect_eaten) {
    *error = expect_eaten ? "a key was not eaten" : "a key was eaten";
    std::cerr << "  send expectation failed expect=" << expect_eaten
              << " key=" << (eaten != FALSE) << " vk=" << vk << std::endl;
    return false;
  }
  if (up_eaten != FALSE) {
    *error = "a key-up was eaten";
    return false;
  }
  return true;
}

bool SendChar(ITfKeyEventSink* sink, ITfContext* context, WCHAR ch,
              bool expect_eaten, std::string* error) {
  const SHORT mapped = ::VkKeyScanW(ch);
  if (mapped == -1) {
    *error = "the active layout cannot type a test character";
    return false;
  }
  const UINT vk = static_cast<UINT>(LOBYTE(mapped));
  const BYTE modifiers = HIBYTE(mapped);
  SetStateForKey(ch, (modifiers & 1) != 0, (modifiers & 2) != 0,
                 (modifiers & 4) != 0);
  return SendVirtualKey(sink, context, vk, expect_eaten, error);
}

bool SendText(ITfKeyEventSink* sink, ITfContext* context, const std::wstring& text,
              bool expect_eaten, std::string* error) {
  for (const WCHAR ch : text) {
    if (!SendChar(sink, context, ch, expect_eaten, error)) {
      return false;
    }
  }
  return true;
}

bool SendShortcut(ITfKeyEventSink* sink, ITfContext* context, UINT vk,
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
  return SendVirtualKey(sink, context, vk, false, error);
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
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "the product controller bound and resolved permission");
  LocalCheck(::GetFileAttributesW(paths.marker.c_str()) == INVALID_FILE_ATTRIBUTES,
             "the deployment marker was retired after the successful start");
  LocalCheck(std::filesystem::is_regular_file(paths.state),
             "the deployment snapshot was recorded");
  LocalCheck(std::filesystem::is_regular_file(paths.compiled),
             "the compiled schema exists in the user engine directory");

  std::string error;
  LocalCheck(SendText(sink, context, L"Hello", true, &error), "type Hello: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space while composing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello ", 5000),
             "Hello + Space commits the preserved case");

  error.clear();
  LocalCheck(SendText(sink, context, L"helo", true, &error), "type helo: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space after helo is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello helo ", 5000),
             "default raw input does not auto-correct helo");

  error.clear();
  LocalCheck(SendText(sink, context, L"helo", true, &error), "type helo again: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_BACK, true, &error),
             "BackSpace while composing is eaten: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space after BackSpace is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc, L"Hello helo hel ", 5000),
             "BackSpace edits the composition");

  error.clear();
  LocalCheck(SendText(sink, context, L"123-45_67@x.y", true, &error),
             "digits and technical marks are forwarded: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space after marks is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y ", 5000),
             "digits and marks are committed unchanged");

  error.clear();
  LocalCheck(SendText(sink, context, L"user@example.com", true, &error),
             "email is accepted: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space after email is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com ", 5000),
             "email is committed unchanged");

  error.clear();
  LocalCheck(SendShortcut(sink, context, 'C', true, false, false, &error),
             "Ctrl+C passes to the host: " + error);
  LocalCheck(SendShortcut(sink, context, 'V', true, false, false, &error),
             "Ctrl+V passes to the host: " + error);
  LocalCheck(SendShortcut(sink, context, 'Z', true, false, false, &error),
             "Ctrl+Z passes to the host: " + error);
  LocalCheck(SendShortcut(sink, context, 'X', false, true, false, &error),
             "Alt+X passes to the host: " + error);
  LocalCheck(SendShortcut(sink, context, 'R', false, false, true, &error),
             "Win+R passes to the host: " + error);
  LocalCheck(test_case.doc->text_utf16() ==
                 L"Hello helo hel 123-45_67@x.y user@example.com ",
             "shortcut pass-through changed no text");

  error.clear();
  LocalCheck(SendText(sink, context, L"quickbrownfox", true, &error),
             "fast typing is accepted back to back: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
             "space after fast typing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com quickbrownfox ",
                         8000),
             "fast typing keeps order and commits once");

  error.clear();
  LocalCheck(SendText(sink, context, L"zzz", true, &error), "type zzz: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_ESCAPE, true, &error),
             "Escape while composing is eaten: " + error);
  LocalCheck(WaitForText(*test_case.host, test_case.doc,
                         L"Hello helo hel 123-45_67@x.y user@example.com quickbrownfox ",
                         5000),
             "Escape cancels the composition without changing committed text");

  EndCase(&test_case);
  return g_failures == 0;
}

bool RunPermissionCase(const Paths& paths, TestDocumentOptions options,
                       const std::string& label) {
  Case test_case;
  options.initial_text_utf16 = L"protected";
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
  LocalCheck(SUCCEEDED(hr) && test_eaten == FALSE,
             label + ": prediction passes through");
  BOOL eaten = TRUE;
  test_case.product.sink->OnKeyDown(context, vk, KeyLparam(vk, true, false), &eaten);
  LocalCheck(eaten == FALSE, label + ": the key is not eaten");
  LocalCheck(test_case.doc->text_utf16() == L"protected",
             label + ": the document is unchanged");
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
  LocalCheck(ReadyToType(*test_case.host, sink, context, 15000),
             "external edit: ready to type");
  std::string error;
  LocalCheck(SendText(sink, context, L"abc", true, &error), "external edit: type abc: " + error);
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
  LocalCheck(SendText(sink, context, L"def", true, &error),
             "external edit: typing after the external write is accepted: " + error);
  LocalCheck(SendVirtualKey(sink, context, VK_SPACE, true, &error),
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
  if (!ReadyToType(*host, product.sink, context, 15000)) {
    result->error = "the session never became ready";
  } else if (!SendText(product.sink, context, word, true, &result->error) ||
             !SendVirtualKey(product.sink, context, VK_SPACE, true, &result->error) ||
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

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path product_dll;
  std::filesystem::path rime_dll;
  std::filesystem::path data_dir;
  std::filesystem::path lexicon;
  std::filesystem::path run_root;
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
  const std::filesystem::path user_root = run_root / L"user";
  const std::filesystem::path marker = user_root / L"engine" / L"deploy-required";
  const std::filesystem::path state = user_root / L"engine" / L"deployed-state";
  const std::filesystem::path compiled =
      user_root / L"engine" / L"build" / L"epocanva_english.schema.yaml";

  epocanva::product::ProductSettings settings;
  settings.completion_enabled = true;
  settings.completion_candidates = 4;
  settings.spelling_enabled = false;
  settings.spelling_candidates = 2;

  if (mode == "multi") {
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
