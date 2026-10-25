// EpoCanva P3.10 -- project-internal TSF context identity.
//
// WHAT AN IDENTITY IS. One TSF context object, identified by its canonical IUnknown
// (ITfContext::QueryInterface(IID_IUnknown)); COM guarantees that every interface of the
// same COM object yields the same IUnknown pointer. The registry keeps ONE reference to
// that canonical pointer for the entry's lifetime, which is what makes pointer comparison
// safe: the address cannot be reused by a different object while the entry exists.
//
// WHAT AN IDENTITY IS NOT. The id is not a caller-supplied number, not an ITfContext
// address exposed to callers, and not persisted anywhere. Callers receive a monotonic
// project id; the address comparison happens only inside this object, on the STA thread
// that owns the contexts. No COM pointer ever crosses a thread.
//
// GENERATIONS. A context's generation is incremented by BeginBinding() and identifies one
// binding attempt. A verdict or callback stamped with an older generation can never be
// applied to the newer binding. Re-focusing the same context therefore produces a new
// generation instead of silently reusing the old binding.
//
// THE UNCERTAIN LATCH. MarkUncertain() records "a TSF write on this context could not be
// confirmed" against the REAL identity, so a replacement coordinator that is handed the
// same ITfContext cannot bypass it with a different integer id. The latch lives exactly as
// long as this registry object; it is NOT a process-wide or permanent block, and this
// phase does not claim otherwise. There is deliberately no ClearUncertain(): the only way
// to end a registry's lifetime is to destroy it (host teardown).
//
// BOUNDED STATE. Every entry holds one COM reference to a context. The number of entries is
// capped (constructor argument, default kDefaultCapacity). Reaching the cap is an explicit
// kCapacityExceeded refusal -- never a silent eviction of an entry, because dropping an
// uncertain entry would silently forget a lockdown.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_REGISTRY_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_REGISTRY_H_

#include <windows.h>
#include <msctf.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace epocanva {
namespace tsf {
namespace context {

// One context's observed identity. `generation` is 0 until BeginBinding() has been called
// for this context; a value record from Observe() is a lookup, not a binding.
struct ContextIdRecord {
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  bool uncertain_blocked = false;
  bool valid() const { return context_id != 0; }
};

enum class ContextRegistryStatus : std::uint8_t {
  kOk = 0,
  kInvalidContext,   // null context or canonical IUnknown unavailable
  kCapacityExceeded, // the cap is reached and this is a new identity; nothing was evicted
  kClosed,           // ReleaseAll() ran; the registry is terminal
};

const char* DescribeContextRegistryStatus(ContextRegistryStatus status);

// Value-only diagnostics for assertions and text-free logging.
struct ContextRegistryDiagnostics {
  std::uint64_t identities_assigned = 0;
  std::uint64_t lookups = 0;
  std::uint64_t capacity_refusals = 0;
  std::uint64_t binding_generations = 0;
  std::uint64_t uncertain_marks = 0;
  std::uint64_t uncertain_lookups = 0;
  std::uint64_t uncertain_hits = 0;
  std::uint64_t forgotten_entries = 0;
  std::uint64_t canonical_references_acquired = 0;
  std::uint64_t canonical_references_released = 0;
  std::size_t live_entries = 0;
  std::size_t peak_entries = 0;
  std::size_t capacity = 0;
  bool closed = false;
  bool entries_balanced() const {
    return canonical_references_acquired ==
           canonical_references_released + live_entries;
  }
};

class ContextRegistry {
 public:
  static constexpr std::size_t kDefaultCapacity = 64;

  explicit ContextRegistry(std::size_t capacity = kDefaultCapacity);
  ~ContextRegistry();

  ContextRegistry(const ContextRegistry&) = delete;
  ContextRegistry& operator=(const ContextRegistry&) = delete;

  // Looks up or allocates the identity for `context` without starting a binding. The
  // returned generation is the context's last generation (0 before its first bind).
  ContextRegistryStatus Observe(ITfContext* context, ContextIdRecord* record);

  // Looks up or allocates the identity and increments its generation. A context whose
  // uncertain latch is set yields kOk with `record->uncertain_blocked == true` and does
  // NOT advance the generation: no binding was begun. The caller must refuse to bind.
  ContextRegistryStatus BeginBinding(ITfContext* context, ContextIdRecord* record);

  // Reads the latch for a real context. Never allocates: a context never seen by the
  // registry cannot be blocked. Used by the coordinator's gates with the bound context, so
  // a caller-supplied integer id has no influence.
  bool IsUncertain(ITfContext* context);

  // Records the latch for a real context. The context is registered if necessary; a
  // capacity refusal is reported and the latch is then NOT held by this registry.
  ContextRegistryStatus MarkUncertain(ITfContext* context);

  // Drops a non-uncertain entry (the context is being uninitialized). An uncertain entry is
  // deliberately kept, because forgetting it would erase the lockdown. Returns true when an
  // entry was dropped.
  bool Forget(ITfContext* context);

  // Terminal for this registry's lifetime: every entry reference is released and the object
  // refuses further operations. Destructor calls it.
  void ReleaseAll();

  ContextRegistryDiagnostics Diagnostics() const;

  // Test/evidence lookup by id; never exposes a COM pointer. Returns false when absent.
  bool FindById(std::uint64_t context_id, ContextIdRecord* record) const;

 private:
  struct Entry {
    IUnknown* canonical = nullptr;  // one reference held for the entry's lifetime
    std::uint64_t context_id = 0;
    std::uint64_t generation = 0;
    bool uncertain_blocked = false;
  };

  ContextRegistryStatus LookupOrInsert(ITfContext* context, Entry** entry);

  std::vector<Entry> entries_;
  std::size_t capacity_ = kDefaultCapacity;
  std::uint64_t next_context_id_ = 1;
  bool closed_ = false;
  ContextRegistryDiagnostics diagnostics_;
};

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_REGISTRY_H_
