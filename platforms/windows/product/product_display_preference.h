// EpoCanva W3-B -- process-wide candidate display preference.
//
// The settings executable writes `candidate_display=host|always_self`. The TSF DLL must
// know it before presenting a menu, but reading the INI synchronously on the activating
// STA perturbs timing-sensitive input state. The engine-start transaction (which already
// runs on the bootstrap thread and already reads settings.ini for the deployment digest)
// parses it there and publishes one process-wide atomic. The STA only reads the atomic.
//
// The value applies to the next engine start: each Prepare refreshes it; menus for that
// engine read it when they update.
#ifndef EPOCANVA_PRODUCT_DISPLAY_PREFERENCE_H_
#define EPOCANVA_PRODUCT_DISPLAY_PREFERENCE_H_

#include <atomic>

namespace epocanva {
namespace product {

inline std::atomic<bool>& CandidateDisplayAlwaysSelfFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}

inline void SetCandidateDisplayAlwaysSelfForProcess(bool always_self) {
  CandidateDisplayAlwaysSelfFlag().store(always_self, std::memory_order_relaxed);
}

inline bool CandidateDisplayAlwaysSelfForProcess() {
  return CandidateDisplayAlwaysSelfFlag().load(std::memory_order_relaxed);
}

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_DISPLAY_PREFERENCE_H_
