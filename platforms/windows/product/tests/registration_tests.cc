// EpoCanva W3-B -- B-layer registration contract tests.
//
// These tests never touch the real registry. A complete in-memory RegistryOps stands in
// for HKLM/HKCU; the production Win32 backend is exercised only in an authorized layer-C
// session. What is proven here: the exact key/value set, readback verification, idempotent
// re-registration, rollback after an injected write failure, refusal of a foreign CLSID,
// refusal to change an existing non-Apartment class, unregister/readback-absent, and that
// DllCanUnloadNow answers S_FALSE for the whole write sequence.
#include <windows.h>

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "identifiers.h"
#include "module_state.h"
#include "registration.h"

namespace {

namespace tsf = epocanva::tsf;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

const wchar_t kModulePath[] =
    L"C:\\Program Files\\EpoCanva\\epocanva_tsf.dll";

std::vector<std::wstring> SplitPath(const std::wstring& path) {
  std::vector<std::wstring> parts;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find(L'\\', start);
    parts.push_back(path.substr(start, slash == std::wstring::npos
                                           ? std::wstring::npos
                                           : slash - start));
    if (slash == std::wstring::npos) {
      break;
    }
    start = slash + 1;
  }
  return parts;
}

class FakeRegistry final : public tsf::RegistryOps {
 public:
  using KeyMap = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

  KeyMap machine;
  KeyMap user;
  int writes = 0;
  int writes_until_failure = -1;  // -1: never; 0: the next write fails
  std::function<void()> on_write;
  std::vector<std::wstring> written_paths;

  KeyMap& Map(tsf::RegistrationScope scope) {
    return scope == tsf::RegistrationScope::kMachine ? machine : user;
  }

  bool KeyExists(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* exists, std::wstring* why) override {
    (void)why;
    *exists = Map(scope).count(subkey) != 0;
    return true;
  }
  bool CreateKey(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* created, std::wstring* why) override {
    KeyMap& map = Map(scope);
    const bool fresh = map.count(subkey) == 0;
    EnsurePath(&map, subkey);
    *created = fresh;
    (void)why;
    return true;
  }
  bool DeleteKey(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* removed, std::wstring* why) override {
    KeyMap& map = Map(scope);
    *removed = false;
    for (auto it = map.begin(); it != map.end();) {
      const std::wstring& key = it->first;
      const bool match = key == subkey ||
                         (key.size() > subkey.size() &&
                          key.compare(0, subkey.size(), subkey) == 0 &&
                          key[subkey.size()] == L'\\');
      if (match) {
        *removed = true;
        it = map.erase(it);
      } else {
        ++it;
      }
    }
    (void)why;
    return true;
  }
  bool WriteString(tsf::RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, const std::wstring& value,
                   std::wstring* why) override {
    if (writes_until_failure == 0) {
      writes_until_failure = -1;
      *why = L"injected write failure";
      return false;
    }
    if (writes_until_failure > 0) {
      --writes_until_failure;
    }
    KeyMap& map = Map(scope);
    EnsurePath(&map, subkey);
    map[subkey][value_name] = value;
    ++writes;
    written_paths.push_back(subkey);
    if (on_write) {
      on_write();
    }
    return true;
  }
  bool ReadString(tsf::RegistrationScope scope, const std::wstring& subkey,
                  const std::wstring& value_name, std::wstring* value, bool* exists,
                  std::wstring* why) override {
    KeyMap& map = Map(scope);
    const auto key = map.find(subkey);
    if (key == map.end()) {
      *exists = false;
      value->clear();
      return true;
    }
    const auto found = key->second.find(value_name);
    if (found == key->second.end()) {
      *exists = false;
      value->clear();
      return true;
    }
    *exists = true;
    *value = found->second;
    (void)why;
    return true;
  }
  bool DeleteValue(tsf::RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, bool* removed,
                   std::wstring* why) override {
    KeyMap& map = Map(scope);
    const auto key = map.find(subkey);
    *removed = key != map.end() && key->second.erase(value_name) != 0;
    (void)why;
    return true;
  }
  bool ListSubkeys(tsf::RegistrationScope scope, const std::wstring& subkey,
                   std::vector<std::wstring>* names, std::wstring* why) override {
    names->clear();
    const std::wstring prefix = subkey + L"\\";
    for (const auto& entry : Map(scope)) {
      if (entry.first.size() > prefix.size() &&
          entry.first.compare(0, prefix.size(), prefix) == 0) {
        const std::wstring rest = entry.first.substr(prefix.size());
        if (rest.find(L'\\') == std::wstring::npos) {
          names->push_back(rest);
        }
      }
    }
    (void)why;
    return true;
  }
  bool ListValues(tsf::RegistrationScope scope, const std::wstring& subkey,
                  std::vector<std::wstring>* names, std::wstring* why) override {
    names->clear();
    const auto key = Map(scope).find(subkey);
    if (key != Map(scope).end()) {
      for (const auto& value : key->second) {
        names->push_back(value.first);
      }
    }
    (void)why;
    return true;
  }

