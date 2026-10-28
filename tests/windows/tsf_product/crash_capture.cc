// EpoCanva W2-B-R -- test-only crash evidence capture. See crash_capture.h.
#include "crash_capture.h"

#include <windows.h>
// clang-format off
#include <dbghelp.h>
#include <tlhelp32.h>
// clang-format on

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace epocanva {
namespace tsf {
namespace product {
namespace test {
namespace {

constexpr DWORD kMaxFirstChanceReports = 8;
constexpr int kMaxFrames = 96;

std::filesystem::path g_directory;
std::wstring g_symbol_search_path;
std::string g_phase = "startup";
std::mutex g_phase_mutex;
PVOID g_vectored_handle = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
CrashCounterCallback g_counter_callback = nullptr;
LONG g_first_chance_count = 0;
thread_local bool t_in_handler = false;

std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
}

std::string WideToNarrow(const std::wstring& text) { return Narrow(text); }

std::string CurrentPhase() {
  std::lock_guard<std::mutex> guard(g_phase_mutex);
  return g_phase;
}

std::string Format(const char* format, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000374u:
      return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409u:
      return "STATUS_STACK_BUFFER_OVERRUN";
    default:
      return "exception";
  }
}

struct ModuleRecord {
  std::wstring name;
  std::wstring path;
  std::uintptr_t base = 0;
  std::size_t size = 0;
};

std::vector<ModuleRecord> EnumerateModules() {
  std::vector<ModuleRecord> modules;
  const HANDLE snapshot = ::CreateToolhelp32Snapshot(
      TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ::GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) {
    return modules;
  }
  MODULEENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (::Module32FirstW(snapshot, &entry)) {
    do {
      ModuleRecord record;
      record.name = entry.szModule;
      record.path = entry.szExePath;
      record.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
      record.size = entry.modBaseSize;
      modules.push_back(std::move(record));
      entry.dwSize = sizeof(entry);
    } while (::Module32NextW(snapshot, &entry));
  }
  ::CloseHandle(snapshot);
  return modules;
}

const ModuleRecord* FindModule(const std::vector<ModuleRecord>& modules,
                               std::uintptr_t address) {
  for (const ModuleRecord& module : modules) {
    if (address >= module.base && address < module.base + module.size) {
      return &module;
    }
  }
  return nullptr;
}

std::string ModuleVersion(const std::wstring& path) {
  DWORD handle = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (size == 0) {
    return "unavailable";
  }
  std::vector<unsigned char> buffer(size);
  if (!::GetFileVersionInfoW(path.c_str(), handle, size, buffer.data())) {
    return "unavailable";
  }
  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (!::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info),
                        &info_size) ||
      info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
    return "unavailable";
  }
  return Format("%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                LOWORD(info->dwFileVersionLS));
}

std::string DescribeAddress(const std::vector<ModuleRecord>& modules,
                            std::uintptr_t address, bool symbols, HANDLE process) {
  const ModuleRecord* module = FindModule(modules, address);
  const std::uintptr_t offset =
      module != nullptr ? address - module->base : address;
  const std::string where =
      module != nullptr ? Narrow(module->name) + "+0x" + Format("%llX", (unsigned long long)offset)
                        : "0x" + Format("%llX", (unsigned long long)address);
  if (!symbols || module == nullptr) {
    return where;
  }
  alignas(SYMBOL_INFOW) unsigned char storage[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
  SYMBOL_INFOW* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
  symbol->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (!::SymFromAddrW(process, static_cast<DWORD64>(address), &displacement, symbol)) {
    return where;
  }
  std::string text = Narrow(module->name) + "!" + Narrow(std::wstring(symbol->Name, symbol->NameLen));
  if (displacement != 0) {
    text += "+0x" + Format("%llX", static_cast<unsigned long long>(displacement));
  }
  IMAGEHLP_LINEW64 line = {};
  line.SizeOfStruct = sizeof(line);
  DWORD line_displacement = 0;
  if (::SymGetLineFromAddrW64(process, static_cast<DWORD64>(address), &line_displacement,
                              &line)) {
    text += " [" + Narrow(std::wstring(line.FileName)) + ":" +
            std::to_string(line.LineNumber) + "]";
  }
  return text;
}

void WriteThreads(FILE* out) {
  const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,
                                                     ::GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) {
    std::fprintf(out, "threads: snapshot failed err=%lu\n", ::GetLastError());
    return;
  }
  const DWORD process_id = ::GetCurrentProcessId();
  const DWORD current_thread = ::GetCurrentThreadId();
  std::fprintf(out, "threads (current=%lu):\n", current_thread);
  THREADENTRY32 entry = {};
  entry.dwSize = sizeof(entry);
  if (::Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != process_id) {
        continue;
      }
      std::string line = Format("  tid=%lu", entry.th32ThreadID);
      const HANDLE thread =
          ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
      if (thread != nullptr) {
        FILETIME creation = {};
        FILETIME exit = {};
        FILETIME kernel = {};
        FILETIME user = {};
        if (::GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
          ULARGE_INTEGER user_time;
          user_time.LowPart = user.dwLowDateTime;
          user_time.HighPart = user.dwHighDateTime;
          line += Format(" cpu_user_ms=%llu",
                         static_cast<unsigned long long>(user_time.QuadPart / 10000));
        }
        line += Format(" priority=%d", ::GetThreadPriority(thread));
        PWSTR description = nullptr;
        if (SUCCEEDED(::GetThreadDescription(thread, &description)) &&
            description != nullptr) {
          line += " name=" + Narrow(description);
          ::LocalFree(description);
        }
        ::CloseHandle(thread);
      } else {
        line += Format(" (OpenThread failed err=%lu)", ::GetLastError());
      }
      if (entry.th32ThreadID == current_thread) {
        line += "  <-- faulting thread";
      }
      std::fprintf(out, "%s\n", line.c_str());
      entry.dwSize = sizeof(entry);
    } while (::Thread32Next(snapshot, &entry));
  }
  ::CloseHandle(snapshot);
}

