#include "data_agent/operators/transactional_apply_op.h"

#include "data_agent/branch/branch_manager.h"
#include "data_agent/object_store/object_store.h"

namespace data_agent {

TaskResult TransactionalApplyOp::Execute(TaskContext *context) const {
    TaskResult result;
    if (context == 0) {
        result.status = TaskStatus::kAborted;
        result.message = "Task context was null.";
        return result;
    }

    BranchManager branch_manager;
    branch_manager.MarkWinner(&context->branches, context->txn.winner_branch_id);

    result.task_id = context->spec.task_id;
    result.status = TaskStatus::kAborted;
    result.txn_id = context->txn.txn_id;
    result.winner_branch_id = context->txn.winner_branch_id;
    result.candidate_count = static_cast<uint32_t>(context->candidates.size());
    result.total_intent_count = static_cast<uint32_t>(context->intents.size());
    result.output_object_id = "output:" + std::to_string(context->spec.task_id);
    result.message = "Synthetic task aborted before backend apply.";

    if (context->object_store == 0) {
        result.message = "Object store was not configured.";
        return result;
    }

    if (!context->object_store->SelectWinner(
            context->txn.txn_id,
            static_cast<uint32_t>(context->txn.winner_branch_id))) {
        result.message = "Backend winner selection failed.";
        context->object_store->AbortTxn(context->txn.txn_id);
        return result;
    }

    if (context->spec.conflict_period > 0 &&
            context->spec.task_id % context->spec.conflict_period == 0) {
        context->object_store->ForceBumpVersion(
            "input:" + std::to_string(context->spec.task_id));
    }

    BackendCommitResult commit = context->object_store->CommitWinner(context->txn.txn_id);
    if (!commit.ok) {
        result.message = "Backend commit failed: " + commit.error;
        result.conflict_abort_count = commit.conflict_abort_count;
        context->txn.state = AgentTxnState::kAborted;
        return result;
    }

    context->txn.state = AgentTxnState::kCommitted;
    result.status = TaskStatus::kCommitted;
    result.message = "Synthetic task committed through Data Agent runtime and object store.";
    result.backend_object_count = static_cast<uint32_t>(context->object_store->ObjectCount());
    result.applied_intent_count = commit.committed_intent_count + commit.validated_read_count;
    result.validated_read_count = commit.validated_read_count;
    result.applied_write_count = commit.applied_write_count;
    result.backend_planned_loser_count = commit.planned_loser_count;
    result.conflict_abort_count = commit.conflict_abort_count;

    for (uint64_t i = 0; i < context->branches.size(); ++i) {
        const Branch &branch = context->branches[i];
        if (branch.branch_id == context->txn.winner_branch_id) {
            result.committed_branch_count += 1;
            result.winner_score = branch.result.score;
        } else {
            result.discarded_branch_count += 1;
        }
    }

    Object output_object;
    if (context->object_store->Get(result.output_object_id, &output_object)) {
        result.output_payload = output_object.payload;
        result.output_version = output_object.version;
    }
    return result;
}

}  // namespace data_agent
