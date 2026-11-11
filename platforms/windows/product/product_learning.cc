#include "product_learning.h"

#include <filesystem>
#include <system_error>

#include "engine/data_store/windows_learning_repository.h"
#include "product_io.h"

namespace epocanva {
namespace product {
namespace {

bool LearningArtefact(const std::wstring& name) {
  if (name.size() < 2 || name.compare(0, 2, L"n-") != 0) return false;
  const bool data = name.find(L".dat") != std::wstring::npos;
  const bool temporary = name.find(L".tmp") != std::wstring::npos;
  const bool backup = name.find(L".v2") != std::wstring::npos;
  const bool corrupt = name.find(L".corrupt") != std::wstring::npos;
  return data || temporary || backup || corrupt;
}

}  // namespace

bool LocalLearningDataPresent(const ProductPaths& paths) {
  std::error_code error;
  if (!std::filesystem::is_directory(paths.learning_dir, error) || error) return false;
  for (const auto& entry : std::filesystem::directory_iterator(paths.learning_dir, error)) {
    if (error) break;
    if (entry.is_regular_file(error) && !error &&
        LearningArtefact(entry.path().filename().wstring())) return true;
  }
  return false;
}

bool ClearAllLocalLearning(const ProductPaths& paths, std::wstring* why) {
  if (paths.user_root.empty()) {
    if (why != nullptr) *why = L"the user data path is unavailable";
    return false;
  }
  try {
    learning::WindowsLearningRepository repository(paths.user_root);
    const auto status = repository.DeleteAll(true);
    if (status == learning::Status::kOk) return true;
    if (why != nullptr) {
      *why = status == learning::Status::kBusy
          ? L"the learning store is in use; close the application and retry"
          : L"the learning store could not be cleared (" +
                Utf8ToWide(std::string(learning::StatusName(status))) + L")";
    }
    return false;
  } catch (...) {
    if (why != nullptr) *why = L"the learning store could not be cleared";
    return false;
  }
}

}  // namespace product
}  // namespace epocanva
