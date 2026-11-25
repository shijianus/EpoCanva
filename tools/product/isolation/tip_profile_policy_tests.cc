// Copyright EpoCanva contributors. SPDX-License-Identifier: AGPL-3.0-only
// Standalone C++17 tests. No Windows headers, COM, DLL loading or registry access.
#include "tip_profile_policy.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace epocanva::tip_profile;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fake {
  State state{Owner::kAbsent, false, false, false};
  std::vector<Mutation> calls;
  int reads = 0;
  int fail_read = -1;
  int change_owner_read = -1;
  bool fail_owner = false;
  bool introduce_other = false;
  bool lie_about_add = false;
  std::vector<Mutation> fail_before, fail_after;
  Owner ReadOwner() { return fail_owner ? Owner::kBlocked : state.owner; }
  bool Read(State* observed) {
    ++reads;
    if (reads == change_owner_read) state.owner = Owner::kBlocked;
    if (reads == fail_read) return false;
    *observed = state;
    return true;
  }
  bool Has(const std::vector<Mutation>& mutations, Mutation mutation) const {
    return std::find(mutations.begin(), mutations.end(), mutation) != mutations.end();
  }
  bool Change(Mutation mutation) {
    calls.push_back(mutation);
    if (Has(fail_before, mutation)) return false;
    switch (mutation) {
      case Mutation::kRegisterCom:
        if (state.owner == Owner::kAbsent) state.owner = Owner::kUser;
        break;
      case Mutation::kRegisterProcessor: state.processor = true; break;
      case Mutation::kAddTarget:
        state.target = !lie_about_add;
        if (introduce_other) state.other_profiles = true;
        break;
      case Mutation::kRemoveTarget: state.target = false; break;
      case Mutation::kUnregisterProcessor: state.processor = false; break;
      case Mutation::kUnregisterCom: state.owner = Owner::kAbsent; break;
    }
    return !Has(fail_after, mutation);
  }
};

