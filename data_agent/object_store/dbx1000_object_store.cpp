#include "data_agent/object_store/dbx1000_object_store.h"

#include "data_agent/object_store/dbx1000_row_bridge.h"
#include "data_agent/object_store/dbx1000_test_backend.h"

namespace data_agent {

uint64_t DBx1000ObjectStore::next_txn_id_ = 1;
std::map<std::string, Object> DBx1000ObjectStore::objects_;
std::map<uint64_t, DBx1000ObjectStore::BackendTxn> DBx1000ObjectStore::txns_;

DBx1000ObjectStore::DBx1000ObjectStore() {}

bool DBx1000ObjectStore::Get(const std::string &object_id, Object *object) {
    if (object == 0) {
        return false;
    }
    if (DBx1000TestBackend::ReadObject(object_id, object)) {
        return true;
    }
    if (DBx1000RowBridge::ReadBoundObject(object_id, object)) {
        return true;
    }
    std::map<std::string, Object>::const_iterator it = objects_.find(object_id);
    if (it == objects_.end()) {
        return false;
    }
    *object = it->second;
    return true;
}

bool DBx1000ObjectStore::Put(const Object &object) {
    objects_[object.object_id] = object;
    return true;
}

bool DBx1000ObjectStore::ApplyIntent(const Intent &intent) {
    return ApplyIntentToObjects(intent);
}

uint64_t DBx1000ObjectStore::ObjectCount() const {
    if (DBx1000TestBackend::IsInitialized()) {
        return DBx1000TestBackend::ObjectCount();
    }
    return static_cast<uint64_t>(objects_.size());
}

uint64_t DBx1000ObjectStore::BeginAgentTxn(const TaskSpec &task) {
    uint64_t txn_id = next_txn_id_++;
    BackendTxn txn;
    txn.txn_id = txn_id;
    txn.task_id = task.task_id;
    for (uint32_t i = 0; i < task.max_candidates; ++i) {
        BackendBranch branch;
        branch.branch_id = i;
        txn.branches[i] = branch;
    }
    txn.use_real_backend = task.backend_mode == BackendMode::kDbx1000Test;
    if (txn.use_real_backend) {
        txn.real_txn = DBx1000TestBackend::BeginTxn(txn_id);
        if (txn.real_txn != 0) {
            DBx1000TestBackend::BeginSemanticBranches(
                txn.real_txn, task.max_candidates);
        }
    }
    txns_[txn_id] = txn;
    return txn_id;
}

bool DBx1000ObjectStore::CreateBranch(uint64_t txn_id, uint32_t branch_id,
        const std::string &candidate_id) {
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it == txns_.end()) {
        return false;
    }
    BackendBranch &branch = txn_it->second.branches[branch_id];
    branch.branch_id = branch_id;
    branch.released = false;
    branch.candidate_id = candidate_id;
    branch.reads.clear();
    branch.intents.clear();
    if (txn_it->second.use_real_backend && txn_it->second.real_txn != 0) {
        return DBx1000TestBackend::BeginSemanticBranch(
            txn_it->second.real_txn, branch_id);
    }
    return true;
}

bool DBx1000ObjectStore::RecordRead(uint64_t txn_id, uint32_t branch_id,
        const std::string &object_id) {
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it == txns_.end()) {
        return false;
    }
    std::map<uint32_t, BackendBranch>::iterator branch_it =
        txn_it->second.branches.find(branch_id);
    if (branch_it == txn_it->second.branches.end()) {
        return false;
    }
    BackendReadIntent read;
    read.object_id = object_id;
    Object object;
    read.observed_exists = Get(object_id, &object);
    read.observed_version = read.observed_exists ? object.version : 0;
    branch_it->second.reads.push_back(read);
    if (txn_it->second.use_real_backend && txn_it->second.real_txn != 0) {
        return DBx1000TestBackend::RecordSemanticRead(
            txn_it->second.real_txn, object_id);
    }
    return true;
}

