#pragma once

#include "data_agent/common/types.h"
#include "data_agent/transaction/txn_context.h"

namespace data_agent {

class TxnManager {
public:
    TxnManager();

    AgentTxnContext Begin(const TaskSpec &task, uint64_t txn_id) const;
};

}  // namespace data_agent
