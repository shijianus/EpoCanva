// EpoCanva W2 -- deployment snapshot and cross-process start-coordination tests.
//
// The normal cases simulate deployment without starting an engine. The cleanup-lock
// case uses a test-only runtime DLL to prove failed cleanup retains the file locks.
// They exercise the real product configuration
// transaction (fingerprint, marker retirement, state record, named-mutex coordination)
// against throwaway user roots below --run-root. The product data layout is copied next to
// this executable by CMake, so ResolveProductPaths(GetModuleHandle(nullptr)) is the same
// code path the product DLL uses.
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "product_engine_config.h"
#include "product_io.h"
#include "product_paths.h"
#include "product_settings.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void ReleaseLeaseLock(epocanva::product::EngineStartTransaction* transaction) {
  void* lock = transaction->TakeLeaseLock();
  if (lock != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(lock));
  }
}

bool Exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

void ResetUserRoot(const std::filesystem::path& root) {
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root / L"engine", ec);
  std::filesystem::create_directories(root / L"logs", ec);
}

std::filesystem::path ExeDirectory() {
  std::vector<wchar_t> buffer(1024, L'\0');
  const DWORD written =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written == 0 || written >= buffer.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), written)).parent_path();
}

std::wstring Quote(const std::filesystem::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

struct ChildResult {
  bool launched = false;
  bool exited_zero = false;
  bool requested = false;
  bool completed = false;
  std::string fingerprint;
};

bool RunChild(const std::filesystem::path& exe, const std::filesystem::path& user_root,
              const std::filesystem::path& artifact, const std::filesystem::path& result,
              DWORD hold_ms, bool runtime_lease, PROCESS_INFORMATION* process) {
  std::wstring command = Quote(exe);
  command += L" --child";
  command += L" --user-root " + Quote(user_root);
  command += L" --artifact " + Quote(artifact);
  command += L" --result " + Quote(result);
  command += L" --hold-ms " + std::to_wstring(hold_ms);
  if (runtime_lease) {
    command += L" --mode runtime";
  }
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  const BOOL ok = ::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, nullptr, &startup, process);
  return ok != FALSE;
}

ChildResult ReadChildResult(const std::filesystem::path& result, DWORD wait_result,
                            DWORD exit_code) {
  ChildResult parsed;
  parsed.launched = true;
  parsed.exited_zero = wait_result == WAIT_OBJECT_0 && exit_code == 0;
  const std::string text = ReadText(result);
  parsed.requested = text.find("requested=1") != std::string::npos;
  parsed.completed = text.find("completed=1") != std::string::npos;
  const std::size_t marker = text.find("fingerprint=");
  if (marker != std::string::npos) {
    const std::size_t end = text.find('\n', marker);
    parsed.fingerprint =
        text.substr(marker + 12, end == std::string::npos ? std::string::npos
                                                          : end - marker - 12);
  }
  return parsed;
}

int RunChildMain(const std::filesystem::path& user_root,
                 const std::filesystem::path& artifact,
                 const std::filesystem::path& result, DWORD hold_ms, bool runtime_lease) {
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  if (!epocanva::product::ApplyProductEngineConfig(::GetModuleHandleW(nullptr), nullptr)) {
    WriteText(result, "completed=0\nreason=apply\n");
    return 3;
  }
  epocanva::product::EngineStartTransaction transaction;
  if (!transaction.Prepare(nullptr)) {
    WriteText(result, "completed=0\nreason=prepare\n");
    return 4;
  }
  const bool requested = transaction.deployment_requested();
  if (runtime_lease) {
    if (requested) {
      WriteText(result, "completed=0\nreason=runtime-requested-deployment\n");
      transaction.Abort();
      return 6;
    }
    const std::string fingerprint = transaction.fingerprint();
    if (!transaction.Complete(nullptr)) {
      WriteText(result, "completed=0\nreason=complete\n");
      return 5;
    }
    void* lease_lock = transaction.TakeLeaseLock();
    if (lease_lock == nullptr) {
      WriteText(result, "completed=0\nreason=no-lease-lock\n");
      return 7;
    }
    std::string text;
    text += "requested=0\ncompleted=1\nfingerprint=" + fingerprint + "\n";
    WriteText(result, text);
    WriteText(result.wstring() + L".ready", "ready=1\n");
    if (hold_ms > 0) {
      ::Sleep(hold_ms);
    }
    ::CloseHandle(static_cast<HANDLE>(lease_lock));
    return 0;
  }
  if (hold_ms > 0) {
    ::Sleep(hold_ms);
  }
  if (requested) {
    // Only a deploying activation produces a compiled schema. A non-deploying activation
    // must not touch the output another process is using.
    std::error_code ec;
    std::filesystem::create_directories(artifact.parent_path(), ec);
    WriteText(artifact, "simulated compiled schema\n");
  }
  const std::string fingerprint = transaction.fingerprint();
  if (!transaction.Complete(nullptr)) {
    WriteText(result, "completed=0\nreason=complete\n");
    return 5;
  }
  std::string text;
  text += requested ? "requested=1\n" : "requested=0\n";
  text += "completed=1\nfingerprint=" + fingerprint + "\n";
  WriteText(result, text);
  return 0;
}

