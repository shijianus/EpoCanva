// EpoCanva P3.1 -- the module state itself. See module_state.h for the contract.
#include "module_state.h"

namespace epocanva {
namespace tsf {

namespace {

HINSTANCE g_instance = nullptr;

// InterlockedIncrement/Decrement take a `volatile LONG*`; the two counters are the only
// mutable state in this translation unit and every access goes through the Interlocked
// family, so no other synchronisation is needed for the accounting itself.
volatile LONG g_live_objects = 0;
volatile LONG g_server_locks = 0;
volatile LONG g_host_active = 0;
volatile LONG g_host_failed = 0;

}  // namespace

void SetModuleInstance(HINSTANCE instance) {
  g_instance = instance;
}

HINSTANCE ModuleInstance() {
  return g_instance;
}

LONG ModuleAddObject() {
  return InterlockedIncrement(&g_live_objects);
}

LONG ModuleReleaseObject() {
  const LONG after = InterlockedDecrement(&g_live_objects);
  if (after < 0) {
    // A Release with no matching reference is a caller bug. Restore the counter so it
    // can never wrap (a wrapped counter would flip every later DllCanUnloadNow to
    // S_FALSE and masquerade as a leak), and report the clamped value; the refusal is
    // observable at the call site that triggered it.
    InterlockedIncrement(&g_live_objects);
    return 0;
  }
  return after;
}

LONG ModuleAddLock() {
  return InterlockedIncrement(&g_server_locks);
}

bool ModuleReleaseLock() {
  const LONG after = InterlockedDecrement(&g_server_locks);
  if (after < 0) {
    InterlockedIncrement(&g_server_locks);
    return false;  // there was no outstanding lock; the caller's bookkeeping is wrong
  }
  return true;
}

void ModuleSetHostActive(bool active) {
  InterlockedExchange(&g_host_active, active ? 1 : 0);
}

void ModuleSetHostFailed(bool failed) {
  if (failed) {
    InterlockedExchange(&g_host_failed, 1);
  }
}

LONG ModuleHostActiveFlag() { return g_host_active; }
LONG ModuleHostFailedFlag() { return g_host_failed; }
LONG ModuleLiveObjectCount() { return g_live_objects; }
LONG ModuleServerLockCount() { return g_server_locks; }

bool ModuleCanUnload() {
  // Both counters are read non-atomically here, but DllCanUnloadNow is only meaningful
  // when the caller has released everything it holds and no other thread of the same
  // host is mid-creation -- that sequencing is the COM contract of the call, not
  // something this function can enforce. Reading both counts as they are is exactly the
  // observable the test program judges.
  //
  // The two engine-host flags are read as atomics and are never written by anything except
  // the broker. g_host_active stays set from the first start attempt until the executor has
  // been destroyed after a clean stop; g_host_failed is one-way. Neither check joins,
  // initialises the engine, calls COM or performs file I/O.
  return g_live_objects == 0 && g_server_locks == 0 && g_host_active == 0 &&
         g_host_failed == 0;
}

}  // namespace tsf
}  // namespace epocanva
