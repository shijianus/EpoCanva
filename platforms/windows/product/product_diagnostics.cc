#include "product_diagnostics.h"

#include <objbase.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "identifiers.h"
#include "product_io.h"
#include "product_manifest.h"

namespace epocanva {
namespace product {
namespace {

struct RequiredFile {
  const char* relative_path;
  const wchar_t* label;
};

constexpr std::array<RequiredFile, 8> kRequiredFiles = {{
    {"EpoCanva.exe", L"Application"},
    {"epocanva_tsf.dll", L"Text service DLL"},
    {"rime.dll", L"English engine"},
    {"data/default.yaml", L"Engine default config"},
    {"data/epocanva_english.schema.yaml", L"English schema"},
    {"data/lexicon/epocanva_en_alpha.tsv", L"English lexicon"},
    {"NOTICE.txt", L"Notices"},
    {"VERSION.txt", L"Version file"},
}};

constexpr const char* kCapabilities =
    "English natural keyboard input through a TSF key sink, prefix completion and "
    "one-edit spelling suggestions. No candidate window, registration, installer, "
    "learning, other languages or system-wide input in this build.";

struct WindowsVersion {
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
};

WindowsVersion ReadWindowsVersion() {
  WindowsVersion version;
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return version;
  }
  const auto fn =
      reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
  if (fn == nullptr) {
    return version;
  }
  RTL_OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn(&info) != 0) {
    return version;
  }
  version.major = info.dwMajorVersion;
  version.minor = info.dwMinorVersion;
  version.build = info.dwBuildNumber;
  return version;
}

std::wstring ArchitectureName() {
  SYSTEM_INFO info = {};
  ::GetNativeSystemInfo(&info);
  switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return L"x64";
    case PROCESSOR_ARCHITECTURE_ARM64:
      return L"arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
      return L"x86";
    default:
      return L"unknown";
  }
}

std::wstring HostDescription() {
  const WindowsVersion version = ReadWindowsVersion();
  std::wstring name = L"Windows";
  if (version.major == 10 && version.build >= 22000) {
    name = L"Windows 11";
  } else if (version.major == 10) {
    name = L"Windows 10";
  } else if (version.major != 0) {
    name = L"Windows " + std::to_wstring(version.major) + L"." +
           std::to_wstring(version.minor);
  }
  if (version.build != 0) {
    name += L" " + std::to_wstring(version.major) + L"." +
            std::to_wstring(version.minor) + L"." + std::to_wstring(version.build);
  }
  name += L" (" + ArchitectureName() + L")";
  return name;
}

std::wstring GuidString(REFGUID guid) {
  wchar_t buffer[64] = {};
  const int written = ::StringFromGUID2(guid, buffer, 64);
  if (written <= 0) {
    return std::wstring();
  }
  return std::wstring(buffer, static_cast<std::size_t>(written - 1));
}

bool QueryInprocServer(HKEY root, REGSAM access, const std::wstring& subkey,
                       std::wstring* server_path) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, access, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buffer[2048] = {};
  DWORD type = 0;
  DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
  const LONG status = ::RegQueryValueExW(key, nullptr, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buffer), &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return false;
  }
  buffer[bytes / sizeof(wchar_t)] = L'\0';
  if (server_path != nullptr) {
    *server_path = buffer;
  }
  return true;
}

std::wstring FileStateFromVerification(const FileVerification* verification,
                                       bool manifest_ok) {
  if (verification == nullptr) {
    return manifest_ok ? L"not listed in package.manifest" : L"not verified";
  }
  if (!verification->present) {
    return L"missing";
  }
  if (!verification->size_ok) {
    return L"size mismatch";
  }
  if (!verification->hash_ok) {
    return L"SHA-256 mismatch";
  }
  return L"OK";
}

const FileVerification* FindVerification(
    const std::vector<FileVerification>& files, const std::string& relative_path) {
  for (const FileVerification& file : files) {
    if (file.entry.relative_path == relative_path) {
      return &file;
    }
  }
  return nullptr;
}

void AddItem(ProductStatus* status, std::wstring label, std::wstring detail,
             StatusLevel level) {
  status->items.push_back(StatusItem{std::move(label), std::move(detail), level});
}

}  // namespace

bool IsTextServiceRegistered(std::wstring* server_path) {
  const std::wstring guid = GuidString(epocanva::tsf::kTextServiceClsid);
  if (guid.empty()) {
    return false;
  }
  const std::wstring subkey =
      L"SOFTWARE\\Classes\\CLSID\\" + guid + L"\\InprocServer32";
  if (QueryInprocServer(HKEY_LOCAL_MACHINE, KEY_READ | KEY_WOW64_64KEY, subkey,
                        server_path)) {
    return true;
  }
  return QueryInprocServer(HKEY_CURRENT_USER, KEY_READ, subkey, server_path);
}