void WriteModules(FILE* out, const std::vector<ModuleRecord>& modules,
                  const ModuleRecord* faulting) {
  std::fprintf(out, "modules (%zu):\n", modules.size());
  for (const ModuleRecord& module : modules) {
    std::string line = "  0x" + Format("%llX", (unsigned long long)module.base) +
                       "  " + Narrow(module.name);
    if (&module == faulting || _wcsicmp(module.name.c_str(), L"textinputframework.dll") == 0 ||
        _wcsicmp(module.name.c_str(), L"msctf.dll") == 0 ||
        _wcsicmp(module.name.c_str(), L"rime.dll") == 0 ||
        _wcsnicmp(module.name.c_str(), L"epocanva", 8) == 0) {
      line += "  version=" + ModuleVersion(module.path);
    }
    std::fprintf(out, "%s\n", line.c_str());
  }
}

// Fast stack for first-chance reports: no DbgHelp, no allocation beyond the frame array.
void WriteFastStack(FILE* out, const std::vector<ModuleRecord>& modules) {
  void* frames[kMaxFrames] = {};
  const USHORT count = ::RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
  for (USHORT index = 0; index < count; ++index) {
    std::fprintf(out, "  #%02u %s\n", index,
                 DescribeAddress(modules, reinterpret_cast<std::uintptr_t>(frames[index]),
                                 false, ::GetCurrentProcess())
                     .c_str());
  }
}

void WriteStackFromContext(FILE* out, HANDLE thread, CONTEXT* context,
                           const std::vector<ModuleRecord>& modules,
                           bool manage_symbols) {
  const HANDLE process = ::GetCurrentProcess();
  if (manage_symbols) {
    if (!::SymInitializeW(process, g_symbol_search_path.c_str(), TRUE)) {
      std::fprintf(out, "symbols: SymInitializeW failed err=%lu; using module offsets\n",
                   ::GetLastError());
    } else {
      ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    }
  }
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context->Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context->Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context->Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  CONTEXT walk_context = *context;
  for (int index = 0; index < kMaxFrames; ++index) {
    if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walk_context,
                       nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    std::fprintf(
        out, "  #%02d %s\n", index,
        DescribeAddress(modules, static_cast<std::uintptr_t>(frame.AddrPC.Offset), true,
                        process)
            .c_str());
  }
  if (manage_symbols) {
    ::SymCleanup(process);
  }
}

