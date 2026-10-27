// EpoCanva W1 -- product wiring between the TSF DLL and the engine host.
//
// Called from DllGetClassObject (never DllMain) so the text service has an engine
// environment before the first Activate. When settings.ini was saved, the marker
// written by EpoCanva.exe requests a one-shot deployment on this activation; the marker
// is removed only after the broker has actually started the runtime.
#ifndef EPOCANVA_PRODUCT_ENGINE_CONFIG_H_
#define EPOCANVA_PRODUCT_ENGINE_CONFIG_H_

#include <windows.h>

#include <string>

namespace epocanva {
namespace product {

bool ApplyProductEngineConfig(HMODULE module, std::wstring* why);
void NotifyEngineStarted();

// Test/diagnostic read-back. Empty when no deployment is pending in this process.
bool PendingDeploymentToken(std::string* token);

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_ENGINE_CONFIG_H_