struct Test { std::string name; std::function<void()> run; };
std::vector<Test> Cases() {
  std::vector<Test> tests;
  tests.push_back({"both_registry_scopes_are_required", [] {
    const Owner values[] = {Owner::kAbsent, Owner::kUser, Owner::kMachine, Owner::kBlocked};
    for (Owner user : values) for (Owner machine : values) {
      Owner expected = Owner::kBlocked;
      if (user == Owner::kAbsent && machine != Owner::kBlocked) expected = machine;
      else if (machine == Owner::kAbsent && user != Owner::kBlocked) expected = user;
      Check(CombineOwners(user, machine) == expected, "scope masking or double ownership");
    }
  }});
  for (bool install : {false, true}) {
    for (int scenario = 0; scenario != 4; ++scenario) {
      tests.push_back({std::string(install ? "register" : "unregister") +
          "_untrusted_snapshot_" + std::to_string(scenario), [=] {
        Fake ops;
        if (scenario == 0) ops.state.owner = Owner::kBlocked;
        if (scenario == 1) ops.fail_read = 1;
        if (scenario == 2) ops.state = {Owner::kAbsent, true, true, false};
        if (scenario == 3) ops.state = {Owner::kUser, false, true, false};
        const State before = ops.state;
        Check((install ? Register(ops) : Unregister(ops)) == 4, "untrusted state accepted");
        Check(ops.calls.empty() && Same(before, ops.state), "untrusted state mutated");
      }});
    }
  }
  tests.push_back({"existing_registration_is_idempotent", [] {
    Fake ops;
    ops.state = {Owner::kMachine, true, true, true};
    const State before = ops.state;
    Check(Register(ops) == 0 && Register(ops) == 0, "existing registration failed");
    Check(ops.calls == std::vector<Mutation>{Mutation::kRegisterCom, Mutation::kRegisterCom}
          && Same(ops.state, before), "existing registration was not repaired in place");
  }});
  tests.push_back({"fresh_registration_and_removal", [] {
    Fake ops;
    const State absent = ops.state;
    Check(Register(ops) == 0, "fresh registration failed");
    Check(Same(ops.state, State{Owner::kUser, true, true, false}), "incorrect registered state");
    Check(ops.calls == std::vector<Mutation>{Mutation::kRegisterCom,
          Mutation::kRegisterProcessor, Mutation::kAddTarget}, "registration order");
    ops.calls.clear();
    Check(Unregister(ops) == 0 && Same(ops.state, absent), "removal failed");
    Check(ops.calls == std::vector<Mutation>{Mutation::kRemoveTarget,
          Mutation::kUnregisterProcessor, Mutation::kUnregisterCom}, "removal order");
    ops.calls.clear();
    Check(Unregister(ops) == 0 && ops.calls.empty(), "absent removal not idempotent");
  }});
  const Mutation additions[] = {Mutation::kRegisterCom, Mutation::kRegisterProcessor,
                               Mutation::kAddTarget};
  for (int operation = 0; operation != 3; ++operation) for (bool partial : {false, true}) {
    tests.push_back({"fresh_failure_" + std::to_string(operation) +
        (partial ? "_partial" : "_unchanged"), [=] {
      Fake ops;
      const State before = ops.state;
      (partial ? ops.fail_after : ops.fail_before).push_back(additions[operation]);
      Check(Register(ops) == 1, "failure hidden or compensation incomplete");
      Check(Same(ops.state, before), "newly created objects not compensated");
    }});
  }
  for (bool processor : {false, true}) for (bool partial : {false, true}) {
    tests.push_back({"existing_com_add_failure_" + std::to_string(processor) +
        (partial ? "_partial" : "_unchanged"), [=] {
      Fake ops;
      ops.state = {Owner::kMachine, processor, false, processor};
      const State before = ops.state;
      (partial ? ops.fail_after : ops.fail_before).push_back(Mutation::kAddTarget);
      Check(Register(ops) == 1 && Same(ops.state, before), "old registration lost on failure");
      Check(!ops.Has(ops.calls, Mutation::kUnregisterCom), "existing COM removed");
      if (processor) Check(!ops.Has(ops.calls, Mutation::kUnregisterProcessor), "old TSF class lost");
    }});
  }
  tests.push_back({"existing_com_repair_failure_is_not_unregistered", [] {
    Fake ops;
    ops.state = {Owner::kMachine, true, true, false};
    const State before = ops.state;
    ops.fail_after.push_back(Mutation::kRegisterCom);
    Check(Register(ops) == 5 && Same(ops.state, before), "repair failure hidden");
    Check(ops.calls == std::vector<Mutation>{Mutation::kRegisterCom}, "compensated pre-existing COM");
  }});
  for (bool partial : {false, true}) {
    tests.push_back({std::string("existing_com_processor_failure_") +
        (partial ? "partial" : "unchanged"), [=] {
      Fake ops;
      ops.state.owner = Owner::kUser;
      const State before = ops.state;
      (partial ? ops.fail_after : ops.fail_before).push_back(Mutation::kRegisterProcessor);
      Check(Register(ops) == 1 && Same(ops.state, before), "old COM lost on class failure");
      Check(!ops.Has(ops.calls, Mutation::kUnregisterCom), "unregistered existing COM");
    }});
  }
  const Mutation removals[] = {Mutation::kRemoveTarget, Mutation::kUnregisterProcessor,
                              Mutation::kUnregisterCom};
  for (int operation = 0; operation != 3; ++operation) for (bool partial : {false, true}) {
    tests.push_back({"rollback_failure_" + std::to_string(operation) +
        (partial ? "_partial" : "_unchanged"), [=] {
      Fake ops;
      ops.fail_after.push_back(Mutation::kAddTarget);
      (partial ? ops.fail_after : ops.fail_before).push_back(removals[operation]);
      Check(Register(ops) == 5, "rollback failure concealed");
      Check(ops.calls.back() == removals[operation], "removed parent after failed child cleanup");
    }});
    tests.push_back({"unregister_failure_" + std::to_string(operation) +
        (partial ? "_partial" : "_unchanged"), [=] {
      Fake ops;
      ops.state = {Owner::kUser, true, true, false};
      (partial ? ops.fail_after : ops.fail_before).push_back(removals[operation]);
      Check(Unregister(ops) == 1, "unregister failure concealed");
      Check(ops.calls.back() == removals[operation], "continued deletion after failure");
      for (Mutation addition : additions) Check(!ops.Has(ops.calls, addition), "reconstructed unowned settings");
    }});
  }
  tests.push_back({"other_language_profiles_block_removal", [] {
    Fake ops;
    ops.state = {Owner::kUser, true, true, true};
    Check(Unregister(ops) == 4 && ops.calls.empty(), "other language removed");
  }});
  tests.push_back({"unexpected_profile_during_rollback_retains_parents", [] {
    Fake ops;
    ops.introduce_other = true;
    Check(Register(ops) == 5, "unexpected profile accepted");
    Check(ops.calls.back() == Mutation::kAddTarget, "unexpected profile deleted by cleanup");
  }});
  tests.push_back({"successful_hresult_needs_readback", [] {
    Fake ops;
    ops.lie_about_add = true;
    const State before = ops.state;
    Check(Register(ops) == 1 && Same(ops.state, before), "false success without target");
  }});
  tests.push_back({"unknown_owner_after_com_creation_blocks_compensation", [] {
    Fake ops;
    ops.fail_owner = true;
    Check(Register(ops) == 5, "unknown acquisition accepted");
    Check(ops.calls == std::vector<Mutation>{Mutation::kRegisterCom}, "deleted unknown owner");
  }});
  for (bool install : {false, true}) {
    Fake control;
    if (!install) control.state = {Owner::kUser, true, true, false};
    Check((install ? Register(control) : Unregister(control)) == 0, "baseline");
    for (int read = 1; read <= control.reads; ++read) {
      tests.push_back({std::string(install ? "register" : "unregister") +
          "_read_failure_" + std::to_string(read), [=] {
        Fake ops;
        if (!install) ops.state = {Owner::kUser, true, true, false};
        ops.fail_read = read;
        Check((install ? Register(ops) : Unregister(ops)) != 0, "failed observation accepted");
      }});
      tests.push_back({std::string(install ? "register" : "unregister") +
          "_owner_changed_" + std::to_string(read), [=] {
        Fake ops;
        if (!install) ops.state = {Owner::kUser, true, true, false};
        ops.change_owner_read = read;
        Check((install ? Register(ops) : Unregister(ops)) != 0, "changed owner accepted");
        // Fake::Change never rejects ownership: the policy must stop writes.
        Check(ops.state.owner == Owner::kBlocked, "wrote through changed owner");
      }});
    }
  }
  return tests;
}
}  // namespace

int main(int argc, char** argv) {
  auto tests = Cases();
  if (argc == 2 && std::string(argv[1]) == "--force-failure") {
    tests.push_back({"intentional_failure_control", [] { Check(false, "expected failure"); }});
  } else if (argc != 1) {
    std::fprintf(stderr, "usage: tip_profile_policy_tests [--force-failure]\n");
    return 2;
  }
  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.run();
      std::printf("PASS %s\n", test.name.c_str());
    } catch (const std::exception& error) {
      ++failures;
      std::printf("FAIL %s: %s\n", test.name.c_str(), error.what());
    }
  }
  std::printf("tests=%zu passed=%zu failed=%d skipped=0\n",
              tests.size(), tests.size() - failures, failures);
  return failures == 0 ? 0 : 1;
}
