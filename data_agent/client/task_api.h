#pragma once

#include "data_agent/common/types.h"

namespace data_agent {

TaskResult submit_task(const TaskSpec &task,
    RuntimeMode mode = RuntimeMode::kAgentLevelTxn);

}  // namespace data_agent
