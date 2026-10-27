// EpoCanva P3.6 -- the text service object: ITfTextInputProcessorEx plus one engine lease.
//
// WHAT THIS OBJECT DOES. It validates Activate/ActivateEx parameters, keeps a reference to
// the activating ITfThreadMgr, remembers the client id and flags, and -- in the hosted build
// -- holds one EngineLease from the process-wide EngineHostBroker. Activation is
// transactional: the new thread-manager reference is taken first, the engine lease is
// acquired second, and only when both succeeded are the old state, old reference and old
// lease swapped out. A failed attempt leaves the previous activation untouched.
//
// REACTIVATION. Activating an already-active service acquires a new lease BEFORE dropping
// the old one and AddRefs the new thread manager BEFORE releasing the old reference. The
// same ITfThreadMgr pointer may therefore be passed again: its reference is incremented
// first, so releasing the old reference can never leave a dangling pointer. The net lease
// count and thread-manager reference count are unchanged after a successful reactivation.
// Deactivating an inactive service remains a successful no-op.
//
// DEACTIVATE AND THE LAST LEASE. Deactivate moves the thread-manager reference and lease out
// under the object's mutex, then releases them outside it. Releasing the last process lease
// stops and joins the executor on this explicit caller thread. Deactivate is never called
// from DllMain.
//
// ABNORMAL DESTRUCTION. If a host releases the last COM reference without Deactivate, the
// destructor must not wait for the executor worker: that Release may be running under the
// loader lock. It releases the thread-manager reference and calls EngineLease::Abandon(),
// which gives up the lease without stopping or joining. The broker consequently stays
// Failed for the rest of the process, DllCanUnloadNow stays S_FALSE, and the worker is ended
// only by process exit. This is a defensive abnormal path, never the normal lifecycle; the
// loader-lock behaviour of the ThreadMgr Release itself remains unverified and is recorded
// in docs/windows/tsf-engine-lifetime-contract.md.
//
// WHAT THIS OBJECT DOES NOT DO, AND MUST NOT BE READ AS DOING. There is no key sink, no
// keystroke handling, no edit session, no candidate window, no composition, no call into
// librime from a TSF thread, no IPC. Starting a Runtime and stopping it are the only engine
// actions here; no client, controller or host text is created or modified. ItfThreadMgr and
// ITfContext are never passed to the executor worker.
//
// THREADING. Reference counts use the Interlocked family. The activation fields are guarded
// by state_mutex_ because Activate/Deactivate may be called from different host threads; no
// COM or broker call is made while that mutex is held. The broker has its own mutex and
// never calls back into a text service.
#ifndef EPOCANVA_TSF_TEXT_SERVICE_H_
#define EPOCANVA_TSF_TEXT_SERVICE_H_

#include <msctf.h>

#include <memory>
#include <mutex>

#ifdef EPOCANVA_TSF_ENGINE_HOST
#include "engine_host.h"
#endif

namespace epocanva {
namespace tsf {

#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
namespace product {
class ProductTextController;
}
#endif

class EpoCanvaTextService
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
    : public ITfTextInputProcessorEx,
      public ITfKeyEventSink
#else
    : public ITfTextInputProcessorEx
#endif
{
 public:
  EpoCanvaTextService();

  EpoCanvaTextService(const EpoCanvaTextService&) = delete;
  EpoCanvaTextService& operator=(const EpoCanvaTextService&) = delete;

  // IUnknown -- supports IID_IUnknown, IID_ITfTextInputProcessor and
  // IID_ITfTextInputProcessorEx; everything else is E_NOINTERFACE with a null out-value.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // ITfTextInputProcessor
  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* thread_manager,
                                     TfClientId client_id) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;

  // ITfTextInputProcessorEx
  HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* thread_manager,
                                       TfClientId client_id, DWORD flags) override;

#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  // ITfKeyEventSink. Test callbacks predict only; key-down reports eaten only when the
  // controller really accepted the key into its bounded admission; key-up is never eaten.
  HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM wparam,
                                          LPARAM lparam, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM wparam,
                                        LPARAM lparam, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM wparam,
                                      LPARAM lparam, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM wparam,
                                    LPARAM lparam, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID guid,
                                           BOOL* eaten) override;
#endif

 private:
  ~EpoCanvaTextService();  // Release()-only lifecycle; never joins an engine worker

  // The one activation path both Activate and ActivateEx run.
  HRESULT ActivateInternal(ITfThreadMgr* thread_manager, TfClientId client_id,
                           DWORD flags);

#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  HRESULT AdviseKeySink(ITfThreadMgr* thread_manager, TfClientId client_id);
  void UnadviseKeySink(ITfThreadMgr* thread_manager, TfClientId client_id);
#endif

  std::mutex state_mutex_;  // guards every field below; held during no COM/broker call
  volatile LONG reference_count_;
  ITfThreadMgr* thread_manager_;  // AddRef'd while active, released by Deactivate
  TfClientId client_id_;
  DWORD activation_flags_;
  bool active_;
#ifdef EPOCANVA_TSF_ENGINE_HOST
  EngineLease engine_lease_;  // one process-broker lease per activation
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  // Shared lock on <user>/engine/engine.lock, held exactly as long as the lease. It keeps
  // another process from deleting the compiled schema while this engine is running.
  void* engine_lease_lock_ = nullptr;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  // Serializes sink callbacks against the controller swap/teardown. TSF normally calls
  // both on one STA; this mutex also makes a cross-thread Deactivate safe.
  std::mutex controller_mutex_;
  std::unique_ptr<product::ProductTextController> controller_;
  bool key_sink_advised_ = false;
#endif
};

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_TEXT_SERVICE_H_
