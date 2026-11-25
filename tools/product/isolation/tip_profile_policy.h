// Copyright EpoCanva contributors. SPDX-License-Identifier: AGPL-3.0-only
// Pure registration policy. Ops supplies checked observations and mutations;
// the fake implementation never loads a DLL, opens the registry, or calls TSF.
#ifndef EPOCANVA_TIP_PROFILE_POLICY_H_
#define EPOCANVA_TIP_PROFILE_POLICY_H_

namespace epocanva::tip_profile {

enum class Owner { kAbsent, kUser, kMachine, kBlocked };
inline bool Owned(Owner owner) {
  return owner == Owner::kUser || owner == Owner::kMachine;
}

// The DLL export removes both scopes. Even two matching registrations are
// ambiguous for compensation: fail closed rather than deleting both.
inline Owner CombineOwners(Owner user, Owner machine) {
  if (user == Owner::kBlocked || machine == Owner::kBlocked ||
      (user != Owner::kAbsent && machine != Owner::kAbsent)) {
    return Owner::kBlocked;
  }
  return user != Owner::kAbsent ? user : machine;
}

struct State {
  Owner owner = Owner::kBlocked;
  bool processor = false;
  bool target = false;
  bool other_profiles = false;
};
inline bool Same(const State& a, const State& b) {
  return a.owner == b.owner && a.processor == b.processor &&
         a.target == b.target && a.other_profiles == b.other_profiles;
}
inline bool Coherent(const State& state) {
  return state.owner != Owner::kBlocked &&
         (!(state.target || state.other_profiles) || state.processor) &&
         (state.owner != Owner::kAbsent || !state.processor);
}

enum class Mutation {
  kRegisterCom, kRegisterProcessor, kAddTarget,
  kRemoveTarget, kUnregisterProcessor, kUnregisterCom
};

// Each step verifies its precondition again and verifies the actual post-state.
// This detects observable interference; TSF and the COM registration exports do
// not provide an atomic transaction against unrelated registry writers.
template <class Ops>
bool Step(Ops& ops, Mutation mutation, const State& before, const State& after) {
  State observed;
  if (!ops.Read(&observed) || !Same(observed, before)) return false;
  if (!ops.Change(mutation)) return false;
  return ops.Read(&observed) && Same(observed, after);
}

struct Attempts {
  bool com = false;
  bool processor = false;
  bool target = false;
};

// Compensation only removes objects absent at entry and attempted by this run.
// Never remove a parent after a failed child cleanup. Failed calls may have
// changed state, so inspect instead of assuming HRESULT implies atomicity.
template <class Ops>
bool Rollback(Ops& ops, const State& before, Owner acquired,
              const Attempts& attempts) {
  State current;
  if (!ops.Read(&current)) return false;
  if (Same(current, before)) return true;
  if (!Owned(acquired) || current.owner != acquired ||
      current.other_profiles != before.other_profiles ||
      (before.processor && !current.processor) ||
      (before.target && !current.target)) return false;
  if (current.target && !before.target) {
    if (!attempts.target) return false;
    State after = current;
    after.target = false;
    if (!Step(ops, Mutation::kRemoveTarget, current, after)) return false;
    current = after;
  }
  if (current.processor && !before.processor) {
    if (!attempts.processor || current.target || current.other_profiles) return false;
    State after = current;
    after.processor = false;
    if (!Step(ops, Mutation::kUnregisterProcessor, current, after)) return false;
    current = after;
  }
  if (before.owner == Owner::kAbsent && Owned(current.owner)) {
    if (!attempts.com || current.processor || current.target ||
        current.other_profiles) return false;
    State after = current;
    after.owner = Owner::kAbsent;
    if (!Step(ops, Mutation::kUnregisterCom, current, after)) return false;
    current = after;
  }
  return Same(current, before);
}

// Exit codes: 0 verified success; 1 failed operation with completed/no necessary
// compensation; 4 untrusted/inconsistent state; 5 incomplete compensation.
template <class Ops>
int Register(Ops& ops) {
  State before;
  if (!ops.Read(&before) || !Coherent(before)) return 4;
  State current = before;
  Attempts attempts;
  Owner acquired = before.owner;
  if (before.owner == Owner::kAbsent) {
    State check;
    if (!ops.Read(&check) || !Same(check, before)) return 4;
    attempts.com = true;
    const bool registered = ops.Change(Mutation::kRegisterCom);
    // ReadOwner does not depend on profile enumeration; it can identify a COM
    // registration created before a later enumeration/COM failure.
    acquired = ops.ReadOwner();
    current.owner = acquired;
    if (!registered || !Owned(acquired) || !ops.Read(&check) ||
        !Same(check, current)) {
      return Rollback(ops, before, acquired, attempts) ? 1 : 5;
    }
  } else if (!Step(ops, Mutation::kRegisterCom, before, before)) {
    // Preserve the DLL's repair path for an existing matching registration.
    // Its export owns its internal registry snapshot/rollback. Never try to
    // compensate an export failure by unregistering the pre-existing owner.
    return 5;
  }
  if (before.target) return 0;
  if (!current.processor) {
    State after = current;
    after.processor = true;
    attempts.processor = true;
    if (!Step(ops, Mutation::kRegisterProcessor, current, after)) {
      return Rollback(ops, before, acquired, attempts) ? 1 : 5;
    }
    current = after;
  }
  State after = current;
  after.target = true;
  attempts.target = true;
  if (!Step(ops, Mutation::kAddTarget, current, after)) {
    return Rollback(ops, before, acquired, attempts) ? 1 : 5;
  }
  return 0;
}

template <class Ops>
int Unregister(Ops& ops) {
  State current;
  if (!ops.Read(&current) || !Coherent(current) || current.other_profiles) return 4;
  if (current.owner == Owner::kAbsent) return 0;
  // Unregistration is not transactional. On failure keep the remaining state
  // and return failure; AddLanguageProfile cannot restore activation, enabled
  // state, icon and settings from the former helper's presence-only snapshot.
  if (current.target) {
    State after = current;
    after.target = false;
    if (!Step(ops, Mutation::kRemoveTarget, current, after)) return 1;
    current = after;
  }
  if (current.processor) {
    State after = current;
    after.processor = false;
    if (!Step(ops, Mutation::kUnregisterProcessor, current, after)) return 1;
    current = after;
  }
  State after = current;
  after.owner = Owner::kAbsent;
  return Step(ops, Mutation::kUnregisterCom, current, after) ? 0 : 1;
}

}  // namespace epocanva::tip_profile
#endif  // EPOCANVA_TIP_PROFILE_POLICY_H_
