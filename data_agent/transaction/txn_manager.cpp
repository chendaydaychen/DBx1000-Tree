#include "data_agent/transaction/txn_manager.h"

namespace data_agent {

TxnManager::TxnManager() {}

AgentTxnContext TxnManager::Begin(const TaskSpec &task, uint64_t txn_id) const {
    AgentTxnContext context;
    context.txn_id = txn_id;
    context.task_id = task.task_id;
    context.state = AgentTxnState::kRunning;
    return context;
}

}  // namespace data_agent
