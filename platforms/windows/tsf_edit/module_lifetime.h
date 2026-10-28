// EpoCanva W2-B-R -- module accounting for COM objects the TSF manager keeps.
//
// WHY THIS EXISTS. Several objects this layer hands to TSF (ITfEditSession callbacks,
// ITfCompositionSink) may outlive the activation that created them: TSF owns a reference
// until it grants or cancels the session, and the text service cannot cancel that. The DLL
// must therefore keep itself loaded while any such object is alive, or TSF will later call
// Release on a vtable that no longer exists.
//
// HEADER-ONLY ON PURPOSE. The state is per linked image (inline static), so the product DLL
// counts its own objects and the standalone edit/context tests have their own inert copy
// without any extra source file or link dependency. The product DLL installs the counters in
// ProductTextController::Start; nothing else sets them.
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_MODULE_LIFETIME_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_MODULE_LIFETIME_H_

#include <atomic>

namespace epocanva {
namespace tsf {
namespace edit {

using ModuleCountFunction = long (*)();

// Per-kind counts make an unload refusal explainable (which TSF-owned object is still alive)
// without logging any user data.
enum class ModuleObjectKind : int {
  kEditSession = 0,
  kSelectionRead = 1,
  kCompositionSink = 2,
  kRectRead = 3,
  kCandidateElement = 4,
  kCandidateSink = 5,
  kKindCount = 6,
};

inline std::atomic<ModuleCountFunction>& ModuleAddSlot() {
  static std::atomic<ModuleCountFunction> slot{nullptr};
  return slot;
}
inline std::atomic<ModuleCountFunction>& ModuleReleaseSlot() {
  static std::atomic<ModuleCountFunction> slot{nullptr};
  return slot;
}
inline std::atomic<long>& LiveGuardedSlot() {
  static std::atomic<long> live{0};
  return live;
}
inline std::atomic<long>& LiveKindSlot(ModuleObjectKind kind) {
  static std::atomic<long> slots[static_cast<int>(ModuleObjectKind::kKindCount)];
  return slots[static_cast<int>(kind)];
}

inline void SetModuleCounters(ModuleCountFunction add, ModuleCountFunction release) {
  ModuleAddSlot().store(add);
  ModuleReleaseSlot().store(release);
}

// Counts one live COM object for exactly the lifetime of the owning C++ object.
class ModuleObjectGuard {
 public:
  explicit ModuleObjectGuard(ModuleObjectKind kind) : kind_(kind) {
    const ModuleCountFunction add = ModuleAddSlot().load();
    if (add != nullptr) {
      add();
      counted_ = true;
      LiveGuardedSlot().fetch_add(1);
      LiveKindSlot(kind).fetch_add(1);
    }
  }
  ~ModuleObjectGuard() {
    if (!counted_) {
      return;
    }
    LiveKindSlot(kind_).fetch_sub(1);
    LiveGuardedSlot().fetch_sub(1);
    const ModuleCountFunction release = ModuleReleaseSlot().load();
    if (release != nullptr) {
      release();
    }
  }
  ModuleObjectGuard(const ModuleObjectGuard&) = delete;
  ModuleObjectGuard& operator=(const ModuleObjectGuard&) = delete;

 private:
  ModuleObjectKind kind_;
  bool counted_ = false;
};

// Test aid: the number of guards constructed minus destroyed. Not used by the product.
inline long LiveGuardedObjects() { return LiveGuardedSlot().load(); }
inline long LiveGuardedObjects(ModuleObjectKind kind) {
  return LiveKindSlot(kind).load();
}

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_MODULE_LIFETIME_H_
