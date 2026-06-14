#include "data_agent/operators/candidate_generate_op.h"

namespace data_agent {

void CandidateGenerateOp::Execute(TaskContext *context) const {
    if (context == 0) {
        return;
    }
    context->candidates.clear();
    context->candidates.reserve(context->spec.max_candidates);
    for (uint32_t i = 0; i < context->spec.max_candidates; ++i) {
        CandidateSpec candidate;
        candidate.candidate_id = i;
        candidate.summary = "candidate_" + std::to_string(i);
        context->candidates.push_back(candidate);
    }
}

}  // namespace data_agent
