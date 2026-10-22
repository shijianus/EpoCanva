// EpoCanva P3.1 -- the module-wide accounting behind DllCanUnloadNow.
//
// WHAT THIS KEEPS. Three facts and nothing else:
//   1. the HINSTANCE DllMain recorded (the only thing DllMain is allowed to do here),
//   2. how many COM objects this DLL has alive (the class factory counts; every text
//      service instance counts),
//   3. how many LockServer(TRUE) locks are outstanding.
// DllCanUnloadNow answers S_OK exactly when (2) and (3) are both zero -- the standard
// in-proc server contract (the "server lock" of the COM local-server rules, applied to
// an unregistered DLL that is only ever loaded by LoadLibrary).
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

// DllCanUnloadNow's whole decision: S_OK only when nothing is alive and nothing is locked.
bool ModuleCanUnload();

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_MODULE_STATE_H_
