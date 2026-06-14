#pragma once

#include <stdint.h>

#include <vector>

namespace data_agent {

enum class AgentTxnState {
    kCreated,
    kRunning,
    kWaitingWinner,
    kCommitting,
    kCommitted,
    kAborted,
};

struct AgentTxnContext {
    uint64_t txn_id;
    uint64_t task_id;
    AgentTxnState state;
    std::vector<uint64_t> branch_ids;
    uint64_t winner_branch_id;

    AgentTxnContext()
        : txn_id(0),
          task_id(0),
          state(AgentTxnState::kCreated),
          winner_branch_id(0) {}
};

}  // namespace data_agent
