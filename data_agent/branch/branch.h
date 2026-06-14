#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace data_agent {

enum class BranchState {
    kCreated,
    kRunning,
    kStaged,
    kWinner,
    kLoser,
    kCommitted,
    kDiscarded,
};

struct BranchResult {
    bool feasible;
    double score;
    std::string summary;

    BranchResult()
        : feasible(false), score(0.0) {}
};

struct Branch {
    uint64_t branch_id;
    uint64_t txn_id;
    BranchState state;
    std::vector<uint64_t> read_intent_ids;
    std::vector<uint64_t> write_intent_ids;
    BranchResult result;

    Branch()
        : branch_id(0), txn_id(0), state(BranchState::kCreated) {}
};

}  // namespace data_agent