void WriteSymbolStack(FILE* out, CONTEXT* context,
                      const std::vector<ModuleRecord>& modules) {
  WriteStackFromContext(out, ::GetCurrentThread(), context, modules, true);
}

// Hang diagnostics: suspend every other thread, walk its stack with local symbols, resume.
void WriteLiveThreadStacks(FILE* out, const std::vector<ModuleRecord>& modules) {
  const HANDLE process = ::GetCurrentProcess();
  const DWORD process_id = ::GetCurrentProcessId();
  const DWORD current_thread = ::GetCurrentThreadId();
  if (!::SymInitializeW(process, g_symbol_search_path.c_str(), TRUE)) {
    std::fprintf(out, "symbols: SymInitializeW failed err=%lu\n", ::GetLastError());
  } else {
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  }
  const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, process_id);
  if (snapshot == INVALID_HANDLE_VALUE) {
    std::fprintf(out, "thread stacks: snapshot failed err=%lu\n", ::GetLastError());
    ::SymCleanup(process);
    return;
  }
  THREADENTRY32 entry = {};
  entry.dwSize = sizeof(entry);
  if (::Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != process_id ||
          entry.th32ThreadID == current_thread) {
        continue;
      }
      const HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
      if (thread == nullptr) {
        std::fprintf(out, "thread %lu: OpenThread failed err=%lu\n", entry.th32ThreadID,
                     ::GetLastError());
        continue;
      }
      if (::SuspendThread(thread) == static_cast<DWORD>(-1)) {
        std::fprintf(out, "thread %lu: SuspendThread failed\n", entry.th32ThreadID);
        ::CloseHandle(thread);
        continue;
      }
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_FULL;
      if (::GetThreadContext(thread, &context)) {
        std::fprintf(out, "thread %lu stack (rip=%016llX):\n", entry.th32ThreadID,
                     static_cast<unsigned long long>(context.Rip));
        WriteStackFromContext(out, thread, &context, modules, false);
      } else {
        std::fprintf(out, "thread %lu: GetThreadContext failed err=%lu\n",
                     entry.th32ThreadID, ::GetLastError());
      }
      ::ResumeThread(thread);
      ::CloseHandle(thread);
      entry.dwSize = sizeof(entry);
    } while (::Thread32Next(snapshot, &entry));
  }
  ::CloseHandle(snapshot);
  ::SymCleanup(process);
}

void WriteCounters(FILE* out) {
  const CrashCounterCallback callback = g_counter_callback;
  if (callback == nullptr) {
    return;
  }
  const char* text = callback();
  if (text != nullptr) {
    std::fprintf(out, "product counters: %s\n", text);
  }
}

void WriteMiniDump(const std::filesystem::path& path, EXCEPTION_POINTERS* pointers) {
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  MINIDUMP_EXCEPTION_INFORMATION info = {};
  info.ThreadId = ::GetCurrentThreadId();
  info.ExceptionPointers = pointers;
  info.ClientPointers = FALSE;
  (void)::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                            static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
                                                       MiniDumpWithThreadInfo |
                                                       MiniDumpWithUnloadedModules),
                            pointers != nullptr ? &info : nullptr, nullptr, nullptr);
  ::CloseHandle(file);
}

