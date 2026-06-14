#pragma once

#include "data_agent/runtime/task_context.h"

namespace data_agent {

class WinnerSelectOp {
public:
    uint64_t Execute(TaskContext *context) const;
};

}  // namespace data_agent
