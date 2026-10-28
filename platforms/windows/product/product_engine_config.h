// EpoCanva W2 -- product wiring between the TSF DLL and the engine host.
//
// TWO STEPS, NOT ONE. ApplyProductEngineConfig() runs in DllGetClassObject (never DllMain)
// and only resolves the product/user paths and installs a base engine configuration.
// EngineStartTransaction::Prepare() runs on every product activation, before the broker is
// asked for a lease, and is the only place a deployment request can be created. It is an
// explicit initialization/configuration-change step, never a key-path operation.
//
// DEPLOYMENT IDENTITY. A deployment is requested only when the compiled user schema does
// not match the current input snapshot. The snapshot is a SHA-256 over the settings token,
// the schema/default/custom files, the product lexicon and rime.dll. A successful start
// records the snapshot in <user>/engine/deployed-state; the settings marker is removed
// only after that record was written. A failed deployment therefore always leaves the
// marker (the retry evidence) in place.
//
// CROSS-PROCESS SERIALIZATION. Every activation holds a named mutex derived from the
// per-user engine directory for the whole start/deploy window. Two host processes that
// share one user directory therefore cannot compile into the same output concurrently.
// The wait is bounded; a timeout fails this activation with a retryable error instead of
// deploying without coordination.
//
// WHAT IS NOT HERE. No key-path calls, no network I/O, no text logging. A running engine
// is never redeployed underneath itself: a settings change while another activation holds
// the engine is recorded as "deferred until restart" and its marker survives.
#ifndef EPOCANVA_PRODUCT_ENGINE_CONFIG_H_
#define EPOCANVA_PRODUCT_ENGINE_CONFIG_H_

#include <windows.h>

#include <string>

#include "engine_host.h"

namespace epocanva {
namespace product {

// DllGetClassObject step. Resolves the module/user paths, creates the user directories and
// installs a base EngineHostConfig without a deployment request. Returns false (with a
// description) when the product or user layout cannot be resolved. Callable repeatedly.
bool ApplyProductEngineConfig(HMODULE module, std::wstring* why);

// One activation's deployment transaction. Prepare() acquires the cross-process start
// lock; Complete()/Abort() release it. The destructor aborts an unfinished transaction, so
// an exception path cannot leak the mutex. All methods run on the activating STA.
class EngineStartTransaction {
 public:
  EngineStartTransaction();
  ~EngineStartTransaction();

  EngineStartTransaction(const EngineStartTransaction&) = delete;
  EngineStartTransaction& operator=(const EngineStartTransaction&) = delete;

  // Re-reads the settings marker and the input snapshot, installs the EngineHostConfig
  // this activation must use, and holds the per-user start lock until Complete/Abort.
  bool Prepare(std::wstring* why);

  // Called after EngineHostBroker::Acquire succeeded. Verifies the compiled artefact when
  // a deployment was requested, records the deployed snapshot and retires the marker.
  // Returns false only when a requested deployment produced no usable artefact; the caller
  // must release the engine lease and fail activation. A state-record write failure keeps
  // the marker and still returns true (the engine is usable, the retry evidence survives).
  bool Complete(std::wstring* why);

  // Called after Acquire failed or a later activation step failed. Never writes or removes
  // anything; the marker remains the retry evidence.
  void Abort();

  // Transfers the shared engine.lock handle acquired for this activation. The caller must
  // hold it exactly as long as the engine lease: closing it while the engine runs would
  // let another process delete the compiled output underneath it. Returns null after a
  // failed Complete or a previous transfer.
  void* TakeLeaseLock();

  bool prepared() const { return prepared_; }
  bool deployment_requested() const { return deployment_requested_; }
  // True when a settings/data change could not be applied because another activation is
  // already running the engine. The marker is kept and the change applies after restart.
  bool deferred_while_running() const { return deferred_while_running_; }
  // The exact configuration this transaction installed for the broker. Callers on another
  // thread use this copy instead of re-reading the process-wide value, which a second
  // activation could replace between Prepare and Acquire.
  bool config(epocanva::tsf::EngineHostConfig* out) const;
  const std::string& fingerprint() const { return fingerprint_; }

 private:
  bool prepared_ = false;
  bool deployment_requested_ = false;
  bool deferred_while_running_ = false;
  bool exclusive_ = false;
  std::string token_;
  std::string fingerprint_;
  epocanva::tsf::EngineHostConfig config_;
  void* mutex_ = nullptr;      // named start mutex HANDLE
  void* lock_file_ = nullptr;  // engine.lock HANDLE (shared or exclusive)
};

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_ENGINE_CONFIG_H_
