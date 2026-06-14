#pragma once

#include <vector>

#include "data_agent/branch/branch.h"

namespace data_agent {

class BranchManager {
public:
    BranchManager();

    Branch Create(uint64_t txn_id, uint64_t branch_id) const;
    std::vector<Branch> CreateBranches(uint64_t txn_id, uint32_t branch_count) const;
    void MarkWinner(std::vector<Branch> *branches, uint64_t winner_branch_id) const;
};

}  // namespace data_agent
