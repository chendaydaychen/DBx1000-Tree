#include "data_agent/operators/winner_select_op.h"

namespace data_agent {

uint64_t WinnerSelectOp::Execute(TaskContext *context) const {
    if (context == 0 || context->branches.empty()) {
        return 0;
    }

    uint64_t winner_branch_id = context->branches[0].branch_id;
    double winner_score = context->branches[0].result.score;
    for (uint64_t i = 1; i < context->branches.size(); ++i) {
        const Branch &branch = context->branches[i];
        if (branch.result.feasible && branch.result.score > winner_score) {
            winner_branch_id = branch.branch_id;
            winner_score = branch.result.score;
        }
    }

    context->txn.winner_branch_id = winner_branch_id;
    context->txn.state = AgentTxnState::kWaitingWinner;
    return winner_branch_id;
}

}  // namespace data_agent
