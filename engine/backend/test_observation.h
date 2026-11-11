// Internal evaluator adapter. This header is deliberately outside engine/include.
// Only the independent evaluation executable's backend copy defines the option;
// production libraries and DLLs neither compile nor export this function.
#ifndef EPOCANVA_BACKEND_TEST_OBSERVATION_H_
#define EPOCANVA_BACKEND_TEST_OBSERVATION_H_

#ifndef EPOCANVA_BACKEND_TEST_OBSERVATION
#error "Candidate observation is restricted to the evaluation executable"
#endif

#include "epocanva_backend.h"
#include "engine/candidates/candidate_contract.h"

namespace epocanva::backend::testing {
struct CandidateObservation {
  candidates::Record contract;
  // Count of producer emissions before this row, prior to sorting/truncation.
  std::uint32_t producer_insertion_ordinal = 0;
};

// Copies the actual published record, under the same session/privacy/token
// checks as the public read operation. No attribution is inferred from a case.
epocanva_status ReadCandidateObservation(
    epocanva_backend_session_handle handle,
    const epocanva_backend_request* request, std::uint64_t snapshot_token,
    std::uint32_t candidate_ordinal, CandidateObservation* out) noexcept;
}  // namespace epocanva::backend::testing
#endif
