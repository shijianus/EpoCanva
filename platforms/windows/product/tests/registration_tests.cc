// EpoCanva W3-B -- B-layer registration contract tests.
//
// These tests never touch the real registry. A complete in-memory RegistryOps stands in
// for HKLM/HKCU; the production Win32 backend is exercised only in an authorized layer-C
// session. What is covered here: exact key/value snapshots, idempotent re-registration,
// rollback after every injected create/write/read failure or missing readback, preservation
// of pre-existing category containers, non-recursive deletion and incomplete rollback,
// refusal of a foreign CLSID or non-Apartment class, unregister with shared categories,
// and that DllCanUnloadNow answers S_FALSE during value writes.
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
  std::size_t start_utf16 = 0;
  while (start_utf16 <= path.size()) {
    const std::size_t slash_utf16 = path.find(L'\\', start_utf16);
    parts.push_back(path.substr(
        start_utf16, slash_utf16 == std::wstring::npos ? std::wstring::npos
                                                      : slash_utf16 - start_utf16));
    if (slash_utf16 == std::wstring::npos) {
      break;
    }
    start_utf16 = slash_utf16 + 1;
  }
  return parts;
}

class FakeRegistry final : public tsf::RegistryOps {
 public:
  using KeyMap = std::map<std::wstring, std::map<std::wstring, std::wstring>>;
  struct Call {
    std::string action;
    std::wstring path;
  };

  FakeRegistry() {
    // Windows' shared ancestors are not owned by this registration transaction.
    for (KeyMap* map : {&machine, &user}) {
      EnsurePath(map, L"Software\\Classes\\CLSID");
      (*map)[L"Software\\Classes\\Unrelated"][L"Owner"] = L"OtherProduct";
    }
  }

  KeyMap machine;
  KeyMap user;
  int writes = 0;
  int operations_until_failure = -1;  // -1: never; 0: the next operation fails once
  int injected_failures = 0;
  bool missing_read_on_failure = false;  // successful readback of an absent key/value
  std::wstring delete_failure_path;  // persistent failure during rollback
  std::function<void()> on_write;
  std::vector<std::wstring> written_paths;
  std::vector<Call> calls;

  KeyMap& Map(tsf::RegistrationScope scope) {
    return scope == tsf::RegistrationScope::kMachine ? machine : user;
  }

  bool KeyExists(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* exists, std::wstring* why) override {
    *exists = false;
    if (!BeforeCall("key-exists", subkey, why)) {
      return missing_read_on_failure;
    }
    *exists = Map(scope).count(subkey) != 0;
    return true;
  }
  bool CreateKey(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* created, std::wstring* why) override {
    *created = false;
    if (!BeforeCall("create-key", subkey, why)) return false;
    KeyMap& map = Map(scope);
    const bool fresh = map.count(subkey) == 0;
    EnsurePath(&map, subkey);
    *created = fresh;
    return true;
  }
  bool DeleteKey(tsf::RegistrationScope scope, const std::wstring& subkey,
                 bool* removed, std::wstring* why) override {
    *removed = false;
    if (!BeforeCall("delete-key", subkey, why)) return false;
    if (!delete_failure_path.empty() && subkey == delete_failure_path) {
      if (why != nullptr) *why = L"injected rollback delete failure";
      return false;
    }
    KeyMap& map = Map(scope);
    const auto key = map.find(subkey);
    if (key == map.end()) return true;
    // RegDeleteKeyExW deletes a leaf and its values, never its subkeys.
    const std::wstring prefix = subkey + L"\\";
    const auto child = map.lower_bound(prefix);
    if (child != map.end() && child->first.compare(0, prefix.size(), prefix) == 0) {
      if (why != nullptr) *why = L"the key still has subkeys";
      return false;
    }
    map.erase(key);
    *removed = true;
    return true;
  }
  bool WriteString(tsf::RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, const std::wstring& value,
                   std::wstring* why) override {
    if (!BeforeCall("write-string", subkey, why)) return false;
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
    *exists = false;
    value->clear();
    if (!BeforeCall("read-string", subkey, why)) {
      return missing_read_on_failure;
    }
    KeyMap& map = Map(scope);
    const auto key = map.find(subkey);
    if (key == map.end()) return true;
    const auto found = key->second.find(value_name);
    if (found == key->second.end()) return true;
    *exists = true;
    *value = found->second;
    return true;
  }
  bool DeleteValue(tsf::RegistrationScope scope, const std::wstring& subkey,
                   const std::wstring& value_name, bool* removed,
                   std::wstring* why) override {
    *removed = false;
    if (!BeforeCall("delete-value", subkey, why)) return false;
    KeyMap& map = Map(scope);
    const auto key = map.find(subkey);
    *removed = key != map.end() && key->second.erase(value_name) != 0;
    return true;
  }
  bool ListSubkeys(tsf::RegistrationScope scope, const std::wstring& subkey,
                   std::vector<std::wstring>* names, std::wstring* why) override {
    names->clear();
    if (!BeforeCall("list-subkeys", subkey, why)) return false;
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
    return true;
  }
  bool ListValues(tsf::RegistrationScope scope, const std::wstring& subkey,
                  std::vector<std::wstring>* names, std::wstring* why) override {
    names->clear();
    if (!BeforeCall("list-values", subkey, why)) return false;
    const auto key = Map(scope).find(subkey);
    if (key != Map(scope).end()) {
      for (const auto& value : key->second) {
        names->push_back(value.first);
      }
    }
    return true;
  }