bool DBx1000ObjectStore::RecordIntent(uint64_t txn_id, uint32_t branch_id,
        const Intent &intent) {
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it == txns_.end()) {
        return false;
    }
    std::map<uint32_t, BackendBranch>::iterator branch_it =
        txn_it->second.branches.find(branch_id);
    if (branch_it == txn_it->second.branches.end()) {
        return false;
    }
    branch_it->second.intents.push_back(intent);
    if (txn_it->second.use_real_backend && txn_it->second.real_txn != 0) {
        return DBx1000TestBackend::RecordSemanticIntent(
            txn_it->second.real_txn, intent);
    }
    return true;
}

bool DBx1000ObjectStore::SelectWinner(uint64_t txn_id, uint32_t branch_id) {
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it == txns_.end()) {
        return false;
    }
    if (txn_it->second.branches.find(branch_id) == txn_it->second.branches.end()) {
        return false;
    }
    txn_it->second.winner_selected = true;
    txn_it->second.winner_branch_id = branch_id;
    return true;
}

BackendCommitResult DBx1000ObjectStore::CommitWinner(uint64_t txn_id) {
    BackendCommitResult result;
    result.txn_id = txn_id;
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it == txns_.end()) {
        result.error = "txn_not_found";
        return result;
    }

    BackendTxn &txn = txn_it->second;
    if (!txn.winner_selected) {
        result.error = "winner_not_selected";
        txns_.erase(txn_it);
        return result;
    }

    result.winner_branch_id = txn.winner_branch_id;
    if (txn.use_real_backend && txn.real_txn != 0) {
        RC rc = DBx1000TestBackend::CommitSemanticWinner(
            txn.real_txn, txn.winner_branch_id);
        for (std::map<uint32_t, BackendBranch>::iterator it = txn.branches.begin();
                it != txn.branches.end(); ++it) {
            if (it->first == txn.winner_branch_id) {
                result.validated_read_count =
                    static_cast<uint32_t>(it->second.reads.size());
                result.committed_intent_count =
                    static_cast<uint32_t>(it->second.intents.size());
                result.applied_write_count =
                    static_cast<uint32_t>(it->second.intents.size());
                continue;
            }
            if (!it->second.released) {
                it->second.released = true;
                result.planned_loser_count += 1;
            }
        }
        if (rc != RCOK) {
            result.error = "real_txn_abort";
            result.conflict_abort_count = 1;
            DBx1000TestBackend::DestroyTxn(txn.real_txn);
            txns_.erase(txn_it);
            return result;
        }
        result.ok = true;
        DBx1000TestBackend::DestroyTxn(txn.real_txn);
        txns_.erase(txn_it);
        return result;
    }

    BackendBranch &winner = txn.branches[txn.winner_branch_id];
    for (uint64_t i = 0; i < winner.reads.size(); ++i) {
        const BackendReadIntent &read = winner.reads[i];
        std::map<std::string, Object>::const_iterator object_it = objects_.find(read.object_id);
        bool exists = object_it != objects_.end();
        uint64_t version = exists ? object_it->second.version : 0;
        if (exists != read.observed_exists || version != read.observed_version) {
            result.error = "read_version_changed";
            result.conflict_abort_count = 1;
            txns_.erase(txn_it);
            return result;
        }
        result.validated_read_count += 1;
    }

    for (std::map<uint32_t, BackendBranch>::iterator it = txn.branches.begin();
            it != txn.branches.end(); ++it) {
        if (it->first == txn.winner_branch_id) {
            continue;
        }
        if (!it->second.released) {
            it->second.released = true;
            result.planned_loser_count += 1;
        }
    }

    for (uint64_t i = 0; i < winner.intents.size(); ++i) {
        if (!ApplyIntentToObjects(winner.intents[i])) {
            result.error = "intent_apply_failed";
            txns_.erase(txn_it);
            return result;
        }
        result.committed_intent_count += 1;
        if (winner.intents[i].type != IntentType::kRead) {
            result.applied_write_count += 1;
        }
    }

    result.ok = true;
    txns_.erase(txn_it);
    return result;
}

