#include "data_agent/runtime/task_runtime.h"

#include "data_agent/operators/branch_evaluate_op.h"
#include "data_agent/operators/candidate_generate_op.h"
#include "data_agent/operators/transactional_apply_op.h"
#include "data_agent/operators/winner_select_op.h"
#include "data_agent/object_store/dbx1000_object_store.h"
#include "data_agent/object_store/dbx1000_test_backend.h"

namespace data_agent {

TaskRuntime::TaskRuntime() {}

TaskResult TaskRuntime::SubmitTask(const TaskSpec &task, RuntimeMode mode) const {
    if (mode == RuntimeMode::kBaselineMultiTxn) {
        return SubmitBaselineMultiTxnTask(task);
    }
    return SubmitAgentLevelTask(task);
}

TaskResult TaskRuntime::SubmitAgentLevelTask(const TaskSpec &task) const {
    TaskContext context = BuildContext(task);
    CandidateGenerateOp candidate_generate_op;
    BranchEvaluateOp branch_evaluate_op;
    WinnerSelectOp winner_select_op;
    TransactionalApplyOp transactional_apply_op;

    candidate_generate_op.Execute(&context);
    branch_evaluate_op.Execute(&context);
    winner_select_op.Execute(&context);
    return transactional_apply_op.Execute(&context);
}

TaskResult TaskRuntime::SubmitBaselineMultiTxnTask(const TaskSpec &task) const {
    if (task.backend_mode == BackendMode::kDbx1000Test) {
        return SubmitBaselineMultiTxnTaskReal(task);
    }

    TaskContext planning_context = BuildContext(task);
    CandidateGenerateOp candidate_generate_op;
    BranchEvaluateOp branch_evaluate_op;
    WinnerSelectOp winner_select_op;

    candidate_generate_op.Execute(&planning_context);
    branch_evaluate_op.Execute(&planning_context);
    uint64_t winner_branch_id = winner_select_op.Execute(&planning_context);

    TaskResult result;
    result.task_id = task.task_id;
    result.candidate_count = static_cast<uint32_t>(planning_context.candidates.size());
    result.total_intent_count = static_cast<uint32_t>(planning_context.intents.size());
    result.winner_branch_id = winner_branch_id;
    result.output_object_id = "output:" + std::to_string(task.task_id);
    result.txn_count = static_cast<uint32_t>(planning_context.branches.size());
    result.planned_abort_txn_count =
        result.txn_count > 0 ? result.txn_count - 1 : 0;

    for (uint64_t i = 0; i < planning_context.branches.size(); ++i) {
        const Branch &planned_branch = planning_context.branches[i];
        bool is_winner = planned_branch.branch_id == winner_branch_id;

        TaskSpec branch_task = task;
        branch_task.max_candidates = 1;
        TaskContext exec_context = BuildContext(branch_task);
        exec_context.candidates.push_back(planning_context.candidates[i]);
        exec_context.branches.push_back(planned_branch);
        exec_context.branches[0].branch_id = 0;
        exec_context.txn.winner_branch_id = 0;
        exec_context.txn.state = AgentTxnState::kWaitingWinner;
        exec_context.object_store->CreateBranch(exec_context.txn.txn_id, 0,
            planning_context.candidates[i].summary);

        for (uint64_t j = 0; j < planning_context.intents.size(); ++j) {
            const Intent &intent = planning_context.intents[j];
            if (intent.branch_id != planned_branch.branch_id) {
                continue;
            }
            Intent remapped = intent;
            remapped.branch_id = 0;
            exec_context.intents.push_back(remapped);
            if (remapped.type == IntentType::kRead) {
                exec_context.object_store->RecordRead(exec_context.txn.txn_id, 0,
                    remapped.object_id);
            } else {
                exec_context.object_store->RecordIntent(exec_context.txn.txn_id, 0,
                    remapped);
            }
        }

        if (!is_winner) {
            exec_context.object_store->AbortTxn(exec_context.txn.txn_id);
            result.aborted_txn_count += 1;
            continue;
        }

        TransactionalApplyOp transactional_apply_op;
        TaskResult winner_result = transactional_apply_op.Execute(&exec_context);
        result.txn_id = winner_result.txn_id;
        result.status = winner_result.status;
        result.committed_branch_count = winner_result.committed_branch_count;
        result.discarded_branch_count =
            static_cast<uint32_t>(planning_context.branches.size() - 1);
        result.applied_intent_count = winner_result.applied_intent_count;
        result.validated_read_count = winner_result.validated_read_count;
        result.applied_write_count = winner_result.applied_write_count;
        result.backend_object_count = winner_result.backend_object_count;
        result.backend_planned_loser_count = winner_result.backend_planned_loser_count;
        result.conflict_abort_count = winner_result.conflict_abort_count;
        result.winner_score = winner_result.winner_score;
        result.output_version = winner_result.output_version;
        result.output_payload = winner_result.output_payload;
        result.message = winner_result.message;
        if (winner_result.status == TaskStatus::kCommitted) {
            result.committed_txn_count = 1;
        } else {
            result.aborted_txn_count += 1;
        }
    }

    if (result.status != TaskStatus::kCommitted) {
        result.status = TaskStatus::kAborted;
        if (result.message.empty()) {
            result.message = "Baseline multi-txn winner failed to commit.";
        }
    }
    return result;
}

TaskResult TaskRuntime::SubmitBaselineMultiTxnTaskReal(const TaskSpec &task) const {
    TaskContext planning_context;
    planning_context.spec = task;
    planning_context.txn = txn_manager_.Begin(task, task.task_id);
    CandidateGenerateOp candidate_generate_op;
    BranchEvaluateOp branch_evaluate_op;
    WinnerSelectOp winner_select_op;

    candidate_generate_op.Execute(&planning_context);
    branch_evaluate_op.Execute(&planning_context);
    uint64_t winner_branch_id = winner_select_op.Execute(&planning_context);

    TaskResult result;
    result.task_id = task.task_id;
    result.candidate_count = static_cast<uint32_t>(planning_context.candidates.size());
    result.total_intent_count = static_cast<uint32_t>(planning_context.intents.size());
    result.winner_branch_id = winner_branch_id;
    result.output_object_id = "output:" + std::to_string(task.task_id);
    result.txn_count = static_cast<uint32_t>(planning_context.branches.size());
    result.planned_abort_txn_count =
        result.txn_count > 0 ? result.txn_count - 1 : 0;

    for (uint64_t i = 0; i < planning_context.branches.size(); ++i) {
        const Branch &planned_branch = planning_context.branches[i];
        bool is_winner = planned_branch.branch_id == winner_branch_id;
        TraditionalTxnResult branch_result = DBx1000TestBackend::ExecuteTraditionalBranch(
            planning_context.txn.txn_id + i + 1,
            task,
            planned_branch,
            is_winner,
            is_winner && task.conflict_period > 0 &&
                task.task_id % task.conflict_period == 0);
        if (!is_winner) {
            result.aborted_txn_count += 1;
            continue;
        }

        result.txn_id = planning_context.txn.txn_id + i + 1;
        result.validated_read_count = branch_result.validated_read_count;
        result.applied_write_count = branch_result.applied_write_count;
        result.applied_intent_count =
            branch_result.validated_read_count + branch_result.applied_write_count;
        result.backend_object_count =
            static_cast<uint32_t>(DBx1000TestBackend::ObjectCount());
        result.backend_planned_loser_count =
            static_cast<uint32_t>(planning_context.branches.size() - 1);
        result.winner_score = planned_branch.result.score;
        result.output_version = branch_result.output_version;
        result.output_payload = branch_result.output_payload;
        result.committed_branch_count = branch_result.rc == RCOK ? 1 : 0;
        result.discarded_branch_count =
            static_cast<uint32_t>(planning_context.branches.size() - 1);
        if (branch_result.rc == RCOK) {
            result.status = TaskStatus::kCommitted;
            result.message = "Traditional DBx1000 winner committed.";
            result.committed_txn_count = 1;
        } else {
            result.status = TaskStatus::kAborted;
            result.message = "Traditional DBx1000 winner aborted.";
            result.aborted_txn_count += 1;
            result.conflict_abort_count = 1;
        }
    }

    if (result.status != TaskStatus::kCommitted && result.message.empty()) {
        result.status = TaskStatus::kAborted;
        result.message = "Traditional DBx1000 winner failed to commit.";
    }
    return result;
}

TaskContext TaskRuntime::BuildContext(const TaskSpec &task) const {
    TaskContext context;
    context.spec = task;
    static DBx1000ObjectStore object_store;
    context.object_store = &object_store;
    uint64_t backend_txn_id = context.object_store->BeginAgentTxn(task);
    context.txn = txn_manager_.Begin(task, backend_txn_id);
    return context;
}

}  // namespace data_agent
