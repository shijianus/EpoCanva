#ifndef EPOCANVA_PRODUCT_BACKEND_RESOURCES_H_
#define EPOCANVA_PRODUCT_BACKEND_RESOURCES_H_
#include "product_paths.h"
#include "../engine_executor/backend_config.h"
namespace epocanva::product {
// Fixed local manifest; no resource discovery or path search. Text is configuration only.
bool LoadBackendResources(const ProductPaths& paths,
    windows::engine::UnifiedBackendConfig* config, std::wstring* why);
}
#endif
