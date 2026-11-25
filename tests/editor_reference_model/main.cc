// Independent scalar-sequence oracle versus the production TSF edit executor.
// All public document offsets, selections and extents are UTF-16 code units.
// Application delete/undo use real host ITfEditSession callbacks. They do not
// claim installed application undo semantics, IME activation or host acceptance.
// Replay files contain only generated operation IDs/operands, never user text.
#include <windows.h>
#include <msctf.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "edit_plan.h"
#include "edit_session.h"
#include "test_host.h"

namespace {
using namespace epocanva::tsf::edit;
using epocanva::tsf::edit::test::TestDocument;
using epocanva::tsf::edit::test::TestDocumentOptions;
using epocanva::tsf::edit::test::TestHost;

constexpr std::uint64_t kSeeds[] = {0x5eed000000000001ULL, 0x5eed000000000002ULL,
    0x5eed000000000003ULL, 0x5eed000000000004ULL, 0x5eed000000000005ULL};
constexpr const char* kNames[] = {"insert", "delete", "caret-utf16", "selection-utf16",
                                 "undo", "invalid-state"};
const std::array<std::u32string, 9> kText = {U"a", U"\u4e2d", U"\u00e9", U"\u3042",
    U"\ud55c", U"\U0001f642", U"e\u0301", U"\U0001f469\u200d\U0001f4bb", U"ab"};

void Require(bool value, const char* what) {
  if (!value) throw std::runtime_error(what);
}

LONG Width(char32_t scalar) { return scalar > 0xffff ? 2 : 1; }
LONG Units(const std::u32string& text) {
  LONG count_utf16 = 0;
  for (auto scalar : text) count_utf16 += Width(scalar);
  return count_utf16;
}
std::wstring Encode16(const std::u32string& text) {
  std::wstring encoded;
  for (auto scalar : text) {
    if (scalar > 0xffff) {
      scalar -= 0x10000;
      encoded.push_back(static_cast<wchar_t>(0xd800 + (scalar >> 10)));
      encoded.push_back(static_cast<wchar_t>(0xdc00 + (scalar & 1023)));
    } else encoded.push_back(static_cast<wchar_t>(scalar));
  }
  return encoded;
}
std::string Encode8(const std::u32string& text) {
  std::string encoded;
  for (auto scalar : text) {
    if (scalar < 0x80) encoded.push_back(static_cast<char>(scalar));
    else if (scalar < 0x800) {
      encoded.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
      encoded.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else if (scalar < 0x10000) {
      encoded.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
      encoded.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
      encoded.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else {
      encoded.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
      encoded.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 63)));
      encoded.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
      encoded.push_back(static_cast<char>(0x80 | (scalar & 63)));
    }
  }
  return encoded;
}

// The oracle stores Unicode scalars, not the implementation's UTF-16 string.
// Only this boundary traversal translates the protocol's UTF-16 offsets.
struct Oracle {
  std::u32string text = U"\U0001f642";
  LONG start_utf16 = 2, end_utf16 = 2;
  LONG composition_start_utf16 = -1, composition_extent_utf16 = 0;
  auto At(LONG offset_utf16) {
    LONG traversed_utf16 = 0;
    auto scalar = text.begin();
    while (scalar != text.end() && traversed_utf16 < offset_utf16) {
      traversed_utf16 += Width(*scalar++);
    }
    Require(traversed_utf16 == offset_utf16, "oracle nonboundary UTF-16 offset");
    return scalar;
  }
  void Replace(LONG first_utf16, LONG last_utf16, const std::u32string& replacement) {
    auto first = At(first_utf16), last = At(last_utf16);
    auto next = text.erase(first, last);
    text.insert(next, replacement.begin(), replacement.end());
    start_utf16 = end_utf16 = first_utf16 + Units(replacement);
  }
  std::vector<LONG> Boundaries() const {
    std::vector<LONG> result{0};
    LONG offset_utf16 = 0;
    for (auto scalar : text) { offset_utf16 += Width(scalar); result.push_back(offset_utf16); }
    return result;
  }
};