 private:
  static void EnsurePath(KeyMap* map, const std::wstring& path) {
    std::wstring current;
    for (const std::wstring& part : SplitPath(path)) {
      current = current.empty() ? part : current + L"\\" + part;
      (*map)[current];
    }
  }
};

const std::wstring& BaseKey() {
  static const std::wstring key =
      std::wstring(L"Software\\Classes\\CLSID\\") + tsf::RegistrationClsidText();
  return key;
}
std::wstring InprocKey() { return BaseKey() + L"\\InprocServer32"; }
std::wstring CategoriesKey() { return BaseKey() + L"\\Implemented Categories"; }
std::wstring KeyboardKey() {
  return CategoriesKey() + L"\\" + tsf::RegistrationKeyboardCategoryText();
}
std::wstring UiElementKey() {
  return CategoriesKey() + L"\\" + tsf::RegistrationUiElementCategoryText();
}

bool HasProductContent(FakeRegistry* registry, tsf::RegistrationScope scope) {
  FakeRegistry::KeyMap& map = registry->Map(scope);
  return map.count(InprocKey()) != 0 || map.count(KeyboardKey()) != 0 ||
         map.count(UiElementKey()) != 0 ||
         (map.count(BaseKey()) != 0 &&
          map[BaseKey()].count(tsf::RegistrationMarkerName()) != 0);
}

void SeedForeign(FakeRegistry* registry, tsf::RegistrationScope scope) {
  registry->Map(scope)[BaseKey()][L"EpoCanvaProduct"] = L"OtherProduct";
  registry->Map(scope)[InprocKey()][L""] = L"C:\\Other\\other_tsf.dll";
  registry->Map(scope)[InprocKey()][L"ThreadingModel"] = L"Apartment";
}

void TestFreshAndIdempotent() {
  FakeRegistry registry;
  const tsf::RegistrationResult first = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(first.ok && first.changed, "fresh registration succeeds and reports a change");
  Check(registry.Map(tsf::RegistrationScope::kUser)[BaseKey()]
                [tsf::RegistrationMarkerName()] == tsf::RegistrationMarkerValue(),
        "the product marker is written");
  Check(registry.Map(tsf::RegistrationScope::kUser)[InprocKey()][L""] ==
            kModulePath,
        "InprocServer32 points at the module");
  Check(registry.Map(tsf::RegistrationScope::kUser)[InprocKey()][L"ThreadingModel"] ==
            L"Apartment",
        "ThreadingModel is Apartment");
  Check(registry.Map(tsf::RegistrationScope::kUser).count(KeyboardKey()) == 1 &&
            registry.Map(tsf::RegistrationScope::kUser).count(UiElementKey()) == 1,
        "both implementation categories exist");
  Check(first.ToJson().find("\"ok\":true") != std::string::npos &&
            first.ToJson().find("\"operations\":[") != std::string::npos,
        "the result serializes to machine-readable JSON");

  const tsf::RegistrationResult second = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(second.ok && !second.changed,
        "a second registration is idempotent (same result, no change)");
  Check(registry.Map(tsf::RegistrationScope::kUser)[InprocKey()][L""] ==
            kModulePath,
        "the second registration leaves the same server path");
}

