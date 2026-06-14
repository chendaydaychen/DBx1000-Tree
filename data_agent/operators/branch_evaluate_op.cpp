#include "data_agent/operators/branch_evaluate_op.h"

#include "data_agent/object_store/object_store.h"

namespace data_agent {

BranchEvaluateOp::BranchEvaluateOp() {}

void BranchEvaluateOp::Execute(TaskContext *context) const {
    if (context == 0) {
        return;
    }

    BranchManager branch_manager;
    context->branches = branch_manager.CreateBranches(
        context->txn.txn_id,
        static_cast<uint32_t>(context->candidates.size()));
    context->intents.clear();

    uint64_t next_intent_id = 0;
    for (uint64_t i = 0; i < context->branches.size(); ++i) {
        Branch &branch = context->branches[i];
        if (context->object_store != 0) {
            context->object_store->CreateBranch(
                context->txn.txn_id,
                static_cast<uint32_t>(branch.branch_id),
                context->candidates[i].summary);
        }
        branch.state = BranchState::kStaged;
        branch.result.feasible = true;
        branch.result.score = ScoreCandidate(context->spec.task_id, branch.branch_id);
        branch.result.summary = context->candidates[i].summary;

        uint64_t observed_version = 0;
        if (context->object_store != 0 && !context->spec.input_objects.empty()) {
            Object input_object;
            if (context->object_store->Get(context->spec.input_objects[0], &input_object)) {
                observed_version = input_object.version;
            }
        }

        Intent read_intent;
        read_intent.intent_id = next_intent_id++;
        read_intent.txn_id = branch.txn_id;
        read_intent.branch_id = branch.branch_id;
        read_intent.object_id = "input:" + std::to_string(context->spec.task_id);
        read_intent.type = IntentType::kRead;
        read_intent.condition = std::to_string(observed_version);
        context->intents.push_back(read_intent);
        branch.read_intent_ids.push_back(read_intent.intent_id);
        if (context->object_store != 0) {
            context->object_store->RecordRead(
                context->txn.txn_id,
                static_cast<uint32_t>(branch.branch_id),
                read_intent.object_id);
        }

        Intent write_intent;
        write_intent.intent_id = next_intent_id++;
        write_intent.txn_id = branch.txn_id;
        write_intent.branch_id = branch.branch_id;
        write_intent.object_id = "output:" + std::to_string(context->spec.task_id);
        write_intent.type = IntentType::kOverwrite;
        write_intent.payload = branch.result.summary + "|score=" +
            std::to_string(branch.result.score);
        write_intent.condition = "0";
        context->intents.push_back(write_intent);
        branch.write_intent_ids.push_back(write_intent.intent_id);
        if (context->object_store != 0) {
            context->object_store->RecordIntent(
                context->txn.txn_id,
                static_cast<uint32_t>(branch.branch_id),
                write_intent);
        }
    }
}

double BranchEvaluateOp::ScoreCandidate(uint64_t task_id, uint64_t branch_id) const {
    uint64_t mixed = ((task_id + 1) * 1315423911ULL) ^ ((branch_id + 7) * 2654435761ULL);
    return static_cast<double>(mixed % 1000ULL) / 1000.0;
}

}  // namespace data_agent
