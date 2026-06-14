#pragma once

#include <vector>

#include "data_agent/branch/branch.h"
#include "data_agent/common/types.h"
#include "data_agent/intent/intent.h"
#include "data_agent/transaction/txn_context.h"

namespace data_agent {

class ObjectStore;

struct TaskContext {
    TaskSpec spec;
    AgentTxnContext txn;
    std::vector<CandidateSpec> candidates;
    std::vector<Branch> branches;
    std::vector<Intent> intents;
    ObjectStore *object_store;

    TaskContext()
        : object_store(0) {}
};

}  // namespace data_agent
