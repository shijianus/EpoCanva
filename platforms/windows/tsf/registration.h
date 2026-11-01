// EpoCanva W3-B -- COM registration for the TSF text service.
//
// WHAT IS WRITTEN. Only the keys that make this product's own CLSID loadable as a TSF
// keyboard TIP: CLSID (with the EpoCanvaProduct marker), InprocServer32 (module path and
// Apartment threading), and the two implementation categories
// GUID_TFCAT_TIP_KEYBOARD / {34745C63-B2F0-4784-8B67-5E12C8701A31} and
// GUID_TFCAT_TIPCAP_UIELEMENTENABLED / {49D2F9CF-1F5E-11D7-A6D3-00065B84435C}.
// No language profile, no input-method selection, no other product's key.
//
// SCOPE. Machine (HKLM, needs elevation) and user (HKCU) are both supported. The exported
// DllRegisterServer chooses: an existing product registration in one scope is updated in
// place; otherwise an elevated process writes HKLM and a normal process writes HKCU.
// Whether TSF honours a per-user TIP registration is not established here; the report
// records it as unverified until a real per-user round is run.
//
// SAFETY. Every write is read back. Any failure rolls back everything this call created or
// changed. A CLSID that belongs to another product (no EpoCanva marker and an
// InprocServer32 that is not epocanva_tsf.dll) is refused before the first write.
//
// THIRD PARTY REACHABILITY. `RegistryOps` is the only seam: the product uses the Win32
// backend below, the B-layer tests use an in-memory backend and therefore never touch the
// real registry.
#ifndef EPOCANVA_TSF_REGISTRATION_H_
#define EPOCANVA_TSF_REGISTRATION_H_

#include <windows.h>

#include <string>
#include <vector>

namespace epocanva {
namespace tsf {

enum class RegistrationScope {
  kMachine = 0,
  kUser = 1,
};

struct RegistrationOperation {
  std::string action;  // create-key, set-default, set-value, remove-key, remove-value,
                       // rollback, verify
  std::string path;
  std::string value;
  std::string result;
};

struct RegistrationResult {
  bool ok = false;
  bool changed = false;
  bool rolled_back = false;
  // True when the key existed and belonged to another product. The caller must not treat
  // this as a transient failure: nothing was or will be written under that identity.
  bool foreign_registration = false;
  bool machine_present_before = false;
  bool user_present_before = false;
  RegistrationScope scope = RegistrationScope::kMachine;
  std::wstring error;
  std::vector<RegistrationOperation> operations;

  std::string ToJson() const;
};

class RegistryOps {
 public:
  virtual ~RegistryOps() {}
  virtual bool KeyExists(RegistrationScope scope, const std::wstring& subkey,
                         bool* exists, std::wstring* why) = 0;
  virtual bool CreateKey(RegistrationScope scope, const std::wstring& subkey,
                         bool* created, std::wstring* why) = 0;
  virtual bool DeleteKey(RegistrationScope scope, const std::wstring& subkey,
                         bool* removed, std::wstring* why) = 0;
  virtual bool WriteString(RegistrationScope scope, const std::wstring& subkey,
                           const std::wstring& value_name, const std::wstring& value,
                           std::wstring* why) = 0;
  virtual bool ReadString(RegistrationScope scope, const std::wstring& subkey,
                          const std::wstring& value_name, std::wstring* value,
                          bool* exists, std::wstring* why) = 0;
  virtual bool DeleteValue(RegistrationScope scope, const std::wstring& subkey,
                           const std::wstring& value_name, bool* removed,
                           std::wstring* why) = 0;
  virtual bool ListSubkeys(RegistrationScope scope, const std::wstring& subkey,
                           std::vector<std::wstring>* names, std::wstring* why) = 0;
  virtual bool ListValues(RegistrationScope scope, const std::wstring& subkey,
                          std::vector<std::wstring>* names, std::wstring* why) = 0;
};

// The real registry. HKLM is read and written through the 64-bit view; HKCU uses its
// default view. Every path is relative to Software\Classes.
class Win32RegistryOps final : public RegistryOps {
 public:
  bool KeyExists(RegistrationScope scope, const std::wstring& subkey, bool* exists,
                 std::wstring* why) override;
  bool CreateKey(RegistrationScope scope, const std::wstring& subkey, bool* created,
                 std::wstring* why) override;
  bool DeleteKey(RegistrationScope scope, const std::wstring& subkey, bool* removed,
                 std::wstring* why) override;
  bool WriteString(RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, const std::wstring& value,
                   std::wstring* why) override;
  bool ReadString(RegistrationScope scope, const std::wstring& subkey,
                  const std::wstring& value_name, std::wstring* value, bool* exists,
                  std::wstring* why) override;
  bool DeleteValue(RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, bool* removed,
                   std::wstring* why) override;
  bool ListSubkeys(RegistrationScope scope, const std::wstring& subkey,
                   std::vector<std::wstring>* names, std::wstring* why) override;
  bool ListValues(RegistrationScope scope, const std::wstring& subkey,
                  std::vector<std::wstring>* names, std::wstring* why) override;
};

bool IsProcessElevated();

// Registers exactly one scope. `module_path` is the absolute path written to
// InprocServer32. Never falls back to another scope: the caller chooses.
RegistrationResult RegisterTextServiceInScope(RegistrationScope scope,
                                              const std::wstring& module_path,
                                              RegistryOps* ops);

// Chooses the scope (existing registration first, then elevation) and registers.
RegistrationResult RegisterTextServiceAuto(const std::wstring& module_path,
                                           RegistryOps* ops);

// Removes this product's registration from both scopes. Foreign keys are left untouched
// and reported; absence is success.
RegistrationResult UnregisterTextService(RegistryOps* ops);

const wchar_t* RegistrationClsidText();
const wchar_t* RegistrationKeyboardCategoryText();
const wchar_t* RegistrationUiElementCategoryText();
const wchar_t* RegistrationMarkerName();
const wchar_t* RegistrationMarkerValue();

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_REGISTRATION_H_
