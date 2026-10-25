// EpoCanva P3.10 -- implementation of the context identity registry. The rules are in
// context_registry.h; this file only applies them. Everything runs on the STA thread that
// owns the contexts; no lock is needed and none is taken.
#include "context_registry.h"

namespace epocanva {
namespace tsf {
namespace context {

const char* DescribeContextRegistryStatus(ContextRegistryStatus status) {
  switch (status) {
    case ContextRegistryStatus::kOk:
      return "Ok";
    case ContextRegistryStatus::kInvalidContext:
      return "InvalidContext";
    case ContextRegistryStatus::kCapacityExceeded:
      return "CapacityExceeded";
    case ContextRegistryStatus::kClosed:
      return "Closed";
  }
  return "UnknownRegistryStatus";
}

ContextRegistry::ContextRegistry(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
  diagnostics_.capacity = capacity_;
}

ContextRegistry::~ContextRegistry() { ReleaseAll(); }

ContextRegistryStatus ContextRegistry::LookupOrInsert(ITfContext* context,
                                                      Entry** entry) {
  *entry = nullptr;
  if (closed_) {
    ++diagnostics_.lookups;
    return ContextRegistryStatus::kClosed;
  }
  if (context == nullptr) {
    return ContextRegistryStatus::kInvalidContext;
  }
  ++diagnostics_.lookups;
  IUnknown* canonical = nullptr;
  const HRESULT hr =
      context->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&canonical));
  if (FAILED(hr) || canonical == nullptr) {
    return ContextRegistryStatus::kInvalidContext;
  }
  // Pointer comparison is valid because every entry holds its own reference to the
  // canonical IUnknown; the address cannot be recycled while the entry exists. This is the
  // documented COM identity rule, not an address-based guess.
  for (Entry& candidate : entries_) {
    if (candidate.canonical == canonical) {
      canonical->Release();  // the entry already holds its reference
      *entry = &candidate;
      return ContextRegistryStatus::kOk;
    }
  }
  if (entries_.size() >= capacity_) {
    canonical->Release();
    ++diagnostics_.capacity_refusals;
    return ContextRegistryStatus::kCapacityExceeded;
  }
  Entry created;
  created.canonical = canonical;  // ownership moves into the entry
  created.context_id = next_context_id_++;
  created.generation = 0;
  created.uncertain_blocked = false;
  entries_.push_back(created);
  ++diagnostics_.identities_assigned;
  ++diagnostics_.canonical_references_acquired;
  if (entries_.size() > diagnostics_.peak_entries) {
    diagnostics_.peak_entries = entries_.size();
  }
  *entry = &entries_.back();
  return ContextRegistryStatus::kOk;
}

ContextRegistryStatus ContextRegistry::Observe(ITfContext* context,
                                               ContextIdRecord* record) {
  if (record == nullptr) {
    return ContextRegistryStatus::kInvalidContext;
  }
  *record = ContextIdRecord{};
  Entry* entry = nullptr;
  const ContextRegistryStatus status = LookupOrInsert(context, &entry);
  if (status != ContextRegistryStatus::kOk || entry == nullptr) {
    return status;
  }
  record->context_id = entry->context_id;
  record->generation = entry->generation;
  record->uncertain_blocked = entry->uncertain_blocked;
  return ContextRegistryStatus::kOk;
}

ContextRegistryStatus ContextRegistry::BeginBinding(ITfContext* context,
                                                    ContextIdRecord* record) {
  if (record == nullptr) {
    return ContextRegistryStatus::kInvalidContext;
  }
  *record = ContextIdRecord{};
  Entry* entry = nullptr;
  const ContextRegistryStatus status = LookupOrInsert(context, &entry);
  if (status != ContextRegistryStatus::kOk || entry == nullptr) {
    return status;
  }
  record->context_id = entry->context_id;
  record->uncertain_blocked = entry->uncertain_blocked;
  if (entry->uncertain_blocked) {
    // No binding is begun; the generation stays where it was so a refused bind does not
    // look like a newer binding in any later comparison.
    record->generation = entry->generation;
    return ContextRegistryStatus::kOk;
  }
  record->generation = ++entry->generation;
  ++diagnostics_.binding_generations;
  return ContextRegistryStatus::kOk;
}

bool ContextRegistry::IsUncertain(ITfContext* context) {
  if (context == nullptr) {
    return false;
  }
  ++diagnostics_.uncertain_lookups;
  IUnknown* canonical = nullptr;
  if (FAILED(context->QueryInterface(IID_IUnknown,
                                     reinterpret_cast<void**>(&canonical))) ||
      canonical == nullptr) {
    return false;
  }
  bool blocked = false;
  for (const Entry& entry : entries_) {
    if (entry.canonical == canonical) {
      blocked = entry.uncertain_blocked;
      break;
    }
  }
  canonical->Release();
  if (blocked) {
    ++diagnostics_.uncertain_hits;
  }
  return blocked;
}

ContextRegistryStatus ContextRegistry::MarkUncertain(ITfContext* context) {
  Entry* entry = nullptr;
  const ContextRegistryStatus status = LookupOrInsert(context, &entry);
  if (status != ContextRegistryStatus::kOk || entry == nullptr) {
    return status;
  }
  entry->uncertain_blocked = true;
  ++diagnostics_.uncertain_marks;
  return ContextRegistryStatus::kOk;
}

bool ContextRegistry::Forget(ITfContext* context) {
  if (context == nullptr) {
    return false;
  }
  IUnknown* canonical = nullptr;
  if (FAILED(context->QueryInterface(IID_IUnknown,
                                     reinterpret_cast<void**>(&canonical))) ||
      canonical == nullptr) {
    return false;
  }
  for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
    if (iterator->canonical == canonical) {
      canonical->Release();  // the temporary QI reference
      if (iterator->uncertain_blocked) {
        // Deliberately kept: dropping it would silently forget the lockdown.
        return false;
      }
      iterator->canonical->Release();
      ++diagnostics_.canonical_references_released;
      entries_.erase(iterator);
      ++diagnostics_.forgotten_entries;
      return true;
    }
  }
  canonical->Release();
  return false;
}

void ContextRegistry::ReleaseAll() {
  if (closed_) {
    return;
  }
  closed_ = true;
  for (Entry& entry : entries_) {
    if (entry.canonical != nullptr) {
      entry.canonical->Release();
      entry.canonical = nullptr;
      ++diagnostics_.canonical_references_released;
    }
  }
  entries_.clear();
  diagnostics_.live_entries = 0;
  diagnostics_.closed = true;
}

ContextRegistryDiagnostics ContextRegistry::Diagnostics() const {
  ContextRegistryDiagnostics copy = diagnostics_;
  copy.live_entries = entries_.size();
  copy.capacity = capacity_;
  copy.closed = closed_;
  return copy;
}

bool ContextRegistry::FindById(std::uint64_t context_id,
                               ContextIdRecord* record) const {
  if (record == nullptr || context_id == 0) {
    return false;
  }
  for (const Entry& entry : entries_) {
    if (entry.context_id == context_id) {
      record->context_id = entry.context_id;
      record->generation = entry.generation;
      record->uncertain_blocked = entry.uncertain_blocked;
      return true;
    }
  }
  return false;
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
