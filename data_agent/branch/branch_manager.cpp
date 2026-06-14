#include "data_agent/branch/branch_manager.h"

namespace data_agent {

BranchManager::BranchManager() {}

Branch BranchManager::Create(uint64_t txn_id, uint64_t branch_id) const {
    Branch branch;
    branch.txn_id = txn_id;
    branch.branch_id = branch_id;
    branch.state = BranchState::kCreated;
    return branch;
}

std::vector<Branch> BranchManager::CreateBranches(uint64_t txn_id,
        uint32_t branch_count) const {
    std::vector<Branch> branches;
    branches.reserve(branch_count);
    for (uint32_t i = 0; i < branch_count; ++i) {
        branches.push_back(Create(txn_id, i));
    }
    return branches;
}

void BranchManager::MarkWinner(std::vector<Branch> *branches,
        uint64_t winner_branch_id) const {
    if (branches == 0) {
        return;
    }
    for (uint64_t i = 0; i < branches->size(); ++i) {
        Branch &branch = (*branches)[i];
        if (branch.branch_id == winner_branch_id) {
            branch.state = BranchState::kCommitted;
        } else {
            branch.state = BranchState::kDiscarded;
        }
    }
}

}  // namespace data_agent