 private:
  bool BeforeCall(const char* action, const std::wstring& path, std::wstring* why) {
    calls.push_back({action, path});
    if (operations_until_failure == 0) {
      operations_until_failure = -1;
      ++injected_failures;
      if (why != nullptr) *why = L"injected registry operation failure";
      return false;
    }
    if (operations_until_failure > 0) --operations_until_failure;
    return true;
  }

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

void TestFakeDeleteKey() {
  FakeRegistry registry;
  registry.user[CategoriesKey()];
  registry.user[KeyboardKey()][L"ForeignValue"] = L"preserve";
  const auto before = registry.user;
  std::wstring why;
  bool removed = true;
  Check(!registry.DeleteKey(tsf::RegistrationScope::kUser, CategoriesKey(),
                            &removed, &why) && !removed && registry.user == before,
        "fake DeleteKey refuses subkeys without changing the tree");
  Check(registry.DeleteKey(tsf::RegistrationScope::kUser, KeyboardKey(),
                           &removed, &why) && removed,
        "fake DeleteKey removes a leaf including its values");
  Check(registry.DeleteKey(tsf::RegistrationScope::kUser, CategoriesKey(),
                           &removed, &why) && removed,
        "fake DeleteKey removes the parent only after its children");
  Check(registry.DeleteKey(tsf::RegistrationScope::kUser, CategoriesKey(),
                           &removed, &why) && !removed,
        "fake DeleteKey treats an absent key as an unchanged success");
}

enum class Fixture {
  kFresh,
  kOwnedBase,
  kEmptyCategoryRoot,
  kSharedCategories,
  kExistingRegistration,
};

void SeedFixture(FakeRegistry* registry, tsf::RegistrationScope scope,
                 Fixture fixture) {
  const auto other_scope = scope == tsf::RegistrationScope::kUser
                               ? tsf::RegistrationScope::kMachine
                               : tsf::RegistrationScope::kUser;
  SeedForeign(registry, other_scope);
  if (fixture == Fixture::kFresh) return;
  auto& map = registry->Map(scope);
  map[BaseKey()][tsf::RegistrationMarkerName()] = tsf::RegistrationMarkerValue();
  map[BaseKey()][L"ForeignValue"] = L"preserve base";
  map[BaseKey() + L"\\ForeignChild"][L"Owner"] = L"OtherProduct";
  if (fixture == Fixture::kOwnedBase) return;
  map[CategoriesKey()];
  if (fixture == Fixture::kEmptyCategoryRoot) return;
  map[CategoriesKey()][L"ForeignValue"] = L"preserve category root";
  map[CategoriesKey() + L"\\ForeignCategory"][L"Owner"] = L"OtherProduct";
  map[KeyboardKey()][L"ForeignValue"] = L"preserve existing category";
  map[KeyboardKey() + L"\\ForeignChild"][L"Owner"] = L"OtherProduct";
  map[InprocKey()][L""] = L"C:\\OldInstall\\epocanva_tsf.dll";
  map[InprocKey()][L"ForeignValue"] = L"preserve inproc";
  if (fixture == Fixture::kExistingRegistration) {
    // Ownership comes from the old server path; registration must restore this
    // previous marker and path if either update or any subsequent readback fails.
    map[BaseKey()][tsf::RegistrationMarkerName()] = L"legacy marker";
    map[InprocKey()][L"ThreadingModel"] = L"Apartment";
    map[UiElementKey()][L"ForeignValue"] = L"preserve second category";
  }
}

void TestRollback(bool self_check_fail) {
  // Derive a finite failure matrix from successful operation traces: all snapshot
  // reads, creates, value writes and all five post-category readbacks, in both hives.
  for (const auto scope :
       {tsf::RegistrationScope::kMachine, tsf::RegistrationScope::kUser}) {
    for (const auto fixture : {Fixture::kFresh, Fixture::kOwnedBase,
                              Fixture::kEmptyCategoryRoot, Fixture::kSharedCategories,
                              Fixture::kExistingRegistration}) {
      FakeRegistry initial;
      SeedFixture(&initial, scope, fixture);
      FakeRegistry successful = initial;
      const auto registered = tsf::RegisterTextServiceInScope(scope, kModulePath,
                                                             &successful);
      Check(registered.ok, "failure-matrix fixture registers successfully");
      bool mutation_phase = false;
      bool readback_phase = false;
      int readbacks = 0;
      for (std::size_t call_number = 0; call_number < successful.calls.size();
           ++call_number) {
        const auto& call = successful.calls[call_number];
        if (call.action == "create-key") mutation_phase = true;
        if (readback_phase) ++readbacks;
        // Also return success with an absent value/key at every verification call.
        for (int missing_read = 0; missing_read < (readback_phase ? 2 : 1);
             ++missing_read) {
          if (self_check_fail && !readback_phase) continue;
          FakeRegistry registry = initial;
          registry.operations_until_failure = static_cast<int>(call_number);
          registry.missing_read_on_failure = missing_read != 0;
          const auto result = tsf::RegisterTextServiceInScope(scope, kModulePath,
                                                             &registry);
          const bool exact = registry.machine == initial.machine &&
                             registry.user == initial.user;
          const std::string label =
              std::string(scope == tsf::RegistrationScope::kMachine ? "machine" : "user") +
              " fixture " + std::to_string(static_cast<int>(fixture)) +
              " operation " + std::to_string(call_number + 1) + " " + call.action +
              (missing_read ? " missing readback" : " failure");
          Check(registry.injected_failures == 1 && !result.ok &&
                    result.rolled_back == mutation_phase && !result.error.empty() &&
                    (mutation_phase || (!result.changed && registry.writes == 0)),
                label + " is reached and reports the correct failure/rollback state");
          // Include empty keys, all foreign values/children, and the untouched hive.
          Check(self_check_fail ? !exact : exact,
                label + " restores both complete registry snapshots exactly");
          if (self_check_fail) return;
        }
        if (call.action == "create-key" && call.path == UiElementKey()) {
          readback_phase = true;
        }
      }
      Check(readbacks == 5, "the matrix reaches every readback after both categories");
    }
  }
}

void TestIncompleteRollback() {
  FakeRegistry successful;
  Check(tsf::RegisterTextServiceInScope(tsf::RegistrationScope::kUser, kModulePath,
                                       &successful).ok,
        "incomplete-rollback trace registers");
  for (const auto& blocked_key : {UiElementKey(), CategoriesKey()}) {
    FakeRegistry registry;
    const auto before = registry.user;
    // Fail the final readback after all five keys exist, then fail cleanup too.
    registry.operations_until_failure = static_cast<int>(successful.calls.size()) - 1;
    registry.delete_failure_path = blocked_key;
    const auto result = tsf::RegisterTextServiceInScope(tsf::RegistrationScope::kUser,
                                                       kModulePath, &registry);
    Check(registry.injected_failures == 1 && !result.ok && !result.rolled_back &&
              result.error.find(L"rollback incomplete") != std::wstring::npos,
          "a failed category cleanup never claims a complete rollback");
    Check(registry.user != before && registry.user.count(blocked_key) == 1 &&
              registry.user.count(CategoriesKey()) == 1 &&
              registry.user.count(BaseKey()) == 1,
          "non-recursive deletion leaves the blocked category and ancestors visible");
  }
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
  const auto before_machine = registry.machine;
  const auto before_user = registry.user;
  for (const auto scope :
       {tsf::RegistrationScope::kMachine, tsf::RegistrationScope::kUser}) {
    Check(tsf::RegisterTextServiceInScope(scope, kModulePath, &registry).ok,
          "unregister fixture registered in each scope");
  }
  const tsf::RegistrationResult removed = tsf::UnregisterTextService(&registry);
  Check(removed.ok && removed.changed,
        "unregister succeeds and reports the removal");
  Check(registry.machine == before_machine && registry.user == before_user,
        "unregister restores both complete hives, including removal of empty parents");
  const tsf::RegistrationResult again = tsf::UnregisterTextService(&registry);
  Check(again.ok && !again.changed && registry.machine == before_machine &&
            registry.user == before_user,
        "a second unregister is an exact successful no-op");
}

void TestUnregisterKeepsSharedCategories() {
  for (const auto scope :
       {tsf::RegistrationScope::kMachine, tsf::RegistrationScope::kUser}) {
    // Test values alone as well as children: RegDeleteKeyExW only guards children.
    for (int content = 1; content <= 3; ++content) {
      FakeRegistry registry;
      const auto other_scope = scope == tsf::RegistrationScope::kUser
                                   ? tsf::RegistrationScope::kMachine
                                   : tsf::RegistrationScope::kUser;
      SeedForeign(&registry, other_scope);
      auto& map = registry.Map(scope);
      map[BaseKey()];
      map[CategoriesKey()];
      if ((content & 1) != 0) map[CategoriesKey()][L"ForeignValue"] = L"preserve";
      if ((content & 2) != 0) {
        map[CategoriesKey() + L"\\ForeignCategory"][L"Owner"] = L"OtherProduct";
      }
      const auto expected_machine = registry.machine;
      const auto expected_user = registry.user;
      map[BaseKey()][tsf::RegistrationMarkerName()] = tsf::RegistrationMarkerValue();
      Check(tsf::RegisterTextServiceInScope(scope, kModulePath, &registry).ok,
            "shared-category unregister fixture registers");
      const auto result = tsf::UnregisterTextService(&registry);
      Check(result.ok && result.changed && result.foreign_registration &&
                !HasProductContent(&registry, scope),
            "unregister removes product content without rejecting shared categories");
      Check(registry.machine == expected_machine && registry.user == expected_user,
            "unregister preserves all shared parent values/children and the other hive");
      const auto again = tsf::UnregisterTextService(&registry);
      Check(again.ok && !again.changed && registry.machine == expected_machine &&
                registry.user == expected_user,
            "repeated unregister leaves shared category content exactly intact");
    }
  }
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
    TestFakeDeleteKey();
    TestFreshAndIdempotent();
    TestRollback(false);
    TestIncompleteRollback();
    TestCanUnloadDuringWrite();
    TestForeignRefusal();
    TestThreadingRefusal();
    TestUnregister();
    TestUnregisterKeepsSharedCategories();
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