void TestFingerprintLifecycle(const std::filesystem::path& user_root) {
  std::cout << "fingerprint lifecycle\n";
  ResetUserRoot(user_root);
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  Check(epocanva::product::ApplyProductEngineConfig(::GetModuleHandleW(nullptr), nullptr),
        "the product configuration resolves from the executable layout");

  epocanva::product::ProductPaths paths;
  std::wstring why;
  Check(epocanva::product::ResolveProductPaths(::GetModuleHandleW(nullptr), &paths, &why),
        "product paths resolve");
  Check(Exists(paths.schema_file) && Exists(paths.default_config) &&
            Exists(paths.lexicon_file),
        "test layout contains the product data files");

  epocanva::product::ProductSettings settings;
  settings.completion_enabled = true;
  settings.completion_candidates = 4;
  settings.spelling_enabled = false;
  settings.spelling_candidates = 2;
  std::string token;
  Check(epocanva::product::SaveSettingsBundle(paths, settings, &token, &why),
        "settings bundle and marker are written");
  Check(Exists(paths.deploy_marker_file), "the marker exists before the first start");

  epocanva::product::EngineStartTransaction first;
  Check(first.Prepare(&why), "first Prepare succeeds");
  Check(first.deployment_requested(), "a fresh user root requests deployment");
  first.Abort();
  Check(Exists(paths.deploy_marker_file),
        "Abort leaves the marker as the retry evidence");

  epocanva::product::EngineStartTransaction second;
  Check(second.Prepare(&why), "second Prepare succeeds after Abort");
  Check(second.deployment_requested(), "the aborted deployment is retried");
  std::error_code ec;
  std::filesystem::create_directories(paths.compiled_schema_file.parent_path(), ec);
  WriteText(paths.compiled_schema_file, "simulated compiled schema\n");
  const std::string expected_fingerprint = second.fingerprint();
  Check(second.Complete(&why), "Complete accepts the compiled artefact");
  ReleaseLeaseLock(&second);
  Check(!Exists(paths.deploy_marker_file), "the marker is retired after the record");
  Check(Exists(paths.deployed_state_file), "the deployment snapshot record exists");
  const std::string state = ReadText(paths.deployed_state_file);
  Check(state.find(expected_fingerprint) != std::string::npos,
        "the record contains the exact fingerprint");

  epocanva::product::EngineStartTransaction third;
  Check(third.Prepare(&why), "third Prepare succeeds");
  Check(!third.deployment_requested(),
        "an unchanged snapshot does not request a second deployment");
  Check(third.Complete(&why), "third Complete succeeds without writing");
  ReleaseLeaseLock(&third);

  settings.spelling_enabled = true;
  settings.spelling_candidates = 3;
  Check(epocanva::product::SaveSettingsBundle(paths, settings, &token, &why),
        "a changed settings bundle is written");
  epocanva::product::EngineStartTransaction changed;
  Check(changed.Prepare(&why), "Prepare after a settings change succeeds");
  Check(changed.deployment_requested(), "a changed settings snapshot requests deployment");
  std::filesystem::create_directories(paths.compiled_schema_file.parent_path(), ec);
  WriteText(paths.compiled_schema_file, "simulated compiled schema\n");
  Check(changed.Complete(&why), "Complete after a settings change succeeds");
  ReleaseLeaseLock(&changed);
  Check(!Exists(paths.deploy_marker_file), "the new marker is retired");
  const std::string changed_state = ReadText(paths.deployed_state_file);
  Check(changed_state.find(changed.fingerprint()) != std::string::npos,
        "the record names the new snapshot");

  WriteText(paths.deploy_marker_file, "not-a-sha256\n");
  epocanva::product::EngineStartTransaction malformed;
  Check(!malformed.Prepare(&why), "a malformed marker refuses preparation");
  Check(Exists(paths.deploy_marker_file), "the malformed marker is not deleted");
}

