// EpoCanva P3.1 -- the module-wide accounting behind DllCanUnloadNow.
//
// WHAT THIS KEEPS. Five facts:
//   1. the HINSTANCE DllMain recorded (the only thing DllMain is allowed to do here),
//   2. how many COM objects this DLL has alive (the class factory counts; every text
//      service instance counts),
//   3. how many LockServer(TRUE) locks are outstanding,
//   4. whether an engine start/run/stop/cleanup window is active (P3.6),
//   5. whether that window ended in an unresolved failure (P3.6, one-way latch).
// DllCanUnloadNow answers S_OK exactly when (2), (3) and (4) are zero and (5) is clear --
// the standard in-proc server contract plus the engine lifetime guard.
//
// WHY THE ENGINE FLAGS ARE ATOMIC AND SEPARATE. DllCanUnloadNow runs on whatever thread COM
// chooses and must not take the broker mutex, join a worker, initialise anything or touch
// the engine. The broker updates these flags around the whole start/stop/cleanup window;
// the unload decision is a bounded read of four counters.
//
// THREADING. The counters are touched by InterlockedIncrement/InterlockedDecrement so
// the accounting holds whichever host thread creates, uses or releases the objects.
// The per-object reference counts in class_factory.cc / text_service.cc are the same:
// an in-proc TSF DLL is loaded into a host process that we do not control, so nothing
// here may assume single-threaded callers.
//
// UNDERFLOW. A Release or LockServer(FALSE) without a matching acquire is a caller bug;
// this module refuses to let the counters wrap to negative (which would silently flip
// every later DllCanUnloadNow answer to S_FALSE and look like a leak). The release
// functions restore the counter and report the refusal, so the bug stays observable at
// the call site instead of corrupting the module's arithmetic.
//
// NO LOGGING (AGENTS.md section 2): this file touches counters and an instance handle.
// Nothing here reads or stores user content, so there is nothing to redact.
#ifndef EPOCANVA_TSF_MODULE_STATE_H_
#define EPOCANVA_TSF_MODULE_STATE_H_

#include <windows.h>

namespace epocanva {
namespace tsf {

// The only state DllMain records: the instance handle on DLL_PROCESS_ATTACH, cleared on
// DLL_PROCESS_DETACH. No COM initialisation, no thread creation, no file or registry
// access, no logging setup -- the P3.1 brief forbids all of that inside DllMain and the
// static CRT needs nothing beyond what _DllMainCRTStartup already does for us.
void SetModuleInstance(HINSTANCE instance);
HINSTANCE ModuleInstance();

// Live-object accounting. Every COM object constructed inside this DLL calls Add on
// construction and Release on destruction. Returns the new count.
LONG ModuleAddObject();
LONG ModuleReleaseObject();  // refuses to go below zero: returns 0 without decrementing

// Server-lock accounting (IClassFactory::LockServer). Returns the new count.
LONG ModuleAddLock();
// Returns true when the lock was released, false when there was no lock to release
// (the caller-bug case above; the counter is left untouched so it cannot underflow).
bool ModuleReleaseLock();

// Engine-host accounting (P3.6). g_host_active covers the entire engine start, run, stop and
// executor-destruction window; g_host_failed is a terminal "cleanup did not end safely"
// latch. Both are plain atomics so DllCanUnloadNow can read them without a lock, allocating,
// initialising anything or touching the engine. The broker is the only writer.
void ModuleSetHostActive(bool active);
void ModuleSetHostFailed(bool failed);  // one-way latch; never cleared in a process lifetime
LONG ModuleHostActiveFlag();
LONG ModuleHostFailedFlag();

// Read-only counters for the private test seam. No caller in the product path uses these.
LONG ModuleLiveObjectCount();
LONG ModuleServerLockCount();

// DllCanUnloadNow's whole decision: S_OK only when nothing is alive, nothing is locked and
// no engine start/stop/cleanup window or unresolved failure is outstanding.
bool ModuleCanUnload();

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_MODULE_STATE_H_