// Application-owned edit. This is the host side of delete and undo, not an
// alternative product executor. All writes still traverse msctf.dll's lock.
class HostWrite final : public ITfEditSession {
 public:
  HostWrite(ITfContext* context, LONG first_utf16, LONG last_utf16,
            std::wstring replacement, LONG caret_start_utf16, LONG caret_end_utf16)
      : context_(context), first_utf16_(first_utf16), last_utf16_(last_utf16),
        replacement_(std::move(replacement)), start_utf16_(caret_start_utf16),
        end_utf16_(caret_end_utf16) { context_->AddRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *object = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = --references_; if (!count) delete this; return count;
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    ITfRange* range = nullptr;
    HRESULT hr = context_->GetStart(cookie, &range);
    if (FAILED(hr)) return result = hr;
    ITfRangeACP* acp = nullptr;
    hr = range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
    if (SUCCEEDED(hr)) hr = acp->SetExtent(first_utf16_, last_utf16_ - first_utf16_);
    if (SUCCEEDED(hr)) hr = range->SetText(cookie, 0, replacement_.data(),
                                           static_cast<LONG>(replacement_.size()));
    if (SUCCEEDED(hr)) hr = acp->SetExtent(start_utf16_, end_utf16_ - start_utf16_);
    if (SUCCEEDED(hr)) {
      TF_SELECTION selection{}; selection.range = range; selection.style.ase = TF_AE_END;
      hr = context_->SetSelection(cookie, 1, &selection);
    }
    if (acp) acp->Release();
    range->Release();
    return result = hr;
  }
  HRESULT result = E_FAIL;
 private:
  ~HostWrite() { context_->Release(); }
  std::atomic<ULONG> references_{1};
  ITfContext* context_;
  LONG first_utf16_, last_utf16_;
  std::wstring replacement_;
  LONG start_utf16_, end_utf16_;
};