void TestCrossProcess(const std::filesystem::path& run_root) {
  std::cout << "cross-process coordination\n";
  const std::filesystem::path user_root = run_root / L"cross-user";
  const std::filesystem::path result_a = run_root / L"cross-a.txt";
  const std::filesystem::path result_b = run_root / L"cross-b.txt";
  ResetUserRoot(user_root);
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  Check(epocanva::product::ApplyProductEngineConfig(::GetModuleHandleW(nullptr), nullptr),
        "configuration applied for the cross-process case");

  epocanva::product::ProductPaths paths;
  std::wstring why;
  Check(epocanva::product::ResolveProductPaths(::GetModuleHandleW(nullptr), &paths, &why),
        "paths resolve for the cross-process case");
  epocanva::product::ProductSettings settings;
  std::string token;
  Check(epocanva::product::SaveSettingsBundle(paths, settings, &token, &why),
        "cross-process settings and marker are written");

  const std::filesystem::path artifact = paths.compiled_schema_file;
  const std::filesystem::path exe = ExeDirectory() / L"epocanva_product_deployment_tests.exe";
  Check(Exists(exe), "the test executable can re-launch itself");

  PROCESS_INFORMATION process_a = {};
  PROCESS_INFORMATION process_b = {};
  const bool launched_a =
      RunChild(exe, user_root, artifact, result_a, 500, false, &process_a);
  const bool launched_b =
      RunChild(exe, user_root, artifact, result_b, 0, false, &process_b);
  Check(launched_a && launched_b, "both child processes launch");
  DWORD wait_a = WAIT_FAILED;
  DWORD wait_b = WAIT_FAILED;
  DWORD code_a = 1;
  DWORD code_b = 1;
  if (launched_a) {
    wait_a = ::WaitForSingleObject(process_a.hProcess, 60000);
    ::GetExitCodeProcess(process_a.hProcess, &code_a);
    ::CloseHandle(process_a.hThread);
    ::CloseHandle(process_a.hProcess);
  }
  if (launched_b) {
    wait_b = ::WaitForSingleObject(process_b.hProcess, 60000);
    ::GetExitCodeProcess(process_b.hProcess, &code_b);
    ::CloseHandle(process_b.hThread);
    ::CloseHandle(process_b.hProcess);
  }
  const ChildResult a = ReadChildResult(result_a, wait_a, code_a);
  const ChildResult b = ReadChildResult(result_b, wait_b, code_b);
  Check(a.launched && b.launched, "both children report a result");
  Check(a.exited_zero && b.exited_zero, "both children exit successfully");
  Check(a.requested != b.requested,
        "exactly one of two racing processes performs the deployment");
  Check(!Exists(paths.deploy_marker_file),
        "the marker is retired once the deployment record exists");
  Check(Exists(paths.deployed_state_file), "the cross-process state record exists");
  const std::string state = ReadText(paths.deployed_state_file);
  const std::string winner = a.requested ? a.fingerprint : b.fingerprint;
  Check(!winner.empty() && state.find(winner) != std::string::npos,
        "the record matches the deploying process's fingerprint");
}

bool WaitForFile(const std::filesystem::path& path, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (Exists(path)) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    ::Sleep(20);
  }
}

bool WaitForProcess(PROCESS_INFORMATION* process, DWORD* exit_code, DWORD timeout_ms) {
  const DWORD waited = ::WaitForSingleObject(process->hProcess, timeout_ms);
  if (waited == WAIT_OBJECT_0) {
    ::GetExitCodeProcess(process->hProcess, exit_code);
  }
  ::CloseHandle(process->hThread);
  ::CloseHandle(process->hProcess);
  *process = PROCESS_INFORMATION{};
  return waited == WAIT_OBJECT_0;
}

