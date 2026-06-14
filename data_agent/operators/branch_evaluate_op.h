#pragma once

#include "data_agent/branch/branch_manager.h"
#include "data_agent/intent/intent_manager.h"
#include "data_agent/runtime/task_context.h"

namespace data_agent {

class BranchEvaluateOp {
public:
    BranchEvaluateOp();

    void Execute(TaskContext *context) const;

private:
    double ScoreCandidate(uint64_t task_id, uint64_t branch_id) const;
};

}  // namespace data_agent
