// Bounded host-confirmed context. Text is transient and never enters diagnostics.
#ifndef EPOCANVA_TSF_PRODUCT_HOST_CONTEXT_H_
#define EPOCANVA_TSF_PRODUCT_HOST_CONTEXT_H_
#include <windows.h>
#include <msctf.h>
#include <memory>
#include <string>
#include "target_revalidation.h"
namespace epocanva::tsf::product {
struct HostContextSnapshot {
  bool valid = false;
  LONG caret_utf16 = -1;  // ACP in UTF-16 code units
  LONG token_start_utf16 = -1;
  std::string preceding_utf8;  // at most 512 UTF-16 code units converted strictly
  std::string token_utf8;
  std::u16string token_utf16;
};
// Synchronous read only. A refused/pending lock produces no usable value. Both permission
// and target are checked inside the actual cookie before reading at most 513 UTF-16 units.
HostContextSnapshot ReadHostContext(ITfContext* context, TfClientId client,
    LONG expected_caret_utf16, const std::shared_ptr<context::TargetWriteGuard>& guard,
    std::uint64_t context_id, std::uint64_t generation);
}
#endif