// App A is running with the old compiled settings and holds the runtime shared lock for its
// whole engine lease. A changed settings bundle from app B must defer, keep the marker, and
// must never delete the compiled output under the live engine. A plain activation (no
// deployment) must still be admitted while that shared lock is held. Once app A exits, the
// next activation deploys the new snapshot.
void TestRuntimeLease(const std::filesystem::path& run_root) {
  std::cout << "runtime lease and deferred deployment\n";
  const std::filesystem::path user_root = run_root / L"runtime-user";
  const std::filesystem::path seed_result = run_root / L"runtime-seed.txt";
  const std::filesystem::path holder_result = run_root / L"runtime-holder.txt";
  const std::filesystem::path plain_result = run_root / L"runtime-plain.txt";
  const std::filesystem::path deferred_result = run_root / L"runtime-deferred.txt";
  const std::filesystem::path deploy_result = run_root / L"runtime-deploy.txt";
  ResetUserRoot(user_root);
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  Check(epocanva::product::ApplyProductEngineConfig(::GetModuleHandleW(nullptr), nullptr),
        "runtime: configuration applied");

  epocanva::product::ProductPaths paths;
  std::wstring why;
  Check(epocanva::product::ResolveProductPaths(::GetModuleHandleW(nullptr), &paths, &why),
        "runtime: paths resolve");
  epocanva::product::ProductSettings first;
  first.completion_enabled = true;
  first.completion_candidates = 4;
  first.spelling_enabled = true;
  first.spelling_candidates = 2;
  std::string token;
  auto save_bundle = [&paths](const epocanva::product::ProductSettings& settings,
                              std::string* saved_token, std::wstring* saved_why) {
    // The repository lives on a synced volume; a transient sharing violation on the atomic
    // replace is retried a few times, and the last real reason is still reported.
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (epocanva::product::SaveSettingsBundle(paths, settings, saved_token,
                                                saved_why)) {
        return true;
      }
      ::Sleep(50);
    }
    return false;
  };
  Check(save_bundle(first, &token, &why), "runtime: the first settings bundle is written");

  const std::filesystem::path exe =
      ExeDirectory() / L"epocanva_product_deployment_tests.exe";
  const std::filesystem::path artifact = paths.compiled_schema_file;

  PROCESS_INFORMATION seed = {};
  Check(RunChild(exe, user_root, artifact, seed_result, 0, false, &seed),
        "runtime: the seed child launches");
  DWORD seed_code = 1;
  Check(WaitForProcess(&seed, &seed_code, 60000), "runtime: the seed child exits");
  const ChildResult seeded = ReadChildResult(seed_result, WAIT_OBJECT_0, seed_code);
  Check(seeded.exited_zero && seeded.requested && !Exists(paths.deploy_marker_file),
        "runtime: the seed deployment is recorded and the marker retired");

  PROCESS_INFORMATION holder = {};
  Check(RunChild(exe, user_root, artifact, holder_result, 8000, true, &holder),
        "runtime: the holder child launches");
  const bool holder_ready = WaitForFile(holder_result.wstring() + L".ready", 30000);
  Check(holder_ready, "runtime: the holder acquired its runtime lease");
  if (!holder_ready) {
    (void)WaitForProcess(&holder, &seed_code, 30000);
    return;
  }

  const ULONGLONG plain_start = ::GetTickCount64();
  PROCESS_INFORMATION plain = {};
  Check(RunChild(exe, user_root, artifact, plain_result, 0, false, &plain),
        "runtime: a plain activation launches while the engine runs");
  DWORD plain_code = 1;
  const bool plain_exited = WaitForProcess(&plain, &plain_code, 30000);
  const ULONGLONG plain_elapsed = ::GetTickCount64() - plain_start;
  const ChildResult plain_activation =
      ReadChildResult(plain_result, plain_exited ? WAIT_OBJECT_0 : WAIT_TIMEOUT, plain_code);
  Check(plain_exited && plain_activation.exited_zero && !plain_activation.requested,
        "runtime: a second host joins the running engine without deploying");
  Check(plain_elapsed < 5000,
        "runtime: the shared runtime lock does not block a new host");

  epocanva::product::ProductSettings second = first;
  second.completion_candidates = 6;
  second.spelling_candidates = 3;
  Check(save_bundle(second, &token, &why),
        "runtime: a changed settings bundle is written while the engine runs (" +
            std::string(why.begin(), why.end()) + ")");
  const std::filesystem::path sentinel = paths.engine_user_dir / L"build" / L"keep.txt";
  {
    std::error_code ec;
    std::filesystem::create_directories(sentinel.parent_path(), ec);
    WriteText(sentinel, "compiled output in use\n");
  }

  PROCESS_INFORMATION deferred = {};
  Check(RunChild(exe, user_root, artifact, deferred_result, 0, false, &deferred),
        "runtime: a deploying activation launches while the engine runs");
  DWORD deferred_code = 1;
  Check(WaitForProcess(&deferred, &deferred_code, 60000),
        "runtime: the deploying activation returns (a bounded wait, not a deadlock)");
  const ChildResult deferral =
      ReadChildResult(deferred_result, WAIT_OBJECT_0, deferred_code);
  Check(deferral.exited_zero && !deferral.requested,
        "runtime: the deployment defers while the shared lock is held");
  Check(Exists(paths.deploy_marker_file),
        "runtime: the deferred marker survives for the next real start");
  Check(Exists(sentinel),
        "runtime: the compiled output in use was not deleted by the deferral");
  const std::string during = ReadText(paths.deployed_state_file);
  Check(during.find(deferral.fingerprint) == std::string::npos,
        "runtime: no new fingerprint is recorded before the deployment ran");

  DWORD holder_code = 1;
  Check(WaitForProcess(&holder, &holder_code, 30000), "runtime: the holder exits");
  const ChildResult held = ReadChildResult(holder_result, WAIT_OBJECT_0, holder_code);
  Check(held.exited_zero && !held.requested, "runtime: the holder never deployed");

  PROCESS_INFORMATION deployed = {};
  Check(RunChild(exe, user_root, artifact, deploy_result, 0, false, &deployed),
        "runtime: a deploying activation launches after the holder exits");
  DWORD deploy_code = 1;
  Check(WaitForProcess(&deployed, &deploy_code, 60000), "runtime: the deployment exits");
  const ChildResult applied = ReadChildResult(deploy_result, WAIT_OBJECT_0, deploy_code);
  Check(applied.exited_zero && applied.requested,
        "runtime: the next real start performs the deferred deployment");
  Check(!Exists(paths.deploy_marker_file),
        "runtime: the marker is retired after the applied deployment");
  Check(!Exists(sentinel),
        "runtime: the deployment resets the stale compiled output");
  const std::string after = ReadText(paths.deployed_state_file);
  Check(after.find(applied.fingerprint) != std::string::npos,
        "runtime: the record names the newly deployed fingerprint");
}

