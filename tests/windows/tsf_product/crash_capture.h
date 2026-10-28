// EpoCanva W2-B-R -- test-only crash evidence capture.
//
// WHAT IT IS. The controlled product tests run in their own process with no installed
// debugger on this machine. This module installs a vectored exception handler and an
// unhandled-exception filter that record, before the process dies:
//
//   * the exception code, the faulting instruction address, and for access violations the
//     read/write/execute kind and the accessed address;
//   * the faulting module and its version, and the RVA inside that module;
//   * a stack walk of the faulting thread with local PDB symbols where they exist and
//     module+offset everywhere else (Windows PDBs are not downloaded);
//   * the state of every thread in the process;
//   * the test's current phase label and the product's text-free counters;
//   * a minidump for later analysis with a real debugger.
//
// WHAT IT IS NOT. It is not part of the product DLL, it never leaves the development test
// directory, and it does not change how the exception is handled (the process still dies
// with the same unhandled exception after the evidence is written).
#ifndef EPOCANVA_TESTS_TSF_PRODUCT_CRASH_CAPTURE_H_
#define EPOCANVA_TESTS_TSF_PRODUCT_CRASH_CAPTURE_H_

#include <filesystem>
#include <string>

namespace epocanva {
namespace tsf {
namespace product {
namespace test {

// Writes evidence below `directory` (created if missing). `symbol_search_path` is passed to
// DbgHelp; it may be empty (module+offset evidence is still produced).
void InstallCrashCapture(const std::filesystem::path& directory,
                         const std::wstring& symbol_search_path);

// A short, text-free label describing the step that is running now. The handler copies it.
void SetCrashPhase(const std::string& phase);

// Writes a minidump of the live process (all threads) and a text marker, for a watchdog
// that must diagnose a hang rather than a crash. Returns the dump path (empty on failure).
std::filesystem::path WriteHangDump(const char* reason, const std::string& details);

// The optional counter callback runs inside the handler, before the dump is written. It must
// not block on anything the faulting thread might hold; the controlled seam only takes its
// own test mutex, which no product path holds. The returned pointer must stay valid (a
// static buffer in the test), and the text must not contain user input.
using CrashCounterCallback = const char* (*)();
void SetCrashCounterCallback(CrashCounterCallback callback);

}  // namespace test
}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_TSF_PRODUCT_CRASH_CAPTURE_H_
