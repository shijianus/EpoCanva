// EpoCanva W2-B -- the one menu-key admission predicate shared by OnTestKeyDown's
// prediction and OnKeyDown's actual handling.
//
// WHY ONE PREDICATE. The product eats a key exactly when the menu owns it. Prediction
// (ITfKeyEventSink::OnTestKeyDown) and handling (OnKeyDown) previously duplicated that
// decision and drifted: modified Enter/Escape were swallowed by both paths even though
// key_decision.h's modifier policy reserves every Shift+special-key combination for the
// host. This header is the single decision both call sites share, so a prediction that
// says "eaten" can never disagree with what OnKeyDown actually does.
//
// THE RULES. Any Control/Alt/Windows/Shift modifier passes the key through: chords belong
// to the host, and Shift+Enter/Escape is kUnsupportedModifierCombination under the
// documented policy (platforms/windows/input/key_decision.h). Without modifiers, an open,
// active menu owns Up/Down/Escape; Enter is owned only when an explicit selection exists
// -- Enter with no selection has always stayed with the host (the literal row is never
// implied). An inactive menu owns nothing.
//
// UNITS. Virtual-key codes are Win32 VK_* values; no text, offsets or code units here.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_MENU_KEY_ADMISSION_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_MENU_KEY_ADMISSION_H_

#include <cstdint>

#include "../input/key_event.h"

namespace epocanva {
namespace tsf {
namespace product {

enum class MenuKeyRoute {
  kPassThrough = 0,   // the host keeps the key; the menu is untouched
  kNavigateUp,        // the menu moves the explicit selection up one row
  kNavigateDown,      // the menu moves the explicit selection down one row
  kAcceptSelection,   // the menu submits the explicit selection
  kDismiss,           // the menu closes; no text change
};

// Pure function: no state, no side effects, no TSF calls. `menu_active` is the caller's
// CandidateKeysActive() && CandidatesOpen() result; `has_selection` is whether an
// explicit row is currently selected. Menu keys are Win32 virtual-key codes.
inline MenuKeyRoute DecideMenuKey(std::uint32_t virtual_key,
                                  const epocanva::windows::input::ModifierSnapshot& modifiers,
                                  bool menu_active, bool has_selection) {
  if (modifiers.control || modifiers.alt || modifiers.windows || modifiers.shift) {
    return MenuKeyRoute::kPassThrough;
  }
  if (!menu_active) {
    return MenuKeyRoute::kPassThrough;
  }
  switch (virtual_key) {
    case 0x26:  // VK_UP
      return MenuKeyRoute::kNavigateUp;
    case 0x28:  // VK_DOWN
      return MenuKeyRoute::kNavigateDown;
    case 0x0D:  // VK_RETURN
      return has_selection ? MenuKeyRoute::kAcceptSelection : MenuKeyRoute::kPassThrough;
    case 0x1B:  // VK_ESCAPE
      return MenuKeyRoute::kDismiss;
    default:
      return MenuKeyRoute::kPassThrough;
  }
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_MENU_KEY_ADMISSION_H_
