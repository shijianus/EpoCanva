// EpoCanva P3.11/P3.13 -- implementation of the non-interactive lab checks.
#include "lab_self_test.h"

#include <windows.h>
#include <bcrypt.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "lab_run_root.h"
#include "lab_window.h"
#include "scenario_runner.h"

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what,
           const std::string& got = std::string()) {
  ++g_total;
  if (condition) {
    std::printf("[lab-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::printf("[lab-win] FAIL %s", what.c_str());
  if (!got.empty()) {
    std::printf(" (got: %s)", got.c_str());
  }
  std::printf("\n");
}

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

bool RunWindowLifecycle() {
  LabWindowOptions options;
  options.show = false;
  options.start_session = false;
  options.use_window_focus = false;

  HWND top = nullptr;
  HWND document_a = nullptr;
  HWND document_b = nullptr;
  {
    std::string error;
    std::unique_ptr<LabWindow> window = LabWindow::Create(options, &error);
    Check(window != nullptr, "window lifecycle: lab window created", error);
    if (window == nullptr) {
      return false;
    }
    top = window->hwnd();
    document_a = window->document_window(0);
    document_b = window->document_window(1);
    Check(top != nullptr && ::IsWindow(top),
          "window lifecycle: top-level HWND exists");
    Check(document_a != nullptr && ::IsWindow(document_a) &&
              document_b != nullptr && ::IsWindow(document_b),
          "window lifecycle: both document HWNDs exist");
    Check(window->nccreate_messages() == 1 &&
              window->create_messages() == 1,
          "window lifecycle: WM_NCCREATE then WM_CREATE ran once",
          "nccreate=" + std::to_string(window->nccreate_messages()) +
              " create=" + std::to_string(window->create_messages()));
    Check(window->status_window() != nullptr &&
              ::IsWindow(window->status_window()),
          "window lifecycle: status control exists");
    Check(::GetWindowLongPtrW(top, GWLP_USERDATA) != 0,
          "window lifecycle: the state pointer is bound during creation");
    Check(window->classes_registered(),
          "window lifecycle: the window classes are registered");
    window->Destroy();
    Check(!::IsWindow(top), "window lifecycle: the top-level HWND is destroyed");
    Check(!::IsWindow(document_a) && !::IsWindow(document_b),
          "window lifecycle: both document HWNDs are destroyed");
    Check(window->ncdestroy_messages() == 1,
          "window lifecycle: WM_NCDESTROY ran once",
          std::to_string(window->ncdestroy_messages()));
    Check(!window->classes_registered(),
          "window lifecycle: the window classes were released");
    Check(!window->create_rejected(),
          "window lifecycle: the window procedure did not reject creation",
          window->create_error());
  }
  Check(LabWindow::LiveWindowCount() == 0,
        "window lifecycle: no live lab window remains",
        std::to_string(LabWindow::LiveWindowCount()));
  Check(!LabWindow::ClassesRegistered(),
        "window lifecycle: the class registry is empty");

  // Re-create after the unregister to prove the class path can be entered again.
  {
    std::string error;
    std::unique_ptr<LabWindow> second = LabWindow::Create(options, &error);
    Check(second != nullptr,
          "window lifecycle: a second window registers the classes again", error);
    if (second != nullptr) {
      Check(second->nccreate_messages() == 1 &&
                second->create_messages() == 1,
            "window lifecycle: the second window ran creation messages once");
      second->Destroy();
    }
  }
  Check(LabWindow::LiveWindowCount() == 0,
        "window lifecycle: the second window is gone",
        std::to_string(LabWindow::LiveWindowCount()));
  return g_failed == 0;
}

bool CheckCleanupReports(LabSession* session) {
  if (session == nullptr) {
    Check(false, "resource balance: the session still exists");
    return false;
  }
  Check(session->start_status() == engine::StartStatus::kOk,
        "resource balance: the executor started",
        engine::DescribeStartStatus(session->start_status()));
  Check(session->stop_status() == engine::StopStatus::kOk,
        "resource balance: the executor stopped and joined",
        engine::DescribeStopStatus(session->stop_status()));
  const engine::ExecutorRuntimeReport& runtime = session->runtime_report();
  Check(runtime.state == engine::ExecutorState::kStopped,
        "resource balance: the executor reached Stopped");
  Check(runtime.runtime_start_status == engine::RuntimeStartStatus::kOk,
        "resource balance: the engine runtime started");
  Check(runtime.runtime_shutdown_status == engine::RuntimeShutdownStatus::kOk,
        "resource balance: the engine runtime shut down");
  Check(runtime.runtime_info.initialize_calls == 1 &&
            runtime.runtime_info.finalize_calls == 1,
        "resource balance: initialize/finalize are paired",
        "initialize=" + std::to_string(runtime.runtime_info.initialize_calls) +
            " finalize=" + std::to_string(runtime.runtime_info.finalize_calls));
  Check(runtime.client_close_failures == 0,
        "resource balance: no client close failed");
  Check(runtime.live_sessions_after_cleanup == 0,
        "resource balance: no engine session remained",
        std::to_string(runtime.live_sessions_after_cleanup));
  Check(runtime.api_counts.balanced(),
        "resource balance: engine API acquire/free counters are balanced");

  const edit::test::TestHostCleanupReport& cleanup = session->cleanup_report();
  Check(cleanup.shutdown_called, "resource balance: the TSF host was shut down");
  Check(cleanup.com_initialized && cleanup.com_uninitialized,
        "resource balance: COM initialized and uninitialized");
  Check(cleanup.window_created && cleanup.window_destroyed,
        "resource balance: the host message window was destroyed");
  Check(cleanup.thread_manager_created && cleanup.thread_manager_deactivated &&
            cleanup.thread_manager_released,
        "resource balance: the thread manager was deactivated and released");
  Check(cleanup.documents_created == 2 && cleanup.documents_destroyed == 2,
        "resource balance: both documents were created and destroyed",
        std::to_string(cleanup.documents_created) + " vs " +
            std::to_string(cleanup.documents_destroyed));
  Check(cleanup.all_documents_disconnected &&
            cleanup.all_document_managers_released &&
            cleanup.all_contexts_released,
        "resource balance: contexts and document managers were released");
  Check(cleanup.document_managers_still_enumerated <=
            cleanup.foreign_document_managers_at_shutdown_start,
        "resource balance: no TEST document manager remained enumerated "
        "(a real focused window may leave one TSF-owned manager; recorded as foreign)",
        "still=" + std::to_string(cleanup.document_managers_still_enumerated) +
            " foreign_at_start=" +
            std::to_string(cleanup.foreign_document_managers_at_shutdown_start));
  Check(cleanup.store_instances_alive == 0,
        "resource balance: no text store remained alive",
        std::to_string(cleanup.store_instances_alive));
  Check(cleanup.advised_store_sinks_alive == 0,
        "resource balance: no store sink remained advised",
        std::to_string(cleanup.advised_store_sinks_alive));
  Check(cleanup.forced_store_sink_releases == 0,
        "resource balance: no sink had to be force-released",
        std::to_string(cleanup.forced_store_sink_releases));
  Check(cleanup.store_reference_count_after_shutdown == 0,
        "resource balance: the last store reference count is zero",
        std::to_string(cleanup.store_reference_count_after_shutdown));

  const LabSnapshot snapshot = session->Snapshot();
  Check(snapshot.retire_drain_complete && !snapshot.retire_drain_timed_out,
        "resource balance: the retirement drain completed before StopAndJoin",
        "complete=" + std::to_string(snapshot.retire_drain_complete ? 1 : 0) +
            " timed_out=" + std::to_string(snapshot.retire_drain_timed_out ? 1 : 0));
  Check(snapshot.retirement.retiring_bindings == 0,
        "resource balance: no retiring binding remained",
        std::to_string(snapshot.retirement.retiring_bindings));
  Check(coordinator::RetirementAccountingBalanced(snapshot.retirement),
        "resource balance: every settled cleanup has exactly one attribution",
        "settled=" + std::to_string(snapshot.retirement.settled) +
            " finished=" + std::to_string(snapshot.retirement.finished) +
            " already_ordinary=" +
            std::to_string(snapshot.retirement.already_ordinary) +
            " external=" + std::to_string(snapshot.retirement.externally_terminated) +
            " blocked=" + std::to_string(snapshot.retirement.blocked));
  if (snapshot.retirement.settled > 0) {
    Check(coordinator::RetirementCleanupEvidenceConsistent(snapshot.retirement),
          "resource balance: the last cleanup outcome is backed by its operation evidence",
          std::string(coordinator::DescribeCleanupState(
              snapshot.retirement.last_cleanup_state)));
  }
  Check(snapshot.client_usage_after_drain.live == 0,
        "resource balance: no live engine client remained before StopAndJoin",
        std::to_string(snapshot.client_usage_after_drain.live));
  Check(snapshot.client_usage_after_drain.created ==
            snapshot.client_usage_after_drain.closed,
        "resource balance: every created engine client reached the closed state",
        std::to_string(snapshot.client_usage_after_drain.created) + " vs " +
            std::to_string(snapshot.client_usage_after_drain.closed));
  Check(snapshot.capacity_after_drain.occupied == 0,
        "resource balance: no executor result slot remained occupied",
        std::to_string(snapshot.capacity_after_drain.occupied));
  Check(snapshot.client_usage_after_drain.close_requests ==
            snapshot.client_usage_after_drain.close_results_taken,
        "resource balance: every accepted close result was consumed",
        std::to_string(snapshot.client_usage_after_drain.close_requests) + "/" +
            std::to_string(snapshot.client_usage_after_drain.close_results_taken));
  for (int index = 0; index < 2; ++index) {
    Check(snapshot.association_cleared[index],
          std::string("resource balance: association ") +
              (index == 0 ? "A" : "B") + " was cleared",
          HrText(snapshot.association_clear_hr[index]));
    Check(SUCCEEDED(snapshot.association_clear_hr[index]),
          std::string("resource balance: clearing association ") +
              (index == 0 ? "A" : "B") + " succeeded",
          HrText(snapshot.association_clear_hr[index]));
    Check(snapshot.association_previous_released[index],
          std::string("resource balance: previous manager for ") +
              (index == 0 ? "A" : "B") + " was released");
  }
  Check(snapshot.engine_clients_created >= 1,
        "resource balance: at least one engine client was created",
        std::to_string(snapshot.engine_clients_created));
  return g_failed == 0;
}

bool RunScenarios(const LabConfig& config) {
  LabWindowOptions options;
  options.config = config;
  options.show = false;
  options.start_session = true;
  // P3.13: the hidden scenario window still uses the REAL Win32 focus path. SetFocus on
  // the hidden document child is a real thread-focus fact (not a synthesized TSF
  // notification), recorded as win32-thread-focus-revalidation. The TSF-only path leaves
  // the thread with no Win32 focus evidence and the target guard refuses input.
  options.use_window_focus = true;

  std::string error;
  std::unique_ptr<LabWindow> window = LabWindow::Create(options, &error);
  Check(window != nullptr, "scenarios: hidden lab window created", error);
  if (window == nullptr) {
    return false;
  }
  Check(window->session_ready(), "scenarios: TSF session is ready",
        window->startup_error());
  if (!window->session_ready()) {
    window->Destroy();
    return false;
  }
  ScenarioRunner* runner = window->runner();
  Check(runner != nullptr, "scenarios: the fixed scenario runner exists");
  if (runner == nullptr) {
    window->Destroy();
    return false;
  }
  runner->SetProgress([](const std::string& line) {
    std::printf("[lab-win] evidence %s\n", line.c_str());
  });

  bool all_ok = true;
  for (int scenario = 1; scenario <= 9; ++scenario) {
    const ScenarioResult result =
        runner->Run(static_cast<ScenarioId>(scenario));
    Check(result.ok(),
          std::string("scenario ") + ScenarioRunner::Name(
              static_cast<ScenarioId>(scenario)) + " completed",
          result.detail);
    if (!result.ok()) {
      all_ok = false;
      break;
    }
  }

  LabSession* session = window->session();
  session->Shutdown();
  CheckCleanupReports(session);
  window->ShutdownSession();
  window->Destroy();
  window.reset();
  Check(LabWindow::LiveWindowCount() == 0,
        "scenarios: no live lab window remains",
        std::to_string(LabWindow::LiveWindowCount()));
  return all_ok && g_failed == 0;
}

namespace fs = std::filesystem;

std::string NarrowPath(const fs::path& path) { return path.u8string(); }

bool Sha256File(const fs::path& file, std::string* hex, std::string* error) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    *error = "cannot open " + NarrowPath(file);
    return false;
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  if (input.bad()) {
    *error = "cannot read " + NarrowPath(file);
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  NTSTATUS status = ::BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status >= 0) {
    status = ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
  }
  if (status >= 0 && !bytes.empty()) {
    status = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(
                                       const_cast<char*>(bytes.data())),
                              static_cast<ULONG>(bytes.size()), 0);
  }
  std::array<unsigned char, 32> digest = {};
  if (status >= 0) {
    status = ::BCryptFinishHash(hash, digest.data(),
                                static_cast<ULONG>(digest.size()), 0);
  }
  if (hash != nullptr) {
    ::BCryptDestroyHash(hash);
  }
  if (algorithm != nullptr) {
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  if (status < 0) {
    *error = "cannot compute SHA-256 for " + NarrowPath(file);
    return false;
  }
  char text[65] = {};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    std::snprintf(text + index * 2, 3, "%02x",
                  static_cast<unsigned int>(digest[index]));
  }
  *hex = text;
  return true;
}