bool CollectProductStatus(HMODULE module, ProductStatus* status, std::wstring* why) {
  if (status == nullptr) {
    if (why != nullptr) {
      *why = L"CollectProductStatus received a null status pointer";
    }
    return false;
  }
  ProductPaths paths;
  if (!ResolveProductPaths(module, &paths, why)) {
    return false;
  }
  return CollectProductStatusForPaths(paths, status, why);
}

bool CollectProductStatusForPaths(const ProductPaths& paths, ProductStatus* status,
                                  std::wstring* why) {
  if (status == nullptr) {
    if (why != nullptr) {
      *why = L"CollectProductStatusForPaths received a null status pointer";
    }
    return false;
  }
  ProductStatus collected;
  collected.paths = paths;
  collected.version = ProductVersion();
  collected.commit = ProductCommit();
  collected.host = HostDescription();

  ProductManifest manifest;
  std::wstring manifest_why;
  collected.manifest_ok = LoadProductManifest(paths.manifest_file, &manifest,
                                              &manifest_why);
  std::vector<FileVerification> verifications;
  if (collected.manifest_ok) {
    collected.manifest_files = static_cast<int>(manifest.files.size());
    std::wstring verify_why;
    if (!VerifyProductManifest(paths.product_root, manifest, &verifications,
                               &collected.manifest_failures, &verify_why)) {
      collected.manifest_ok = false;
      collected.manifest_issue = verify_why;
    } else if (manifest.version != WideToUtf8(collected.version)) {
      collected.manifest_ok = false;
      collected.manifest_issue =
          L"package.manifest version does not match this executable";
    } else {
      if (!manifest.commit.empty() &&
          manifest.commit != WideToUtf8(collected.commit)) {
        AddItem(&collected, L"Package build",
                L"manifest commit differs from the executable's commit (warning)",
                StatusLevel::kWarning);
      }
      AddItem(&collected, L"Package integrity",
              collected.manifest_failures == 0
                  ? (L"all " + std::to_wstring(collected.manifest_files) +
                     L" listed files match their SHA-256")
                  : (std::to_wstring(collected.manifest_failures) + L" of " +
                     std::to_wstring(collected.manifest_files) +
                     L" listed files failed verification"),
              collected.manifest_failures == 0 ? StatusLevel::kOk
                                               : StatusLevel::kError);
    }
  }
  if (!collected.manifest_ok) {
    collected.manifest_issue =
        collected.manifest_issue.empty() ? manifest_why : collected.manifest_issue;
    AddItem(&collected, L"Package integrity",
            collected.manifest_issue.empty() ? L"package.manifest is unavailable"
                                             : collected.manifest_issue,
            StatusLevel::kError);
  }

  bool required_ok = true;
  for (const RequiredFile& required : kRequiredFiles) {
    const ManifestEntry* entry =
        collected.manifest_ok ? FindManifestEntry(manifest, required.relative_path)
                              : nullptr;
    const FileVerification* verification =
        collected.manifest_ok
            ? FindVerification(verifications, required.relative_path)
            : nullptr;
    const std::wstring state =
        collected.manifest_ok ? FileStateFromVerification(verification, true)
                              : (std::filesystem::is_regular_file(
                                     paths.product_root /
                                     std::filesystem::u8path(required.relative_path))
                                     ? L"present, not verified"
                                     : L"missing");
    const bool ok = collected.manifest_ok
                        ? (verification != nullptr && verification->ok())
                        : (!state.empty() && state != L"missing");
    if (!ok) {
      required_ok = false;
    }
    AddItem(&collected, required.label,
            std::wstring(entry != nullptr || !collected.manifest_ok ? L"" : L"not listed - ") +
                state,
            ok ? StatusLevel::kOk : StatusLevel::kError);
  }

  std::error_code ec;
  int license_files = 0;
  if (std::filesystem::is_directory(paths.licenses_dir, ec) && !ec) {
    for (std::filesystem::directory_iterator it(paths.licenses_dir, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        ++license_files;
      }
    }
  }
  AddItem(&collected, L"Licences",
          license_files > 0 ? (std::to_wstring(license_files) + L" files in " +
                               paths.licenses_dir.wstring())
                            : L"licenses folder is missing or empty",
          license_files > 0 ? StatusLevel::kOk : StatusLevel::kError);

  std::wstring user_why;
  collected.user_data_ok = ProbeUserDataWritable(paths, &user_why);
  AddItem(&collected, L"User data", collected.user_data_ok
                                        ? paths.user_root.wstring()
                                        : user_why,
          collected.user_data_ok ? StatusLevel::kOk : StatusLevel::kError);

  collected.settings = LoadProductSettings(paths.settings_file);
  const SettingsLoadStatus settings_status = collected.settings.status;
  std::wstring settings_detail = paths.settings_file.wstring();
  if (settings_status != SettingsLoadStatus::kLoaded) {
    settings_detail += L" - ";
    settings_detail += DescribeSettingsLoadStatus(settings_status);
    if (!collected.settings.diagnostic.empty()) {
      settings_detail += L": " + collected.settings.diagnostic;
    }
  }
  AddItem(&collected, L"Settings", settings_detail,
          settings_status == SettingsLoadStatus::kLoaded ? StatusLevel::kOk
                                                         : StatusLevel::kWarning);

  std::wstring server_path;
  collected.tsf_registered = IsTextServiceRegistered(&server_path);
  AddItem(&collected, L"Text service registration",
          collected.tsf_registered
              ? (L"CLSID is registered: " + server_path)
              : L"not registered - the key sink is wired, but system registration is "
                L"the W3 installer step",
          collected.tsf_registered ? StatusLevel::kOk : StatusLevel::kWarning);

  const WindowsVersion version = ReadWindowsVersion();
  const bool host_ok = version.major == 10 && version.build >= 22000 &&
                       ArchitectureName() == L"x64";
  AddItem(&collected, L"Host", collected.host,
          host_ok ? StatusLevel::kOk : StatusLevel::kWarning);
  AddItem(&collected, L"Capabilities", Utf8ToWide(kCapabilities), StatusLevel::kOk);

  collected.package_ready = collected.manifest_ok && collected.manifest_failures == 0 &&
                            required_ok && license_files > 0 && collected.user_data_ok;
  *status = std::move(collected);
  return true;
}

