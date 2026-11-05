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
// The current token cannot merge committed Han/Kana/Hangul text with the Latin
// pinyin/romaji the user is typing: "你好shi" is context "你好" plus token "shi".
// Han, Kana and Hangul stay one class so mixed-script Japanese words ("食べる")
// remain one token.
enum class TokenClass { kNone, kLatin, kCjk, kMark, kOther };
TokenClass ScalarClass(unsigned int scalar) {
  if (scalar >= 0x1100 && scalar <= 0x11ff) return TokenClass::kCjk;
  if (scalar >= 0x3040 && scalar <= 0x31ff) return TokenClass::kCjk;
  if (scalar >= 0x3400 && scalar <= 0x4dbf) return TokenClass::kCjk;
  if (scalar >= 0x4e00 && scalar <= 0x9fff) return TokenClass::kCjk;
  if (scalar >= 0xac00 && scalar <= 0xd7af) return TokenClass::kCjk;
  if (scalar >= 0xf900 && scalar <= 0xfaff) return TokenClass::kCjk;
  if (scalar >= 0x20000 && scalar <= 0x3ffff) return TokenClass::kCjk;
  if (scalar >= 0xff66 && scalar <= 0xff9d) return TokenClass::kCjk;
  if (scalar > 0xffff) return TokenClass::kLatin;  // surrogate pair in another plane
  const wchar_t unit = static_cast<wchar_t>(scalar);
  if (!TokenUnit(unit)) return TokenClass::kOther;
  WORD type3 = 0;
  if (!::GetStringTypeW(CT_CTYPE3, &unit, 1, &type3)) return TokenClass::kOther;
  if (type3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) return TokenClass::kMark;
  return TokenClass::kLatin;
}
std::size_t TokenStartUtf16(const std::wstring& text) {
  std::size_t begin_utf16 = text.size();
  TokenClass token_class = TokenClass::kNone;
  while (begin_utf16) {
    std::size_t width_utf16 = 1;
    unsigned int scalar = static_cast<unsigned int>(text[begin_utf16 - 1]);
    if (scalar >= 0xdc00 && scalar <= 0xdfff) {
      if (begin_utf16 < 2) break;
      const auto high = static_cast<unsigned int>(text[begin_utf16 - 2]);
      if (high < 0xd800 || high > 0xdbff) break;
      scalar = 0x10000 + ((high - 0xd800) << 10) + (scalar - 0xdc00);
      width_utf16 = 2;
    } else if (scalar >= 0xd800 && scalar <= 0xdbff) {
      break;
    }
    const TokenClass unit_class = ScalarClass(scalar);
    if (unit_class == TokenClass::kMark) {
      begin_utf16 -= width_utf16;
      continue;
    }
    if (unit_class == TokenClass::kOther) break;
    if (token_class == TokenClass::kNone) token_class = unit_class;
    else if (token_class != unit_class) break;
    begin_utf16 -= width_utf16;
  }
  return begin_utf16;
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
    const std::size_t token_begin_utf16 = TokenStartUtf16(text);
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