bool ReadAll(const fs::path& file, std::string* bytes) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    return false;
  }
  bytes->assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
  return !input.bad();
}

bool SeedSentinel(const fs::path& file, const std::string& bytes,
                  std::string* hash) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output.good()) {
    return false;
  }
  std::string error;
  if (!Sha256File(file, hash, &error)) {
    return false;
  }
  std::printf("[lab-win] evidence sentinel %s sha256=%s (before)\n",
              NarrowPath(file).c_str(), hash->c_str());
  return true;
}

void CheckSentinel(const fs::path& file, const std::string& expected,
                   const std::string& before, const std::string& what) {
  std::string bytes;
  std::string after;
  std::string error;
  const bool read_ok = ReadAll(file, &bytes);
  const bool hash_ok = Sha256File(file, &after, &error);
  Check(read_ok && bytes == expected, what + ": content unchanged",
        read_ok ? bytes : std::string("unreadable"));
  Check(hash_ok && !before.empty() && after == before,
        what + ": full SHA-256 unchanged",
        "before=" + before + " after=" + after + " " + error);
  if (hash_ok) {
    std::printf("[lab-win] evidence sentinel %s sha256=%s (after)\n",
                NarrowPath(file).c_str(), after.c_str());
  }
}

bool Equivalent(const fs::path& left, const fs::path& right) {
  std::error_code ec;
  const bool same = fs::equivalent(left, right, ec);
  return !ec && same;
}

