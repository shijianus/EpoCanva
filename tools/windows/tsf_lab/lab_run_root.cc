// EpoCanva P3.13-R1 -- guarded fresh run-directory preparation. See lab_run_root.h.
#include "lab_run_root.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

namespace fs = std::filesystem;

std::string Narrow(const fs::path& path) { return path.u8string(); }

std::string Win32Text(DWORD code) {
  return std::system_category().message(static_cast<int>(code));
}

bool Fail(std::string* error, const std::string& text) {
  if (error != nullptr) {
    *error = text;
  }
  return false;
}

bool IsDirectory(DWORD attributes) {
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsReparse(DWORD attributes) {
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Reports existence and attributes without following a final reparse point. A missing
// ancestor, including a path below a regular file, is reported as "does not exist"; the
// caller walks upward and refuses the non-directory ancestor.
bool QueryAttributes(const fs::path& path, bool* exists, DWORD* attributes,
                     std::string* error) {
  const DWORD value = ::GetFileAttributesW(path.c_str());
  if (value == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      *exists = false;
      *attributes = 0;
      return true;
    }
    return Fail(error, "cannot inspect " + Narrow(path) + " (" +
                           Win32Text(code) + ")");
  }
  *exists = true;
  *attributes = value;
  return true;
}

bool CanonicalExisting(const fs::path& path, fs::path* resolved,
                       std::string* error) {
  std::error_code ec;
  const fs::path value = fs::canonical(path, ec);
  if (ec) {
    return Fail(error,
                "cannot resolve " + Narrow(path) + " (" + ec.message() + ")");
  }
  *resolved = value;
  return true;
}

bool SameComponent(const fs::path& left, const fs::path& right) {
  const std::wstring a = left.native();
  const std::wstring b = right.native();
  return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

// Component-wise containment, not a string-prefix comparison: "run-evil" is outside
// "run" even though one path string starts with the other.
bool SameOrDescendant(const fs::path& base, const fs::path& candidate) {
  if (!base.is_absolute() || !candidate.is_absolute()) {
    return false;
  }
  auto base_part = base.begin();
  auto candidate_part = candidate.begin();
  for (; base_part != base.end(); ++base_part, ++candidate_part) {
    if (candidate_part == candidate.end() ||
        !SameComponent(*base_part, *candidate_part)) {
      return false;
    }
  }
  return true;
}

bool SafePathComponent(const fs::path& component) {
  const std::wstring text = component.native();
  if (text.empty() || text == L"." || text == L".." || text.size() > 255) {
    return false;
  }
  if (text.back() == L'.' || text.back() == L' ') {
    return false;
  }
  for (const wchar_t value : text) {
    if (value < 0x20) {
      return false;
    }
    switch (value) {
      case L'\\':
      case L'/':
      case L':':
      case L'*':
      case L'?':
      case L'"':
      case L'<':
      case L'>':
      case L'|':
        return false;
      default:
        break;
    }
  }
  return true;
}

bool SafeRunStem(const std::string& stem) {
  if (stem.empty() || stem.size() > 96) {
    return false;
  }
  for (const char raw : stem) {
    const unsigned char value = static_cast<unsigned char>(raw);
    if (value < 0x21 || value > 0x7E) {
      return false;
    }
    switch (value) {
      case '\\':
      case '/':
      case ':':
      case '*':
      case '?':
      case '"':
      case '<':
      case '>':
      case '|':
        return false;
      default:
        break;
    }
  }
  return stem != "." && stem != ".." && stem.back() != '.';
}

bool ResolveTrustedRoot(const fs::path& trusted_root, fs::path* resolved,
                        std::string* error) {
  if (trusted_root.empty() || !trusted_root.is_absolute()) {
    return Fail(error, "trusted run root must be an absolute build path");
  }
  bool exists = false;
  DWORD attributes = 0;
  if (!QueryAttributes(trusted_root, &exists, &attributes, error)) {
    return false;
  }
  if (!exists) {
    const fs::path parent = trusted_root.parent_path();
    bool parent_exists = false;
    DWORD parent_attributes = 0;
    if (parent.empty() || parent == trusted_root) {
      return Fail(error, "trusted run root has no existing directory parent");
    }
    if (!QueryAttributes(parent, &parent_exists, &parent_attributes, error)) {
      return false;
    }
    if (!parent_exists || !IsDirectory(parent_attributes)) {
      return Fail(error, "trusted run root has no existing directory parent");
    }
    std::error_code ec;
    fs::create_directory(trusted_root, ec);
    if (ec) {
      return Fail(error, "cannot create trusted run root " +
                             Narrow(trusted_root) + " (" + ec.message() + ")");
    }
    if (!QueryAttributes(trusted_root, &exists, &attributes, error)) {
      return false;
    }
  }
  if (!exists || !IsDirectory(attributes)) {
    return Fail(error,
                "trusted run root is not a directory: " + Narrow(trusted_root));
  }
  // A trusted root reached through a reparse point is accepted; its canonical target is
  // the anchor. User-supplied components are checked against that anchor below.
  return CanonicalExisting(trusted_root, resolved, error);
}

// Creates a run directory that did not exist before. Existing candidates are preserved
// and skipped; no candidate is ever opened, cleaned or reused.
bool CreateFreshRunDirectory(const fs::path& parent, const std::string& stem,
                             unsigned max_attempts,
                             LabRunDirectories* directories,
                             std::string* error) {
  for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
    const std::string name =
        attempt == 0 ? stem : stem + "-" + std::to_string(attempt);
    const fs::path candidate = parent / fs::path(std::wstring(
                                            name.begin(), name.end()));
    std::error_code ec;
    const bool created = fs::create_directory(candidate, ec);
    if (ec) {
      return Fail(error, "cannot create run directory " + Narrow(candidate) +
                             " (" + ec.message() + ")");
    }
    if (!created) {
      continue;
    }

    bool exists = false;
    DWORD attributes = 0;
    if (!QueryAttributes(candidate, &exists, &attributes, error)) {
      return false;
    }
    if (!exists || !IsDirectory(attributes) || IsReparse(attributes)) {
      return Fail(error, "created run directory is not a plain directory: " +
                             Narrow(candidate));
    }
    fs::path canonical;
    if (!CanonicalExisting(candidate, &canonical, error)) {
      return false;
    }
    if (!SameComponent(canonical.parent_path(), parent) ||
        !SameComponent(canonical.filename(), candidate.filename())) {
      return Fail(error, "created run directory is not a direct child of the "
                         "verified parent: " +
                             Narrow(candidate));
    }

    LabRunDirectories result;
    result.run_dir = canonical;
    const std::array<std::pair<const wchar_t*, fs::path*>, 3> leaves = {{
        {L"shared", &result.shared_dir},
        {L"user", &result.user_dir},
        {L"log", &result.log_dir},
    }};
    for (const auto& leaf : leaves) {
      const fs::path directory = result.run_dir / leaf.first;
      std::error_code leaf_ec;
      const bool leaf_created = fs::create_directory(directory, leaf_ec);
      if (leaf_ec || !leaf_created) {
        return Fail(error, "cannot create a fresh run subdirectory: " +
                               Narrow(directory) +
                               (leaf_ec ? " (" + leaf_ec.message() + ")" : ""));
      }
      bool leaf_exists = false;
      DWORD leaf_attributes = 0;
      if (!QueryAttributes(directory, &leaf_exists, &leaf_attributes, error)) {
        return false;
      }
      if (!leaf_exists || !IsDirectory(leaf_attributes) ||
          IsReparse(leaf_attributes)) {
        return Fail(error, "run subdirectory is not a plain directory: " +
                               Narrow(directory));
      }
      *leaf.second = directory;
    }
    *directories = result;
    return true;
  }
  return Fail(error, "all " + std::to_string(max_attempts) +
                         " run directory names already exist; refusing to reuse "
                         "or delete an existing directory");
}

}  // namespace