struct Operation { unsigned kind, a, b, value; };
struct HostSnapshot { std::wstring text; LONG start_utf16, end_utf16; };
struct ResourceSample {
  std::size_t completed_operations;
  std::uint64_t elapsed_ms;
  DWORD handles;
  SIZE_T working_set_bytes, private_bytes, peak_working_set_bytes;
};
struct RunResult {
  std::string mismatch;
  std::size_t operations = 0;
  std::array<std::size_t, 6> counts{};
  std::uint64_t elapsed_ms = 0;
  std::vector<ResourceSample> resource_samples;
  std::uint64_t stores_alive_after_shutdown = 0, store_sinks_alive_after_shutdown = 0;
};
ResourceSample Sample(std::size_t completed_operations, ULONGLONG started_ms) {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  DWORD handles = 0;
  Require(GetProcessMemoryInfo(GetCurrentProcess(),
              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE,
          "memory observation failed");
  Require(GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE, "handle observation failed");
  return {completed_operations, GetTickCount64() - started_ms, handles,
          counters.WorkingSetSize, counters.PrivateUsage, counters.PeakWorkingSetSize};
}

RunResult Run(const std::vector<Operation>& operations) {
  RunResult result;
  result.resource_samples.reserve(12);
  const ULONGLONG started_ms = GetTickCount64();
  result.resource_samples.push_back(Sample(0, started_ms));
  {
    std::string error;
    auto host = TestHost::Create(&error);
    Require(host != nullptr, "TSF host creation failed");
    TestDocumentOptions options;
    options.initial_text_utf16 = L"\U0001f642";
    options.initial_selection_start_utf16 = options.initial_selection_end_utf16 = 2;
    TestDocument* document = host->CreateDocument(options, &error);
    Require(document != nullptr, "TSF document creation failed");
    CompositionEditSession session(host->client_id());
    Require(session.Bind(document->context(), document->context_id(), 1), "session bind failed");
    session.SetInputPermission(InputPermission::kAllowed);
    Oracle model;
    std::vector<Oracle> model_undo;
    std::vector<HostSnapshot> host_undo;
    auto Capture = [&]() {
      model_undo.push_back(model);
      host_undo.push_back({document->text_utf16(), document->selection_start_utf16(),
                           document->selection_end_utf16()});
      // A fixed bounded host undo history, shared only as a protocol limit.
      if (model_undo.size() > 64) { model_undo.erase(model_undo.begin()); host_undo.erase(host_undo.begin()); }
    };
    auto Apply = [&](EditPlanBuildResult plan) {
      Require(plan.ok(), "valid edit plan rejected");
      const EditResult applied = session.ApplySync(*plan.plan);
      Require(applied.status == EditStatus::kApplied, "TSF edit did not apply");
    };
    auto Retire = [&]() {
      if (model.composition_start_utf16 >= 0) {
        Apply(EditPlan::BuildEndOwnedComposition(session.identity()));
        model.composition_start_utf16 = -1; model.composition_extent_utf16 = 0;
      }
    };
    auto WriteHost = [&](LONG first_utf16, LONG last_utf16, const std::wstring& text,
                         LONG start_utf16, LONG end_utf16) {
      auto* callback = new HostWrite(document->context(), first_utf16, last_utf16,
                                     text, start_utf16, end_utf16);
      HRESULT session_hr = E_FAIL;
      const HRESULT request = document->context()->RequestEditSession(host->client_id(),
          callback, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
      const HRESULT applied = callback->result;
      callback->Release();
      Require(SUCCEEDED(request) && SUCCEEDED(session_hr) && SUCCEEDED(applied), "host edit failed");
    };
    for (const auto& operation : operations) {
      const unsigned kind = operation.kind == 6 ? 0 : operation.kind;
      Require(kind < 6, "invalid replay operation");
      ++result.operations; ++result.counts[kind];
      const auto& text = kText[operation.value % kText.size()];
      if (kind == 0) {
        if (model.start_utf16 != model.end_utf16) {
          // Product contract currently refuses selection replacement; prove the
          // refusal leaves both document and selection intact, never collapse it.
          const auto begin = EditPlan::BuildBegin(session.identity(), Encode8(text),
                                                  model.start_utf16, model.end_utf16);
          const auto commit = EditPlan::BuildCommit(session.identity(), Encode8(text),
                                                    model.start_utf16, model.end_utf16);
          Require(begin.status == EditPlanStatus::kSelectionNotCollapsed &&
                      commit.status == EditPlanStatus::kSelectionNotCollapsed, "noncollapsed insertion accepted");
          // A once-collapsed plan must not write over a selection that changed
          // in the host. Exercise the executor as well as the plan constructor.
          auto stale_selection = EditPlan::BuildCommit(session.identity(), Encode8(text),
                                                       model.start_utf16, model.start_utf16);
          Require(stale_selection.ok(), "collapsed selection fixture failed");
          Require(session.ApplySync(*stale_selection.plan).status == EditStatus::kRejected,
                  "host noncollapsed selection was overwritten");
        } else {
          Capture();
          const bool composing = model.composition_start_utf16 >= 0;
          const LONG first_utf16 = composing ? model.composition_start_utf16 : model.start_utf16;
          const LONG last_utf16 = composing ? first_utf16 + model.composition_extent_utf16 : first_utf16;
          const bool commit = operation.a % 3 == 0;
          auto plan = commit ? EditPlan::BuildCommit(session.identity(), Encode8(text), model.start_utf16, model.end_utf16)
              : composing ? EditPlan::BuildUpdate(session.identity(), Encode8(text))
                          : EditPlan::BuildBegin(session.identity(), Encode8(text), model.start_utf16, model.end_utf16);
          if (operation.kind != 6) Apply(std::move(plan)); // test-only omitted-write mutant
          model.Replace(first_utf16, last_utf16, text);
          model.composition_start_utf16 = commit ? -1 : first_utf16;
          model.composition_extent_utf16 = commit ? 0 : Units(text);
        }
      } else if (kind == 1) {
        Capture();
        if (model.composition_start_utf16 >= 0) {
          Apply(EditPlan::BuildCancel(session.identity()));
          model.Replace(model.composition_start_utf16,
                        model.composition_start_utf16 + model.composition_extent_utf16, U"");
          model.composition_start_utf16 = -1; model.composition_extent_utf16 = 0;
        } else {
          LONG first_utf16 = model.start_utf16;
          if (first_utf16 == model.end_utf16 && first_utf16 > 0) {
            const auto boundaries = model.Boundaries();
            first_utf16 = *std::prev(std::find(boundaries.begin(), boundaries.end(), first_utf16));
          }
          WriteHost(first_utf16, model.end_utf16, L"", first_utf16, first_utf16);
          model.Replace(first_utf16, model.end_utf16, U"");
        }
      } else if (kind == 2 || kind == 3) {
        Retire();
        const auto boundaries = model.Boundaries();
        const LONG first_utf16 = boundaries[operation.a % boundaries.size()];
        const LONG last_utf16 = kind == 2 ? first_utf16 : boundaries[operation.b % boundaries.size()];
        model.start_utf16 = std::min(first_utf16, last_utf16);
        model.end_utf16 = std::max(first_utf16, last_utf16);
        Require(SUCCEEDED(document->SetSelectionRangeForTest(host->client_id(), model.start_utf16,
                            model.end_utf16)), "selection edit failed");
      } else if (kind == 4) {
        Retire();
        if (!host_undo.empty()) {
          const HostSnapshot snapshot = host_undo.back(); host_undo.pop_back();
          WriteHost(0, document->store()->text_length_utf16(), snapshot.text,
                    snapshot.start_utf16, snapshot.end_utf16);
          model = model_undo.back(); model_undo.pop_back();
          // Host undo restores text/selection; composition ownership is ended.
          model.composition_start_utf16 = -1; model.composition_extent_utf16 = 0;
        }
      } else {
        Retire();
        auto plan = EditPlan::BuildCommit(session.identity(), "x", model.start_utf16, model.start_utf16);
        Require(plan.ok(), "invalid-state fixture failed");
        const unsigned mode = operation.a % 5;
        if (mode == 0) {
          // Overlong, truncated, isolated continuation, encoded surrogate and
          // beyond-U+10FFFF inputs must be refused without repairing raw bytes.
          for (const auto* invalid_utf8 : {"\xc0\xaf", "\xe2\x82", "\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
            Require(EditPlan::BuildCommit(session.identity(), invalid_utf8, 0, 0).status ==
                        EditPlanStatus::kInvalidUtf8, "malformed UTF-8 accepted");
          }
        } else if (mode == 1) {
          session.SetInputPermission(InputPermission::kUnknown);
          const auto rejected = session.ApplySync(*plan.plan);
          Require(rejected.status == EditStatus::kRejected &&
                      rejected.reason == EditRejectReason::kPermissionUnknown, "unknown permission accepted");
          session.SetInputPermission(InputPermission::kAllowed);
        } else if (mode == 4) {
          LONG inside_pair_utf16 = -1, offset_utf16 = 0;
          for (auto scalar : model.text) {
            if (Width(scalar) == 2) { inside_pair_utf16 = offset_utf16 + 1; break; }
            offset_utf16 += Width(scalar);
          }
          if (inside_pair_utf16 >= 0) {
            Require(SUCCEEDED(document->SetSelectionForTest(host->client_id(), inside_pair_utf16)),
                    "surrogate-boundary host fixture failed");
            auto invalid = EditPlan::BuildCommit(session.identity(), "x", inside_pair_utf16, inside_pair_utf16);
            Require(invalid.ok(), "surrogate plan fixture failed");
            const auto rejected = session.ApplySync(*invalid.plan);
            Require(rejected.status == EditStatus::kRejected &&
                        rejected.reason == EditRejectReason::kSelectionInsideSurrogatePair,
                    "surrogate-splitting edit accepted");
            Require(SUCCEEDED(document->SetSelectionRangeForTest(host->client_id(), model.start_utf16, model.end_utf16)),
                    "surrogate-boundary selection restore failed");
          } else {
            Require(!EditPlan::BuildCommit(session.identity(), "x", -1, -1).ok(), "negative UTF-16 offset accepted");
          }
        } else {
          document->store()->SetAsyncLocks(true);
          const auto pending = session.ApplyAsync(*plan.plan);
          Require(pending.status == EditStatus::kPending && session.HasPending(), "async callback not pending");
          if (mode == 2) session.Invalidate();
          else session.SetInputPermission(InputPermission::kDenied);
          Require(host->PumpUntil(session.CompletionEvent(), 5000), "async completion timed out");
          const auto rejected = session.PendingResult();
          Require(mode == 2 ? rejected.status == EditStatus::kStale
                             : rejected.status == EditStatus::kRejected &&
                               rejected.reason == EditRejectReason::kPermissionDenied,
                  "stale/revoked async edit accepted");
          document->store()->SetAsyncLocks(false);
          if (mode == 2) {
            Require(session.Bind(document->context(), document->context_id(),
                                 session.identity().generation + 1), "rebind failed");
            Require(session.ApplySync(*plan.plan).status == EditStatus::kStale,
                    "old generation accepted after rebind");
          }
          session.SetInputPermission(InputPermission::kAllowed);
        }
      }
      if (document->text_utf16() != Encode16(model.text)) result.mismatch = "document";
      else if (document->selection_start_utf16() != model.start_utf16 ||
               document->selection_end_utf16() != model.end_utf16) result.mismatch = "selection";
      else if (session.active_composition() != (model.composition_start_utf16 >= 0)) result.mismatch = "composition";
      else if (model.composition_start_utf16 >= 0) {
        const auto observed = session.Snapshot();
        if (observed.composition_start_utf16 != model.composition_start_utf16 ||
            observed.composition_length_utf16 != model.composition_extent_utf16)
          result.mismatch = "composition-range-utf16";
      }
      if (result.operations % 10000 == 0)
        result.resource_samples.push_back(Sample(result.operations, started_ms));
      if (!result.mismatch.empty()) break;
    }
    session.Close(); host->Shutdown();
    const auto cleanup = host->cleanup_report();
    result.stores_alive_after_shutdown = cleanup.store_instances_alive;
    result.store_sinks_alive_after_shutdown = cleanup.advised_store_sinks_alive;
    Require(cleanup.store_instances_alive == 0 && cleanup.advised_store_sinks_alive == 0,
            "TSF host resource leak");
  }
  result.resource_samples.push_back(Sample(result.operations, started_ms));
  result.elapsed_ms = result.resource_samples.back().elapsed_ms;
  return result;
}

std::vector<Operation> Generate(std::uint64_t seed, std::size_t count) {
  auto Next = [&]() { seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
                     return static_cast<unsigned>((seed * 0x2545f4914f6cdd1dULL) >> 32); };
  std::vector<Operation> operations; operations.reserve(count);
  for (std::size_t operation_count = 0; operation_count < count; ++operation_count) {
    Operation operation{};
    // First six operations guarantee every category, independent of RNG luck.
    operation.kind = operation_count < 6 ? static_cast<unsigned>(operation_count) : Next() % 6;
    operation.a = Next(); operation.b = Next(); operation.value = Next();
    operations.push_back(operation);
  }
  return operations;
}

void WriteOperations(const std::string& path, const std::vector<Operation>& operations) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  for (const auto& operation : operations)
    output << operation.kind << ' ' << operation.a << ' ' << operation.b << ' ' << operation.value << '\n';
  output.flush(); Require(static_cast<bool>(output), "replay write failed");
  output.close(); Require(!output.fail(), "replay close failed");
}
std::vector<Operation> ReadOperations(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "replay read failed");
  std::vector<Operation> result;
  std::string line;
  while (std::getline(input, line)) {
    Operation operation{};
    Require(line.find_first_not_of("0123456789 \t\r") == std::string::npos, "invalid replay characters");
    std::istringstream row(line);
    Require(bool(row >> operation.kind >> operation.a >> operation.b >> operation.value), "truncated replay row");
    row >> std::ws;
    Require(row.eof(), "extra replay operands");
    Require(operation.kind < 7 && result.size() < 100000, "invalid replay operands");
    result.push_back(operation);
  }
  Require(input.eof() && !input.bad() && !result.empty(), "empty or malformed replay");
  return result;
}

// Delta debugging, followed by a deletion sweep to prove one-minimality. Only
// the same observed mismatch is retained; infrastructure exceptions never count.
std::vector<Operation> Minimize(std::vector<Operation> sequence, const std::string& mismatch) {
  std::size_t partitions = 2;
  while (sequence.size() >= 2) {
    const std::size_t chunk_operations = (sequence.size() + partitions - 1) / partitions;
    bool reduced = false;
    for (std::size_t first_operation = 0; first_operation < sequence.size(); first_operation += chunk_operations) {
      auto candidate = sequence;
      const auto last_operation = std::min(first_operation + chunk_operations, candidate.size());
      candidate.erase(candidate.begin() + first_operation, candidate.begin() + last_operation);
      if (Run(candidate).mismatch == mismatch) {
        sequence = std::move(candidate); partitions = 2; reduced = true; break;
      }
    }
    if (!reduced) {
      if (partitions >= sequence.size()) break;
      partitions = std::min(partitions * 2, sequence.size());
    }
  }
  for (std::size_t operation_count = 0; operation_count < sequence.size();) {
    auto candidate = sequence; candidate.erase(candidate.begin() + operation_count);
    if (Run(candidate).mismatch == mismatch) sequence = std::move(candidate);
    else ++operation_count;
  }
  return sequence;
}

void WriteReport(const std::string& path, const std::vector<RunResult>& results,
                 const std::vector<std::uint64_t>& seeds, const char* mode,
                 unsigned malformed_replays_rejected) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(output), "report open failed");
  output << "{\n\"format\":\"epocanva-editor-reference-measurements-v2\",\n"
            "\"oracle\":\"independent-editor-reference-model\",\n"
            "\"scope\":\"controlled-real-tsf-edit-layer\",\n"
            "\"failure_reduction\":\"replay-and-minimize\",\n"
            "\"host_undo\":\"controlled-application-edit-session\",\n"
            "\"selection_replacement\":\"unsupported-rejected\",\n"
            "\"composition_observation\":\"session-range-utf16\",\n"
            "\"installed_host_acceptance\":false,\n\"mode\":\"" << mode << "\",\n\"seeds\":[\n";
  std::size_t total_operations = 0;
  bool passed = true;
  for (std::size_t seed_count = 0; seed_count < results.size(); ++seed_count) {
    const auto& result = results[seed_count];
    total_operations += result.operations; passed = passed && result.mismatch.empty();
    if (seed_count) output << ",\n";
    output << "{\"seed\":\"0x" << std::hex << seeds[seed_count] << std::dec
           << "\",\"operations\":" << result.operations << ",\"mismatches\":"
           << (result.mismatch.empty() ? 0 : 1) << ",\"mismatch\":\"" << result.mismatch
           << "\",\"operation_counts\":{";
    for (unsigned category = 0; category < 6; ++category) {
      if (category) output << ',';
      output << '"' << kNames[category] << "\":" << result.counts[category];
    }
    output << "},\"duration_ms\":" << result.elapsed_ms
           << ",\"stores_alive_after_shutdown\":" << result.stores_alive_after_shutdown
           << ",\"store_sinks_alive_after_shutdown\":" << result.store_sinks_alive_after_shutdown
           << ",\"resource_samples\":[";
    bool first_sample = true;
    for (const auto& sample : result.resource_samples) {
      if (!first_sample) output << ',';
      first_sample = false;
      output << "{\"completed_operations\":" << sample.completed_operations
             << ",\"elapsed_ms\":" << sample.elapsed_ms << ",\"handles\":" << sample.handles
             << ",\"working_set_bytes\":" << sample.working_set_bytes
             << ",\"private_bytes\":" << sample.private_bytes
             << ",\"peak_working_set_bytes\":" << sample.peak_working_set_bytes << '}';
    }
    output << "]}";
  }
  output << "\n],\"total_ops\":" << total_operations << ",\"result\":\""
         << (passed ? "passed" : "failed") << "\",\"replay_minimization_negative_control\":"
         << (std::string(mode) == "self-test" ? "true" : "false")
         << ",\"malformed_replays_rejected\":" << malformed_replays_rejected << "}\n";
  output.flush(); Require(static_cast<bool>(output), "report flush failed");
  output.close(); Require(!output.fail(), "report close failed");
}
}  // namespace