std::string FormatProductReport(const ProductStatus& status) {
  std::string report;
  auto line = [&report](const std::string& key, const std::string& value) {
    report += key;
    report += '=';
    report += value;
    report += '\n';
  };
  line("product", "EpoCanva");
  line("version", WideToUtf8(status.version));
  line("commit", WideToUtf8(status.commit));
  line("host", WideToUtf8(status.host));
  line("package", status.package_ready ? "ready" : "incomplete");
  line("manifest", status.manifest_ok ? "ok" : "failed");
  line("manifest_files", std::to_string(status.manifest_files));
  line("manifest_failures", std::to_string(status.manifest_failures));
  line("manifest_issue", WideToUtf8(status.manifest_issue));
  line("user_data", status.user_data_ok ? "ok" : "error");
  line("user_root", WideToUtf8(status.paths.user_root.wstring()));
  line("install_root", WideToUtf8(status.paths.product_root.wstring()));
  line("settings", [&status] {
    switch (status.settings.status) {
      case SettingsLoadStatus::kLoaded:
        return std::string("loaded");
      case SettingsLoadStatus::kMissing:
        return std::string("missing");
      case SettingsLoadStatus::kCorrupt:
        return std::string("corrupt");
      case SettingsLoadStatus::kUnsupportedVersion:
        return std::string("unsupported");
    }
    return std::string("unknown");
  }());
  line("settings_file", WideToUtf8(status.paths.settings_file.wstring()));
  line("tsf_registration", status.tsf_registered ? "yes" : "no");
  line("capabilities", kCapabilities);
  for (std::size_t index = 0; index < status.items.size(); ++index) {
    const StatusItem& item = status.items[index];
    const std::string prefix = "item." + std::to_string(index);
    line(prefix + ".level",
         item.level == StatusLevel::kOk
             ? "ok"
             : (item.level == StatusLevel::kWarning ? "warning" : "error"));
    line(prefix + ".label", WideToUtf8(item.label));
    line(prefix + ".detail", WideToUtf8(item.detail));
  }
  return report;
}

std::wstring FormatStatusLines(const ProductStatus& status) {
  std::wstring lines;
  for (const StatusItem& item : status.items) {
    switch (item.level) {
      case StatusLevel::kOk:
        lines += L"[OK] ";
        break;
      case StatusLevel::kWarning:
        lines += L"[!]  ";
        break;
      case StatusLevel::kError:
        lines += L"[X]  ";
        break;
    }
    lines += item.label;
    lines += L": ";
    lines += item.detail;
    lines += L"\r\n";
  }
  return lines;
}

std::wstring PackageSummary(const ProductStatus& status) {
  if (status.package_ready) {
    return L"Package ready - " + std::to_wstring(status.manifest_files) +
           L" files verified";
  }
  return L"Package incomplete - see status below";
}

}  // namespace product
}  // namespace epocanva