std::string FreshLabRunStem() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc = {};
  ::gmtime_s(&utc, &seconds);
  char stamp[32] = {};
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02dT%02d%02d%02dZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec);
  return "engine-executor-run-" + std::string(stamp) + "-p" +
         std::to_string(::GetCurrentProcessId());
}

bool PrepareFreshLabRunRoot(const fs::path& trusted_root,
                            const fs::path& run_parent,
                            const std::string& run_stem,
                            unsigned max_attempts,
                            LabRunDirectories* directories,
                            std::string* error) {
  if (directories == nullptr) {
    return Fail(error, "run directory output is missing");
  }
  if (!SafeRunStem(run_stem)) {
    return Fail(error, "run directory stem is empty or contains a path "
                       "separator or a reserved character");
  }
  if (max_attempts == 0) {
    return Fail(error, "run directory attempt budget is zero");
  }
  if (run_parent.empty() || !run_parent.is_absolute()) {
    return Fail(error, "run parent must be an absolute path");
  }

  fs::path trusted;
  if (!ResolveTrustedRoot(trusted_root, &trusted, error)) {
    return false;
  }

  // Lexical normalization decides which path we validate and create. Symbolic and
  // junction resolution is still checked by canonicalization below, so a normalized
  // path can never be accepted outside the canonical trusted root.
  const fs::path requested = run_parent.lexically_normal();
  fs::path cursor = requested;
  std::vector<fs::path> missing;
  for (;;) {
    bool exists = false;
    DWORD attributes = 0;
    if (!QueryAttributes(cursor, &exists, &attributes, error)) {
      return false;
    }
    if (exists) {
      break;
    }
    const fs::path parent = cursor.parent_path();
    if (parent.empty() || parent == cursor) {
      return Fail(error, "run parent has no existing ancestor: " +
                             Narrow(requested));
    }
    const fs::path component = cursor.filename();
    if (!SafePathComponent(component)) {
      return Fail(error, "run parent contains an unsafe path component: " +
                             Narrow(component));
    }
    missing.push_back(component);
    cursor = parent;
  }

  DWORD attributes = 0;
  bool exists = false;
  if (!QueryAttributes(cursor, &exists, &attributes, error)) {
    return false;
  }
  if (!exists || !IsDirectory(attributes)) {
    return Fail(error, "run parent has a non-directory ancestor: " +
                           Narrow(cursor));
  }
  fs::path canonical_cursor;
  if (!CanonicalExisting(cursor, &canonical_cursor, error)) {
    return false;
  }
  if (!SameOrDescendant(trusted, canonical_cursor)) {
    return Fail(error, "run parent resolves outside the trusted run root: " +
                           Narrow(canonical_cursor));
  }

  for (auto component = missing.rbegin(); component != missing.rend();
       ++component) {
    if (!SafePathComponent(*component)) {
      return Fail(error, "run parent contains an unsafe path component: " +
                             Narrow(*component));
    }
    const fs::path next = canonical_cursor / *component;
    std::error_code ec;
    fs::create_directory(next, ec);
    if (ec) {
      return Fail(error, "cannot create run parent " + Narrow(next) + " (" +
                             ec.message() + ")");
    }
    bool next_exists = false;
    DWORD next_attributes = 0;
    if (!QueryAttributes(next, &next_exists, &next_attributes, error)) {
      return false;
    }
    if (!next_exists || !IsDirectory(next_attributes) ||
        IsReparse(next_attributes)) {
      return Fail(error, "run parent component is not a plain directory: " +
                             Narrow(next));
    }
    fs::path canonical_next;
    if (!CanonicalExisting(next, &canonical_next, error)) {
      return false;
    }
    if (!SameComponent(canonical_next.parent_path(), canonical_cursor) ||
        !SameComponent(canonical_next.filename(), *component)) {
      return Fail(error, "run parent component is not a direct child of its "
                         "verified parent: " +
                             Narrow(next));
    }
    if (!SameOrDescendant(trusted, canonical_next)) {
      return Fail(error, "run parent resolves outside the trusted run root: " +
                             Narrow(canonical_next));
    }
    canonical_cursor = canonical_next;
  }

  if (!SameOrDescendant(trusted, canonical_cursor)) {
    return Fail(error, "run parent resolves outside the trusted run root: " +
                           Narrow(canonical_cursor));
  }
  return CreateFreshRunDirectory(canonical_cursor, run_stem, max_attempts,
                                 directories, error);
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