int ProbeCleanupLocks(const std::filesystem::path& user_root) {
  for (const wchar_t* name : {L"engine.lock", L"shared-cleanup.lock"}) {
    const auto path = user_root / L"engine" / name;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "cleanup child: lock file opens");
    if (file == INVALID_HANDLE_VALUE) {
      continue;
    }
    OVERLAPPED overlap = {};
    const BOOL acquired = ::LockFileEx(file,
        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap);
    const DWORD error = acquired ? ERROR_SUCCESS : ::GetLastError();
    Check(!acquired && error == ERROR_LOCK_VIOLATION,
          "cleanup child: exclusive deployment access is refused");
    ::CloseHandle(file);
  }
  const std::string utf8 = epocanva::product::WideToUtf8((user_root / L"engine").wstring());
  std::string digest;
  Check(epocanva::product::Sha256(utf8.data(), utf8.size(), &digest, nullptr),
        "cleanup child: start mutex identity resolves");
  const std::wstring name = L"Local\\EpoCanva.Deploy." + epocanva::product::Utf8ToWide(digest);
  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, name.c_str());
  Check(mutex != nullptr, "cleanup child: start mutex opens");
  if (mutex != nullptr) {
    const DWORD wait = ::WaitForSingleObject(mutex, 1000);
    Check(wait == WAIT_OBJECT_0, "cleanup child: start mutex was released normally");
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      ::ReleaseMutex(mutex);
    }
    ::CloseHandle(mutex);
  }
  return g_failures == 0 ? 0 : 14;
}

