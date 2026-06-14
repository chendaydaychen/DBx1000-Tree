#pragma once

#include "data_agent/runtime/task_context.h"

namespace data_agent {

class CandidateGenerateOp {
public:
    void Execute(TaskContext *context) const;
};

}  // namespace data_agent