std::string BuildStamp() {
  SYSTEMTIME time = {};
  ::GetLocalTime(&time);
  return Format("%04u%02u%02u-%02u%02u%02u-%03u", time.wYear, time.wMonth, time.wDay,
                time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}

std::filesystem::path EvidencePath(const std::string& stem, const std::string& stamp,
                                   const wchar_t* extension) {
  return g_directory / (std::wstring(stem.begin(), stem.end()) + L"-" +
                        std::wstring(stamp.begin(), stamp.end()) + extension);
}

void WriteExceptionSummary(FILE* out, const EXCEPTION_RECORD* record,
                           const ModuleRecord* faulting, const std::string& phase) {
  std::fprintf(out, "phase: %s\n", phase.c_str());
  std::fprintf(out, "exception_code: 0x%08lX (%s)\n", record->ExceptionCode,
               ExceptionName(record->ExceptionCode));
  std::fprintf(out, "exception_flags: 0x%08lX\n", record->ExceptionFlags);
  std::fprintf(out, "exception_address: 0x%llX\n",
               static_cast<unsigned long long>(
                   reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)));
  if (faulting != nullptr) {
    const std::uintptr_t rva =
        reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) - faulting->base;
    std::fprintf(out, "faulting_module: %s\n", Narrow(faulting->name).c_str());
    std::fprintf(out, "faulting_module_path: %s\n", Narrow(faulting->path).c_str());
    std::fprintf(out, "faulting_module_version: %s\n",
                 ModuleVersion(faulting->path).c_str());
    std::fprintf(out, "faulting_module_base: 0x%llX\n",
                 static_cast<unsigned long long>(faulting->base));
    std::fprintf(out, "faulting_rva: 0x%llX\n",
                 static_cast<unsigned long long>(rva));
  } else {
    std::fprintf(out, "faulting_module: unknown (address outside every loaded module)\n");
  }
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const ULONG_PTR kind = record->ExceptionInformation[0];
    const char* kind_text =
        kind == 0 ? "read" : (kind == 1 ? "write" : (kind == 8 ? "execute" : "other"));
    std::fprintf(out, "access: %s at 0x%llX\n", kind_text,
                 static_cast<unsigned long long>(record->ExceptionInformation[1]));
  }
  if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR &&
      record->NumberParameters >= 2) {
    std::fprintf(out, "in_page_fault: kind=%llu address=0x%llX status=0x%llX\n",
                 static_cast<unsigned long long>(record->ExceptionInformation[0]),
                 static_cast<unsigned long long>(record->ExceptionInformation[1]),
                 record->NumberParameters >= 3
                     ? static_cast<unsigned long long>(record->ExceptionInformation[2])
                     : 0ULL);
  }
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* pointers) {
  if (t_in_handler || pointers == nullptr || pointers->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  t_in_handler = true;
  const LONG index = ::InterlockedIncrement(&g_first_chance_count);
  if (index <= static_cast<LONG>(kMaxFirstChanceReports)) {
    std::error_code error;
    std::filesystem::create_directories(g_directory, error);
    const std::string stamp = BuildStamp();
    const std::filesystem::path report =
        EvidencePath("first-chance-" + std::to_string(index), stamp, L".txt");
    const std::vector<ModuleRecord> modules = EnumerateModules();
    const ModuleRecord* faulting = FindModule(
        modules, reinterpret_cast<std::uintptr_t>(
                     pointers->ExceptionRecord->ExceptionAddress));
    if (FILE* out = nullptr;
        ::_wfopen_s(&out, report.c_str(), L"w") == 0 && out != nullptr) {
      std::fprintf(out, "first-chance exception (the process may still handle it)\n");
      WriteExceptionSummary(out, pointers->ExceptionRecord, faulting, CurrentPhase());
      std::fprintf(out, "fast stack (no symbols):\n");
      WriteFastStack(out, modules);
      std::fclose(out);
    }
  }
  t_in_handler = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers) {
  if (t_in_handler || pointers == nullptr || pointers->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  t_in_handler = true;
  std::error_code error;
  std::filesystem::create_directories(g_directory, error);
  const std::string stamp = BuildStamp();
  const std::filesystem::path report = EvidencePath("crash", stamp, L".txt");
  const std::filesystem::path dump = EvidencePath("crash", stamp, L".dmp");
  const std::vector<ModuleRecord> modules = EnumerateModules();
  const ModuleRecord* faulting = FindModule(
      modules,
      reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress));
  WriteMiniDump(dump, pointers);
  if (FILE* out = nullptr;
      ::_wfopen_s(&out, report.c_str(), L"w") == 0 && out != nullptr) {
    std::fprintf(out, "unhandled exception evidence (EpoCanva controlled test)\n");
    WriteExceptionSummary(out, pointers->ExceptionRecord, faulting, CurrentPhase());
    std::fprintf(out, "thread_id: %lu\n", ::GetCurrentThreadId());
    std::fprintf(out, "process_id: %lu\n", ::GetCurrentProcessId());
    std::fprintf(out, "minidump: %s\n", Narrow(dump.wstring()).c_str());
    WriteCounters(out);
    std::fprintf(out, "\n");
    WriteThreads(out);
    std::fprintf(out, "\n");
    WriteModules(out, modules, faulting);
    std::fprintf(out, "\nstack (faulting thread, local symbols where available):\n");
    if (pointers->ContextRecord != nullptr) {
      WriteSymbolStack(out, pointers->ContextRecord, modules);
    } else {
      std::fprintf(out, "  context record is null\n");
    }
    std::fclose(out);
  }
  // Stderr plus stdout so the CTest log identifies the evidence files by name.
  std::fprintf(stderr, "CRASH EVIDENCE: %s\n", Narrow(report.wstring()).c_str());
  std::fprintf(stdout, "CRASH EVIDENCE: %s\n", Narrow(report.wstring()).c_str());
  std::fflush(stdout);
  t_in_handler = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashCapture(const std::filesystem::path& directory,
                         const std::wstring& symbol_search_path) {
  g_directory = directory;
  g_symbol_search_path = symbol_search_path;
  std::error_code error;
  std::filesystem::create_directories(g_directory, error);
  if (g_vectored_handle == nullptr) {
    g_vectored_handle = ::AddVectoredExceptionHandler(1, &VectoredHandler);
  }
  if (g_previous_filter == nullptr) {
    g_previous_filter = ::SetUnhandledExceptionFilter(&UnhandledFilter);
  }
}

void SetCrashPhase(const std::string& phase) {
  std::lock_guard<std::mutex> guard(g_phase_mutex);
  g_phase = phase;
}

std::filesystem::path WriteHangDump(const char* reason, const std::string& details) {
  std::error_code error;
  std::filesystem::create_directories(g_directory, error);
  const std::string stamp = BuildStamp();
  const std::filesystem::path dump = EvidencePath("hang", stamp, L".dmp");
  const std::filesystem::path report = EvidencePath("hang", stamp, L".txt");
  const HANDLE file = ::CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  const BOOL written = ::MiniDumpWriteDump(
      ::GetCurrentProcess(), ::GetCurrentProcessId(), file,
      static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                 MiniDumpWithUnloadedModules),
      nullptr, nullptr, nullptr);
  ::CloseHandle(file);
  if (FILE* out = nullptr;
      ::_wfopen_s(&out, report.c_str(), L"w") == 0 && out != nullptr) {
    std::fprintf(out, "hang evidence: %s\nphase: %s\n", reason, CurrentPhase().c_str());
    if (!details.empty()) {
      std::fprintf(out, "details: %s\n", details.c_str());
    }
    std::fprintf(out, "minidump: %s\n", Narrow(dump.wstring()).c_str());
    WriteCounters(out);
    std::fprintf(out, "\n");
    WriteThreads(out);
    std::fprintf(out, "\nthread stacks (suspended, local symbols where available):\n");
    WriteLiveThreadStacks(out, EnumerateModules());
    std::fclose(out);
  }
  std::fprintf(stdout, "HANG EVIDENCE: %s (dump written=%d)\n",
               Narrow(report.wstring()).c_str(), written ? 1 : 0);
  std::fflush(stdout);
  return dump;
}

void SetCrashCounterCallback(CrashCounterCallback callback) {
  g_counter_callback = callback;
}

}  // namespace test
}  // namespace product
}  // namespace tsf
}  // namespace epocanva