void TestRollback(bool self_check_fail) {
  FakeRegistry registry;
  registry.writes_until_failure = 2;  // marker and server path succeed, threading fails
  const tsf::RegistrationResult result = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  const bool has_content = HasProductContent(&registry, tsf::RegistrationScope::kUser);
  // The falsifiability control claims that a failed registration left content behind.
  Check(self_check_fail ? has_content : (!result.ok && result.rolled_back &&
                                         !has_content),
        self_check_fail
            ? "rollback control: deliberately claims failed writes left keys"
            : "a failed write rolls back every key and value this call wrote");
}

void TestCanUnloadDuringWrite() {
  FakeRegistry registry;
  bool saw_s_false = false;
  registry.on_write = [&saw_s_false]() {
    if (!tsf::ModuleCanUnload()) {
      saw_s_false = true;
    }
  };
  Check(tsf::ModuleCanUnload(), "the module is unloadable before registration");
  const tsf::RegistrationResult result = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(result.ok && saw_s_false,
        "DllCanUnloadNow answers S_FALSE while registry keys are written");
  Check(tsf::ModuleCanUnload(), "the module is unloadable again after registration");
}

void TestForeignRefusal() {
  FakeRegistry registry;
  SeedForeign(&registry, tsf::RegistrationScope::kUser);
  const auto before = registry.user;
  const tsf::RegistrationResult result = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(!result.ok && result.foreign_registration && !result.changed,
        "a foreign CLSID is refused before any write");
  Check(registry.user == before, "a refused registration changes no key");
}

void TestThreadingRefusal() {
  FakeRegistry registry;
  registry.user[BaseKey()][tsf::RegistrationMarkerName()] =
      tsf::RegistrationMarkerValue();
  registry.user[InprocKey()][L""] = kModulePath;
  registry.user[InprocKey()][L"ThreadingModel"] = L"Both";
  const tsf::RegistrationResult result = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(!result.ok && registry.user[InprocKey()][L"ThreadingModel"] == L"Both",
        "an existing non-Apartment class is refused and left unchanged");
}

void TestUnregister() {
  FakeRegistry registry;
  const auto registered = tsf::RegisterTextServiceInScope(
      tsf::RegistrationScope::kUser, kModulePath, &registry);
  Check(registered.ok, "unregister fixture registered");
  const tsf::RegistrationResult removed = tsf::UnregisterTextService(&registry);
  Check(removed.ok && removed.changed,
        "unregister succeeds and reports the removal");
  Check(!HasProductContent(&registry, tsf::RegistrationScope::kUser),
        "unregister leaves no product key or marker");
  const tsf::RegistrationResult again = tsf::UnregisterTextService(&registry);
  Check(again.ok && !again.changed,
        "a second unregister is a successful no-op");
}

void TestUnregisterKeepsForeign() {
  FakeRegistry registry;
  SeedForeign(&registry, tsf::RegistrationScope::kUser);
  const auto before = registry.user;
  const tsf::RegistrationResult result = tsf::UnregisterTextService(&registry);
  Check(result.ok && result.foreign_registration,
        "unregister reports a foreign key without failing");
  Check(registry.user == before, "unregister never deletes another product's key");
}

void TestBothScopes() {
  for (tsf::RegistrationScope scope :
       {tsf::RegistrationScope::kMachine, tsf::RegistrationScope::kUser}) {
    FakeRegistry registry;
    const tsf::RegistrationResult result =
        tsf::RegisterTextServiceInScope(scope, kModulePath, &registry);
    Check(result.ok && result.scope == scope,
          scope == tsf::RegistrationScope::kMachine
              ? "the machine scope registers"
              : "the user scope registers");
    Check(registry.Map(scope).count(BaseKey()) == 1,
          "the chosen scope received the CLSID key");
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool self_check_fail = false;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--self-check-fail") {
      self_check_fail = true;
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout << "EpoCanva W3-B registration (fake registry) tests\n";
  if (self_check_fail) {
    TestRollback(true);
  } else {
    TestFreshAndIdempotent();
    TestRollback(false);
    TestCanUnloadDuringWrite();
    TestForeignRefusal();
    TestThreadingRefusal();
    TestUnregister();
    TestUnregisterKeepsForeign();
    TestBothScopes();
  }
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