std::size_t CountEntries(const fs::path& directory) {
  std::error_code ec;
  std::size_t count = 0;
  fs::directory_iterator iterator(directory, ec);
  const fs::directory_iterator end;
  while (!ec && iterator != end) {
    ++count;
    iterator.increment(ec);
  }
  return count;
}

// The user-mode SDK does not publish REPARSE_DATA_BUFFER (it lives in the WDK). This
// test-only definition mirrors the documented mount-point layout used with
// FSCTL_SET_REPARSE_POINT.
struct LabMountPointReparseData {
  USHORT SubstituteNameOffset;
  USHORT SubstituteNameLength;
  USHORT PrintNameOffset;
  USHORT PrintNameLength;
  WCHAR PathBuffer[1];
};

union LabReparseUnion {
  LabMountPointReparseData MountPointReparseBuffer;
  UCHAR GenericReparseBuffer[1];
};

struct LabReparseDataBuffer {
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  LabReparseUnion Reparse;
};

// Test fixture only: turns an empty directory into a junction without requiring the
// symlink privilege. The target is a disposable directory created by the same test.
bool CreateDirectoryJunction(const fs::path& link, const fs::path& target,
                             std::string* error) {
  std::error_code ec;
  if (!fs::create_directory(link, ec) || ec) {
    *error = "cannot create the junction stub (" + ec.message() + ")";
    return false;
  }
  const fs::path absolute_target = fs::absolute(target, ec);
  if (ec) {
    *error = "cannot resolve the junction target (" + ec.message() + ")";
    return false;
  }
  const std::wstring substitute = L"\\??\\" + absolute_target.native();
  const std::size_t path_bytes = substitute.size() * sizeof(wchar_t);
  const std::size_t header_bytes =
      offsetof(LabReparseDataBuffer, Reparse);
  const std::size_t path_offset =
      offsetof(LabReparseUnion, MountPointReparseBuffer) +
      offsetof(LabMountPointReparseData, PathBuffer);
  const std::size_t buffer_bytes =
      header_bytes + path_offset + path_bytes + sizeof(wchar_t);
  std::vector<unsigned char> buffer(buffer_bytes, 0);
  auto* reparse = reinterpret_cast<LabReparseDataBuffer*>(buffer.data());
  reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  reparse->ReparseDataLength =
      static_cast<USHORT>(buffer_bytes - header_bytes);
  reparse->Reserved = 0;
  LabMountPointReparseData* mount = &reparse->Reparse.MountPointReparseBuffer;
  mount->SubstituteNameOffset = 0;
  // Lengths exclude the terminating null; the print name starts after that null.
  mount->SubstituteNameLength =
      static_cast<USHORT>(path_bytes - sizeof(wchar_t));
  mount->PrintNameOffset = static_cast<USHORT>(path_bytes);
  mount->PrintNameLength = 0;
  std::memcpy(mount->PathBuffer, substitute.c_str(),
              path_bytes + sizeof(wchar_t));

  HANDLE handle = ::CreateFileW(
      link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *error = "cannot open the junction stub (" +
             std::to_string(::GetLastError()) + ")";
    return false;
  }
  DWORD returned = 0;
  const BOOL ok = ::DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, reparse,
      static_cast<DWORD>(buffer_bytes), nullptr, 0, &returned, nullptr);
  const DWORD code = ok != FALSE ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(handle);
  if (ok == FALSE) {
    *error = "cannot set the junction reparse point (" + std::to_string(code) +
             ")";
    return false;
  }
  return true;
}

