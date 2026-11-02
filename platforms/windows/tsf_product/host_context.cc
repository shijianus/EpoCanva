#include "host_context.h"
#include <algorithm>
#include <atomic>
#include <utility>
#include "module_lifetime.h"
#include "permission_query.h"
#include "text_encoding.h"
namespace epocanva::tsf::product {
namespace {
// Unicode category is used only to find a bounded token range. No normalization, case
// conversion or guessed transliteration is applied to the host's UTF-16 text.
bool TokenUnit(wchar_t unit) {
  if (unit >= 0xd800 && unit <= 0xdfff) return true;
  if (unit == L'\'' || unit == 0x2019 || unit == L'_' || unit == L'-') return true;
  WORD type1 = 0, type3 = 0;
  if (!::GetStringTypeW(CT_CTYPE1, &unit, 1, &type1) ||
      !::GetStringTypeW(CT_CTYPE3, &unit, 1, &type3)) return false;
  return (type1 & (C1_ALPHA | C1_DIGIT)) != 0 ||
      (type3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0;
}
class Reader final : public ITfEditSession {
 public:
  Reader(ITfContext* context, LONG caret, std::shared_ptr<context::TargetWriteGuard> guard,
         std::uint64_t id, std::uint64_t generation)
      : context_(context), caret_(caret), guard_(std::move(guard)), id_(id), generation_(generation) {
    context_->AddRef();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_INVALIDARG; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto left = --refs_; if (!left) delete this; return static_cast<ULONG>(left);
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    result = {};
    if (!guard_ || !guard_->StillCurrent(context_, id_, generation_) ||
        context::DecidePermission(context::ReadPermissionFactsWithCookie(context_, cookie)).state !=
            context::PermissionState::kAllowed) return S_OK;
    TF_SELECTION selection{}; ULONG fetched = 0;
    if (FAILED(context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
        fetched != 1 || !selection.range) { if (selection.range) selection.range->Release(); return S_OK; }
    ITfRangeACP* acp = nullptr;
    if (FAILED(selection.range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp))) || !acp) {
      selection.range->Release(); return S_OK;
    }
    LONG caret = -1, units = -1;
    bool ok = SUCCEEDED(acp->GetExtent(&caret, &units)) && caret == caret_ && units == 0 && caret >= 0 &&
        !selection.style.fInterimChar;
    const LONG begin = (std::max)(0L, caret - 513L);
    const LONG wanted_units_utf16 = caret - begin;
    if (ok) ok = SUCCEEDED(acp->SetExtent(begin, wanted_units_utf16));
    acp->Release();
    std::wstring text(static_cast<std::size_t>((std::max)(0L, wanted_units_utf16)), L'\0');
    ULONG copied_units_utf16 = 0;
    if (ok && wanted_units_utf16 > 0) ok = SUCCEEDED(selection.range->GetText(cookie, 0,
        text.data(), static_cast<ULONG>(wanted_units_utf16), &copied_units_utf16)) &&
        copied_units_utf16 == static_cast<ULONG>(wanted_units_utf16);
    selection.range->Release();
    if (!ok || !guard_->StillCurrent(context_, id_, generation_)) return S_OK;
    LONG actual_begin = begin;
    // The bound can begin inside a surrogate pair. Discard the incomplete first scalar;
    // never replace it, and never infer a complete token from a truncated suffix.
    if (!text.empty() && text.front() >= 0xdc00 && text.front() <= 0xdfff) {
      text.erase(0, 1); ++actual_begin;
    }
    std::size_t token_begin_utf16 = text.size();
    while (token_begin_utf16 && TokenUnit(text[token_begin_utf16 - 1])) --token_begin_utf16;
    if (token_begin_utf16 == 0 && actual_begin != 0) return S_OK;
    if (text.size() > 512 && token_begin_utf16 == 0) return S_OK;
    auto preceding = text.substr(0, token_begin_utf16);
    if (preceding.size() > 512) preceding.erase(0, preceding.size() - 512);
    if (!preceding.empty() && preceding.front() >= 0xdc00 && preceding.front() <= 0xdfff)
      preceding.erase(0, 1);
    const auto token = text.substr(token_begin_utf16);
    const auto left = windows::text::ConvertUtf16CodeUnitsToUtf8Bytes(preceding);
    const auto current = windows::text::ConvertUtf16CodeUnitsToUtf8Bytes(token);
    if (!left.ok() || !current.ok()) return S_OK;
    result.valid = true; result.caret_utf16 = caret;
    result.token_start_utf16 = actual_begin + static_cast<LONG>(token_begin_utf16);
    result.preceding_utf8 = left.utf8_bytes; result.token_utf8 = current.utf8_bytes;
    result.token_utf16.assign(token.begin(), token.end());
    return S_OK;
  }
  HostContextSnapshot result;
 private:
  ~Reader() { context_->Release(); }
  std::atomic<LONG> refs_{1}; ITfContext* context_; LONG caret_;
  std::shared_ptr<context::TargetWriteGuard> guard_; std::uint64_t id_, generation_;
  edit::ModuleObjectGuard module_guard_{edit::ModuleObjectKind::kRectRead};
};
}
HostContextSnapshot ReadHostContext(ITfContext* context, TfClientId client,
    LONG caret, const std::shared_ptr<context::TargetWriteGuard>& guard,
    std::uint64_t id, std::uint64_t generation) {
  if (!context || !client || caret < 0 || !guard) return {};
  auto* reader = new Reader(context, caret, guard, id, generation);
  HRESULT session_hr = E_FAIL;
  const auto hr = context->RequestEditSession(client, reader, TF_ES_SYNC | TF_ES_READ, &session_hr);
  auto result = SUCCEEDED(hr) && session_hr == S_OK ? std::move(reader->result) : HostContextSnapshot{};
  reader->Release(); return result;
}
}
