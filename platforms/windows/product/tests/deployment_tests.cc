// EpoCanva W2 -- deployment snapshot and cross-process start-coordination tests.
//
// These cases never start the engine. They exercise the real product configuration
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
              DWORD hold_ms, PROCESS_INFORMATION* process) {
  std::wstring command = Quote(exe);
  command += L" --child";
  command += L" --user-root " + Quote(user_root);
  command += L" --artifact " + Quote(artifact);
  command += L" --result " + Quote(result);
  command += L" --hold-ms " + std::to_wstring(hold_ms);
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
                 const std::filesystem::path& result, DWORD hold_ms) {
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
  if (hold_ms > 0) {
    ::Sleep(hold_ms);
  }
  {
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
      RunChild(exe, user_root, artifact, result_a, 500, &process_a);
  const bool launched_b =
      RunChild(exe, user_root, artifact, result_b, 0, &process_b);
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

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path run_root;
  std::filesystem::path child_user_root;
  std::filesystem::path child_artifact;
  std::filesystem::path child_result;
  DWORD hold_ms = 0;
  bool child = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--run-root" && index + 1 < argc) {
      run_root = std::filesystem::path(argv[++index]);
    } else if (argument == "--child") {
      child = true;
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
  if (child) {
    if (child_user_root.empty() || child_artifact.empty() || child_result.empty()) {
      return 2;
    }
    return RunChildMain(child_user_root, child_artifact, child_result, hold_ms);
  }
  if (run_root.empty() || !run_root.is_absolute()) {
    std::cout << "FAIL: --run-root <absolute path> is required\n";
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(run_root, ec);

  TestFingerprintLifecycle(run_root / L"fingerprint-user");
  TestCrossProcess(run_root);

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