bool RunRunRootSafety(const LabConfig& config) {
  const fs::path trusted = config.trusted_run_root;
  std::printf("[lab-win] run-root safety: trusted root %s\n",
              NarrowPath(trusted).c_str());
  Check(!trusted.empty() && trusted.is_absolute(),
        "run-root: the build supplies an absolute trusted root",
        NarrowPath(trusted));
  if (trusted.empty() || !trusted.is_absolute()) {
    return false;
  }

  const std::string base_stem = FreshLabRunStem();
  const fs::path area = trusted / ("selftest-" + base_stem);
  const std::string area_text = NarrowPath(area);
  std::error_code area_error;
  fs::create_directories(area, area_error);
  Check(!area_error, "run-root: the isolated test area is available",
        area_error.message());
  if (area_error) {
    return false;
  }
  std::printf("[lab-win] evidence isolated area %s (retained)\n",
              area_text.c_str());

  {
    const fs::path parent = area / "fresh" / "nested";
    const std::string stem = base_stem + "-fresh";
    LabRunDirectories first;
    std::string error;
    const bool first_ok =
        PrepareFreshLabRunRoot(trusted, parent, stem, 4, &first, &error);
    Check(first_ok, "run-root: a missing nested parent is created", error);
    if (first_ok) {
      Check(first.run_dir.filename().string() == stem,
            "run-root: the first run directory uses the fresh unique name",
            first.run_dir.filename().string());
      Check(Equivalent(first.run_dir.parent_path(), parent),
            "run-root: the run directory is a direct child of the requested "
            "parent");
      Check(fs::is_directory(first.shared_dir) &&
                fs::is_directory(first.user_dir) &&
                fs::is_directory(first.log_dir),
            "run-root: shared, user and log are created inside the new run");
      const fs::path sentinel = first.log_dir / "sentinel-a.txt";
      std::string hash;
      Check(SeedSentinel(sentinel, "fresh-run-a-kept", &hash),
            "run-root: first-run sentinel written");
      LabRunDirectories second;
      error.clear();
      const bool second_ok =
          PrepareFreshLabRunRoot(trusted, parent, stem, 4, &second, &error);
      Check(second_ok, "run-root: a second run gets its own directory", error);
      if (second_ok) {
        Check(!Equivalent(second.run_dir, first.run_dir),
              "run-root: the second run did not reuse the first directory");
        Check(second.run_dir.filename().string() == stem + "-1",
              "run-root: the second run uses the next free name",
              second.run_dir.filename().string());
        CheckSentinel(sentinel, "fresh-run-a-kept", hash,
                      "run-root: first run log survives the second run");
      }
    }
  }

  {
    const fs::path parent = area / "collision";
    const std::string stem = base_stem + "-collision";
    const fs::path zero = parent / stem;
    const fs::path one = parent / (stem + "-1");
    std::string hash_zero;
    std::string hash_one;
    Check(SeedSentinel(zero / "keep.txt", "legacy-zero", &hash_zero) &&
              SeedSentinel(one / "keep.txt", "legacy-one", &hash_one),
          "run-root: collision sentinels written");
    LabRunDirectories run;
    std::string error;
    const bool ok =
        PrepareFreshLabRunRoot(trusted, parent, stem, 4, &run, &error);
    Check(ok, "run-root: existing candidates are skipped, not deleted", error);
    if (ok) {
      Check(run.run_dir.filename().string() == stem + "-2",
            "run-root: the first free collision suffix is used",
            run.run_dir.filename().string());
      CheckSentinel(zero / "keep.txt", "legacy-zero", hash_zero,
                    "run-root: existing stem directory");
      CheckSentinel(one / "keep.txt", "legacy-one", hash_one,
                    "run-root: existing -1 directory");
    }
  }

  {
    const fs::path parent = area / "exhausted";
    const std::string stem = base_stem + "-exhausted";
    std::string hash_zero;
    std::string hash_one;
    std::string hash_two;
    Check(SeedSentinel(parent / stem / "keep.txt", "exhausted-0",
                       &hash_zero) &&
              SeedSentinel(parent / (stem + "-1") / "keep.txt", "exhausted-1",
                           &hash_one) &&
              SeedSentinel(parent / (stem + "-2") / "keep.txt", "exhausted-2",
                           &hash_two),
          "run-root: exhaustion sentinels written");
    const std::size_t before = CountEntries(parent);
    LabRunDirectories run;
    std::string error;
    const bool ok =
        PrepareFreshLabRunRoot(trusted, parent, stem, 3, &run, &error);
    Check(!ok, "run-root: the bounded attempt budget is enforced");
    Check(error.find("all 3 run directory names") != std::string::npos,
          "run-root: the refusal names the attempt budget", error);
    Check(CountEntries(parent) == before,
          "run-root: a refused preparation creates no directory",
          std::to_string(CountEntries(parent)) + " vs " +
              std::to_string(before));
    CheckSentinel(parent / stem / "keep.txt", "exhausted-0", hash_zero,
                  "run-root: exhausted stem directory");
    CheckSentinel(parent / (stem + "-1") / "keep.txt", "exhausted-1", hash_one,
                  "run-root: exhausted -1 directory");
    CheckSentinel(parent / (stem + "-2") / "keep.txt", "exhausted-2", hash_two,
                  "run-root: exhausted -2 directory");
  }

  {
    const fs::path legacy_base = area / "legacy" / "base";
    const fs::path old_run = legacy_base / "engine-executor-run";
    const fs::path old_log = old_run / "shared" / "old.log";
    std::string old_hash;
    Check(SeedSentinel(old_log, "old-log-kept", &old_hash),
          "run-root: legacy run sentinel written");
    LabRunDirectories run;
    std::string error;
    const bool ok =
        PrepareFreshLabRunRoot(trusted, legacy_base, "engine-executor-run", 4,
                               &run, &error);
    Check(ok,
          "run-root: a legacy engine-executor-run is not deletion authority",
          error);
    if (ok) {
      Check(run.run_dir.filename().string() == "engine-executor-run-1",
            "run-root: a sibling name is created next to the legacy directory",
            run.run_dir.filename().string());
      CheckSentinel(old_log, "old-log-kept", old_hash,
                    "run-root: legacy run log");
    }
    LabRunDirectories nested;
    error.clear();
    const bool nested_ok = PrepareFreshLabRunRoot(
        trusted, old_run, "engine-executor-run", 4, &nested, &error);
    Check(nested_ok,
          "run-root: a parent whose leaf is engine-executor-run is usable",
          error);
    if (nested_ok) {
      Check(Equivalent(nested.run_dir.parent_path(), old_run),
            "run-root: the nested run is inside the legacy directory, not a "
            "replacement");
      CheckSentinel(old_log, "old-log-kept", old_hash,
                    "run-root: legacy run log after nested preparation");
    }
  }

  {
    const fs::path outside =
        trusted.parent_path() / ("selftest-outside-" + base_stem);
    std::string outside_hash;
    Check(SeedSentinel(outside / "keep.txt", "outside-kept", &outside_hash),
          "run-root: outside sentinel written");
    LabRunDirectories run;
    std::string error;
    const bool ok = PrepareFreshLabRunRoot(
        trusted, outside, base_stem + "-outside", 4, &run, &error);
    Check(!ok,
          "run-root: an absolute parent outside the trusted root is refused");
    Check(!error.empty(), "run-root: the outside refusal is explained", error);
    CheckSentinel(outside / "keep.txt", "outside-kept", outside_hash,
                  "run-root: outside sentinel");

    error.clear();
    const bool dotted_ok = PrepareFreshLabRunRoot(
        trusted, trusted / ".." / outside.filename(), base_stem + "-dotted", 4,
        &run, &error);
    Check(!dotted_ok, "run-root: a .. path outside the trusted root is refused");
    CheckSentinel(outside / "keep.txt", "outside-kept", outside_hash,
                  "run-root: outside sentinel after the .. attempt");

    const fs::path sibling(trusted.native() + L"-sibling-" +
                           std::to_wstring(::GetCurrentProcessId()));
    std::string sibling_hash;
    Check(SeedSentinel(sibling / "keep.txt", "sibling-kept", &sibling_hash),
          "run-root: prefix-sibling sentinel written");
    error.clear();
    const bool sibling_ok = PrepareFreshLabRunRoot(
        trusted, sibling, base_stem + "-sibling", 4, &run, &error);
    Check(!sibling_ok,
          "run-root: a prefix-similar sibling of the trusted root is refused");
    CheckSentinel(sibling / "keep.txt", "sibling-kept", sibling_hash,
                  "run-root: prefix-sibling sentinel");

    error.clear();
    const bool relative_ok = PrepareFreshLabRunRoot(
        trusted, fs::path("relative-run-parent"), base_stem + "-relative", 4,
        &run, &error);
    Check(!relative_ok, "run-root: a relative parent is refused");
    Check(!error.empty(), "run-root: the relative refusal is explained", error);
  }

  {
    const fs::path target =
        trusted.parent_path() / ("selftest-junction-target-" + base_stem);
    std::string target_hash;
    Check(SeedSentinel(target / "keep.txt", "junction-target-kept",
                       &target_hash),
          "run-root: junction target sentinel written");
    const fs::path link = area / "junction-escape";
    std::string junction_error;
    if (!CreateDirectoryJunction(link, target, &junction_error)) {
      std::printf("[lab-win] SKIP run-root: junction fixture unavailable (%s)\n",
                  junction_error.c_str());
    } else {
      LabRunDirectories run;
      std::string error;
      const bool ok = PrepareFreshLabRunRoot(
          trusted, link, base_stem + "-junction", 4, &run, &error);
      Check(!ok,
            "run-root: a junction that resolves outside the trusted root is "
            "refused");
      Check(!error.empty(), "run-root: the junction refusal is explained",
            error);
    }
    CheckSentinel(target / "keep.txt", "junction-target-kept", target_hash,
                  "run-root: junction target sentinel");
  }

  {
    const fs::path normalize = area / "normalize";
    std::error_code ec;
    fs::create_directories(normalize / "a", ec);
    Check(!ec, "run-root: normalization fixture created", ec.message());
    LabRunDirectories run;
    std::string error;
    const bool ok = PrepareFreshLabRunRoot(
        trusted, normalize / "a" / "." / "b" / ".." / "b",
        base_stem + "-normalized", 4, &run, &error);
    Check(ok, "run-root: . and .. components are normalized before validation",
          error);
    if (ok) {
      Check(Equivalent(run.run_dir.parent_path(), normalize / "a" / "b"),
            "run-root: the run directory is under the normalized parent",
            NarrowPath(run.run_dir.parent_path()));
    }
  }

  std::printf("[lab-win] evidence run-root area retained without cleanup: %s\n",
              area_text.c_str());
  return g_failed == 0;
}

}  // namespace

int RunLabSelfTest(const LabConfig& config, const std::string& case_name) {
  g_total = 0;
  g_failed = 0;
  std::printf("[lab-win] self-test case=%s\n", case_name.c_str());
  bool ok = true;
  if (case_name == "window-lifecycle" || case_name == "all") {
    ok = RunWindowLifecycle() && ok;
  }
  if (case_name == "scenarios" || case_name == "all") {
    ok = RunScenarios(config) && ok;
  }
  if (case_name == "run-root" || case_name == "all") {
    ok = RunRunRootSafety(config) && ok;
  }
  if (case_name != "window-lifecycle" && case_name != "scenarios" &&
      case_name != "run-root" && case_name != "all") {
    std::printf("[lab-win] unknown case: %s\n", case_name.c_str());
    return 2;
  }
  std::printf("[lab-win] RESULT %s %d checks, %d failed\n",
              g_failed == 0 ? "PASS" : "FAIL", g_total, g_failed);
  if (g_failed == 0 && ok) {
    std::printf("RESULT PASS tsf_lab %d checks\n", g_total);
    return 0;
  }
  return 5;
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