void DBx1000ObjectStore::AbortTxn(uint64_t txn_id) {
    std::map<uint64_t, BackendTxn>::iterator txn_it = txns_.find(txn_id);
    if (txn_it != txns_.end() && txn_it->second.use_real_backend &&
            txn_it->second.real_txn != 0) {
        DBx1000TestBackend::DestroyTxn(txn_it->second.real_txn);
    }
    txns_.erase(txn_id);
}

bool DBx1000ObjectStore::ForceBumpVersion(const std::string &object_id) {
    if (DBx1000TestBackend::ForceBumpVersion(object_id)) {
        return true;
    }
    if (DBx1000RowBridge::ForceBumpVersion(object_id)) {
        return true;
    }
    Object current;
    if (!Get(object_id, &current)) {
        return false;
    }
    current.version += 1;
    objects_[object_id] = current;
    return true;
}

void DBx1000ObjectStore::Reset() {
    next_txn_id_ = 1;
    objects_.clear();
    txns_.clear();
    DBx1000RowBridge::ClearBindings();
}

bool DBx1000ObjectStore::Seed(const Object &object) {
    objects_[object.object_id] = object;
    return true;
}

bool DBx1000ObjectStore::ApplyIntentToObjects(const Intent &intent) {
    if (intent.type == IntentType::kRead) {
        DBx1000RowBinding binding;
        if (DBx1000RowBridge::LookupBinding(intent.object_id, &binding)) {
            uint64_t version = 0;
            if (intent.condition.empty()) {
                return true;
            }
            version = static_cast<uint64_t>(strtoull(intent.condition.c_str(), 0, 10));
            return DBx1000RowBridge::ValidateBoundVersion(intent.object_id, version);
        }
        std::map<std::string, Object>::const_iterator object_it = objects_.find(intent.object_id);
        if (object_it == objects_.end()) {
            return false;
        }
        return intent.condition.empty() ||
            intent.condition == std::to_string(object_it->second.version);
    }

    Object current;
    std::map<std::string, Object>::iterator object_it = objects_.find(intent.object_id);
    if (object_it != objects_.end()) {
        current = object_it->second;
    } else {
        current.object_id = intent.object_id;
        current.object_type = ObjectType::kDataRecord;
        current.version = 0;
        current.payload.clear();
    }

    if (!intent.condition.empty() &&
            intent.condition != std::to_string(current.version)) {
        return false;
    }

    DBx1000RowBinding binding;
    if (DBx1000RowBridge::LookupBinding(intent.object_id, &binding)) {
        if (intent.type == IntentType::kAppend) {
            Object bound_object;
            if (!DBx1000RowBridge::ReadBoundObject(intent.object_id, &bound_object)) {
                return false;
            }
            return DBx1000RowBridge::OverwriteBoundObject(
                intent.object_id, bound_object.payload + intent.payload);
        }
        return DBx1000RowBridge::OverwriteBoundObject(intent.object_id, intent.payload);
    }

    if (intent.type == IntentType::kAppend) {
        current.payload += intent.payload;
    } else if (intent.type == IntentType::kDelta) {
        current.payload = intent.payload;
    } else if (intent.type == IntentType::kCas ||
            intent.type == IntentType::kOverwrite) {
        current.payload = intent.payload;
    } else {
        return false;
    }
    current.version += 1;
    objects_[intent.object_id] = current;
    return true;
}

void DBx1000ObjectStore::ClearRowBindings() {
    DBx1000RowBridge::ClearBindings();
}

bool DBx1000ObjectStore::RegisterRowBinding(const std::string &object_id,
        row_t *row, int col_id, ObjectType object_type) {
    return DBx1000RowBridge::RegisterBinding(object_id, row, col_id, object_type);
}

}  // namespace data_agent