int main(int argc, char** argv) {
  std::string report, replay;
  bool force_fail = false, self_test = false;
  for (int argument_count = 1; argument_count < argc; ++argument_count) {
    const std::string argument = argv[argument_count];
    if (argument == "--report" && argument_count + 1 < argc) report = argv[++argument_count];
    else if (argument == "--replay" && argument_count + 1 < argc) replay = argv[++argument_count];
    else if (argument == "--force-fail") force_fail = true;
    else if (argument == "--self-test") self_test = true;
    else return 2;
  }
  if ((self_test && force_fail) || ((self_test || force_fail) && !replay.empty())) return 2;
  try {
    // Probe report output before any expensive protocol work.
    if (!report.empty()) { std::ofstream probe(report, std::ios::binary); Require(bool(probe), "report open failed"); }
    std::vector<RunResult> results;
    std::vector<std::uint64_t> seeds;
    unsigned malformed_replays_rejected = 0;
    if (self_test || force_fail) {
      auto operations = Generate(kSeeds[0], 12);
      operations.push_back({2, 0, 0, 0});
      operations.push_back({6, 0, 0, 0});
      const auto negative = Run(operations);
      Require(negative.mismatch == "document", "omitted-write mutant was not detected");
      if (force_fail) { std::puts("RESULT FAIL omitted-write mutation detected"); return 1; }
      auto minimal = Minimize(operations, negative.mismatch);
      Require(minimal.size() == 1 && minimal.front().kind == 6, "failure sequence was not one-minimal");
      if (!report.empty()) {
        WriteOperations(report + ".original.replay", operations);
        WriteOperations(report + ".replay", minimal);
        Require(Run(ReadOperations(report + ".replay")).mismatch == negative.mismatch, "replay did not fail identically");
        const std::string malformed = report + ".malformed.replay";
        for (const auto& row : {"0 0 0\n", "0 0 0 0 extra\n", "7 0 0 0\n", "",
                                "0 -1 0 0\n", "0 4294967296 0 0\n"}) {
          {
            std::ofstream output(malformed, std::ios::binary);
            output << row;
            output.close(); Require(!output.fail(), "malformed fixture write failed");
          }
          bool rejected = false;
          try { ReadOperations(malformed); } catch (const std::exception&) { rejected = true; }
          Require(rejected, "malformed replay accepted");
          ++malformed_replays_rejected;
        }
      }
      // The same operation without the mutant must succeed.
      minimal.front().kind = 0;
      results.push_back(Run(minimal)); seeds.push_back(kSeeds[0]);
    } else if (!replay.empty()) {
      results.push_back(Run(ReadOperations(replay))); seeds.push_back(kSeeds[0]);
    } else {
      // Warm up COM/TSF once so before/after resource samples exclude first-load effects.
      Require(Run(Generate(kSeeds[0], 2000)).mismatch.empty(), "warmup failed");
      for (const auto seed : kSeeds) {
        auto operations = Generate(seed, 100000);
        // Preserve the exact failing input even if infrastructure or an
        // invariant assertion throws before a RunResult can be returned.
        if (!report.empty()) WriteOperations(report + ".seed-" + std::to_string(seeds.size() + 1) + ".replay", operations);
        const auto measured = Run(operations);
        results.push_back(measured); seeds.push_back(seed);
        std::printf("seed=0x%llx operations=%zu mismatch=%s ms=%llu handles=%lu->%lu\n",
            static_cast<unsigned long long>(seed), measured.operations,
            measured.mismatch.empty() ? "none" : measured.mismatch.c_str(),
            static_cast<unsigned long long>(measured.elapsed_ms), measured.resource_samples.front().handles,
            measured.resource_samples.back().handles);
        if (!measured.mismatch.empty()) {
          if (!report.empty()) {
            WriteReport(report, results, seeds, "stress", 0);
            WriteOperations(report + ".minimal.replay", Minimize(operations, measured.mismatch));
          }
          break;
        }
      }
    }
    if (!report.empty()) WriteReport(report, results, seeds,
        self_test ? "self-test" : replay.empty() ? "stress" : "replay", malformed_replays_rejected);
    for (const auto& result : results) {
      if (!result.mismatch.empty()) {
        std::printf("RESULT FAIL %s\n", result.mismatch.c_str());
        return 1;
      }
    }
    std::puts("RESULT PASS"); return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "EDITOR PROTOCOL BLOCKED: %s\n", error.what()); return 2;
  }
}
