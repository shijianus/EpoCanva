#ifndef EPOCANVA_PRODUCT_LEARNING_H_
#define EPOCANVA_PRODUCT_LEARNING_H_
#include "product_paths.h"

#include <string>

namespace epocanva {
namespace product {

// True when the per-user learning directory contains any durable artefact. Empty locks
// do not count: they are created by normal store access but hold no preference data.
bool LocalLearningDataPresent(const ProductPaths& paths);

// Explicit user action. Removes every profile's learning file, pre-v3 backup, temporary
// stage and .corrupt quarantine. Never depends on a focused field, a Normal field or the
// recording toggle. Returns false with a user-readable reason on failure.
bool ClearAllLocalLearning(const ProductPaths& paths, std::wstring* why);

}  // namespace product
}  // namespace epocanva
#endif