void TestCleanupLock(const std::filesystem::path& run_root,
                     const std::filesystem::path& fault_dll) {
  const auto user_root = run_root / L"cleanup-user";
  ResetUserRoot(user_root);
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  Check(epocanva::product::ApplyProductEngineConfig(::GetModuleHandleW(nullptr), nullptr),
        "cleanup: product configuration applied");
  epocanva::product::EngineStartTransaction transaction;
  std::wstring why;
  if (!transaction.Prepare(&why)) {
    Check(false, "cleanup: start transaction acquired its locks");
    return;
  }

  HMODULE fixture = ::LoadLibraryExW(fault_dll.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  Check(fixture != nullptr, "cleanup: fault API DLL loaded");
  if (fixture == nullptr) {
    return;
  }
  using Configure = void (*)(int);
  const auto configure = reinterpret_cast<Configure>(
      ::GetProcAddress(fixture, "RuntimeFaultConfigure"));
  Check(configure != nullptr, "cleanup: fault API configuration available");
  if (configure == nullptr) {
    ::FreeLibrary(fixture);
    return;
  }
  configure(2);  // initialize and its cleanup finalize both throw
  epocanva::tsf::EngineHostConfig config;
  Check(transaction.config(&config), "cleanup: transaction configuration captured");
  config.rime_dll_path = fault_dll;
  config.deployment.reset();
  epocanva::tsf::EngineLease lease;
  const auto acquired = epocanva::tsf::EngineHostBroker::Instance().Acquire(config, &lease);
  Check(acquired == epocanva::tsf::EngineLeaseStatus::kCleanupFailed,
        "cleanup: the real broker observes the injected runtime cleanup failure");
  epocanva::tsf::EngineHostDiagnostics diagnostics;
  epocanva::tsf::EngineHostBroker::Instance().Snapshot(&diagnostics);
  Check(diagnostics.cleanup_failed && diagnostics.state == epocanva::tsf::EngineHostState::kFailed,
        "cleanup: broker failure is persistent and observable");
  transaction.Abort();  // retain engine.lock; release the separate named start mutex

  // The attachment owner's deleter uses this same release function after a failed stop.
  HANDLE shared = ::CreateFileW((user_root / L"engine" / L"shared-cleanup.lock").c_str(),
      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(shared != INVALID_HANDLE_VALUE, "cleanup: shared runtime lock opens");
  if (shared != INVALID_HANDLE_VALUE) {
    OVERLAPPED overlap = {};
    Check(::LockFileEx(shared, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap) != FALSE,
          "cleanup: shared runtime lock acquired");
    epocanva::product::ReleaseEngineLeaseLock(shared);
  }
  const auto exe = ExeDirectory() / L"epocanva_product_deployment_tests.exe";
  std::wstring command = Quote(exe) + L" --mode cleanup-probe --user-root " + Quote(user_root);
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const bool launched = ::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr,
      FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
  Check(launched, "cleanup: independent lock-probe process launches");
  if (launched) {
    DWORD exit_code = 1;
    Check(WaitForProcess(&process, &exit_code, 10000) && exit_code == 0,
          "cleanup: another process cannot take exclusive locks but can take the start mutex");
  }
  ::FreeLibrary(fixture);
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path run_root;
  std::filesystem::path child_user_root;
  std::filesystem::path child_artifact;
  std::filesystem::path child_result;
  DWORD hold_ms = 0;
  bool child = false;
  bool child_runtime = false;
  std::string mode;
  std::filesystem::path fault_dll;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--run-root" && index + 1 < argc) {
      run_root = std::filesystem::path(argv[++index]);
    } else if (argument == "--child") {
      child = true;
    } else if (argument == "--mode" && index + 1 < argc) {
      mode = argv[++index];
      child_runtime = mode == "runtime";
    } else if (argument == "--fault-dll" && index + 1 < argc) {
      fault_dll = std::filesystem::path(argv[++index]);
    } else if (argument == "--user-root" && index + 1 < argc) {
      child_user_root = std::filesystem::path(argv[++index]);
    } else if (argument == "--artifact" && index + 1 < argc) {
      child_artifact = std::filesystem::path(argv[++index]);
    } else if (argument == "--result" && index + 1 < argc) {
      child_result = std::filesystem::path(argv[++index]);
    } else if (argument == "--hold-ms" && index + 1 < argc) {
      hold_ms = static_cast<DWORD>(std::stoul(argv[++index]));
    }
  }
  if (mode == "cleanup-probe") {
    return child_user_root.is_absolute() ? ProbeCleanupLocks(child_user_root) : 2;
  }
  if (child) {
    if (child_user_root.empty() || child_artifact.empty() || child_result.empty()) {
      return 2;
    }
    return RunChildMain(child_user_root, child_artifact, child_result, hold_ms,
                        child_runtime);
  }
  if (run_root.empty() || !run_root.is_absolute()) {
    std::cout << "FAIL: --run-root <absolute path> is required\n";
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(run_root, ec);

  if (mode == "cleanup-lock") {
    if (!fault_dll.is_absolute()) {
      return 2;
    }
    TestCleanupLock(run_root, fault_dll);
  } else {
    TestFingerprintLifecycle(run_root / L"fingerprint-user");
    TestCrossProcess(run_root);
    TestRuntimeLease(run_root);
  }

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
