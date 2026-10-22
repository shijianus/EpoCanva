// EpoCanva P3.1 -- the text service object: ITfTextInputProcessorEx and nothing else.
//
// WHAT THIS OBJECT DOES. Exactly what the P3.1 brief scopes: parameter validation, the
// thread-manager reference, the client id, the activated flag, and the matching cleanup.
// Activate and ActivateEx share ONE internal path (ActivateInternal) -- Activate is
// defined as ActivateEx(thread_manager, client_id, 0), which is the relationship the
// pinned SDK headers state between the two (msctf.h: ITfTextInputProcessorEx adds
// ActivateEx to ITfTextInputProcessor's Activate/Deactivate pair and nothing more).
//
// WHAT THIS OBJECT DOES NOT DO, AND MUST NOT BE READ AS DOING. There is no key sink, no
// keystroke handling, no edit session, no candidate window, no composition, no call into
// librime, no IPC. ITfTextInputProcessor::Activate on a real TIP is where TSF hands the
// service its thread manager and client id so it can advise sinks; this skeleton only
// records those two facts. Nothing here types text, and a green activation test says
// nothing about input capability -- it says the lifecycle arithmetic is right.
//
// NO REGISTRATION SIDE EFFECTS. Activate never touches the registry, the profile store
// or the input-language list; it takes what the caller hands it. The P3.1 round is
// explicitly unregistered, and the test program exercises this object through a
// LoadLibrary'd DLL, never through regsvr32.
//
// THE REACTIVATION CONTRACT (ours to define, because TSF never issues this sequence
// itself: activation and deactivation alternate 1:1). Activating an already-active
// service runs the full Deactivate path first, then activates -- self-healing, never a
// double-held thread-manager reference, never a leaked client id. Deactivating an
// inactive service is a successful no-op. Both answers are pinned by the test program.
//
// TfClientId IS STORED VERBATIM. The pinned SDK header marks no value invalid
// (msctf.h:1027 -- TfClientId is a plain DWORD with a uuid for marshalling only), so
// validating it here would mean inventing a rule. The one invalid input the SDK's own
// signature lets us detect -- a null ITfThreadMgr -- is rejected with E_INVALIDARG.
//
// THREADING. Reference counts use the Interlocked family (see module_state.h): a TSF
// host may create, call and release the service from the threads it chooses, and the
// object must stay arithmetically sound even though the skeleton's state itself is
// meant to be touched from the activating apartment only.
#ifndef EPOCANVA_TSF_TEXT_SERVICE_H_
#define EPOCANVA_TSF_TEXT_SERVICE_H_

#include <msctf.h>

namespace epocanva {
namespace tsf {

class EpoCanvaTextService : public ITfTextInputProcessorEx {
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

 private:
  ~EpoCanvaTextService();  // Release()-only lifecycle; drops a held thread manager

  // The one activation path both Activate and ActivateEx run.
  HRESULT ActivateInternal(ITfThreadMgr* thread_manager, TfClientId client_id,
                           DWORD flags);

  volatile LONG reference_count_;
  ITfThreadMgr* thread_manager_;  // AddRef'd while active, released by Deactivate
  TfClientId client_id_;
  DWORD activation_flags_;
  bool active_;
};

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_TEXT_SERVICE_H_
