#pragma once

#include "data_agent/transaction/txn_manager.h"
#include "data_agent/common/types.h"
#include "data_agent/runtime/task_context.h"

namespace data_agent {

class TaskRuntime {
public:
    TaskRuntime();

    TaskResult SubmitTask(const TaskSpec &task,
        RuntimeMode mode = RuntimeMode::kAgentLevelTxn) const;

private:
    TaskContext BuildContext(const TaskSpec &task) const;
    TaskResult SubmitAgentLevelTask(const TaskSpec &task) const;
    TaskResult SubmitBaselineMultiTxnTask(const TaskSpec &task) const;
    TaskResult SubmitBaselineMultiTxnTaskReal(const TaskSpec &task) const;

    TxnManager txn_manager_;
};

}  // namespace data_agent
