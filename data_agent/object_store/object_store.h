#pragma once

#include <string>

#include "data_agent/common/types.h"
#include "data_agent/intent/intent.h"
#include "data_agent/object_store/object.h"

namespace data_agent {

struct BackendCommitResult {
    bool ok;
    std::string error;
    uint64_t txn_id;
    uint64_t winner_branch_id;
    uint32_t committed_intent_count;
    uint32_t validated_read_count;
    uint32_t applied_write_count;
    uint32_t planned_loser_count;
    uint32_t conflict_abort_count;

    BackendCommitResult()
        : ok(false),
          txn_id(0),
          winner_branch_id(0),
          committed_intent_count(0),
          validated_read_count(0),
          applied_write_count(0),
          planned_loser_count(0),
          conflict_abort_count(0) {}
};

class ObjectStore {
public:
    virtual ~ObjectStore() {}

    virtual bool Get(const std::string &object_id, Object *object) = 0;
    virtual bool Put(const Object &object) = 0;
    virtual bool ApplyIntent(const Intent &intent) = 0;
    virtual uint64_t ObjectCount() const = 0;
    virtual uint64_t BeginAgentTxn(const TaskSpec &task) = 0;
    virtual bool CreateBranch(uint64_t txn_id, uint32_t branch_id,
        const std::string &candidate_id) = 0;
    virtual bool RecordRead(uint64_t txn_id, uint32_t branch_id,
        const std::string &object_id) = 0;
    virtual bool RecordIntent(uint64_t txn_id, uint32_t branch_id,
        const Intent &intent) = 0;
    virtual bool SelectWinner(uint64_t txn_id, uint32_t branch_id) = 0;
    virtual BackendCommitResult CommitWinner(uint64_t txn_id) = 0;
    virtual void AbortTxn(uint64_t txn_id) = 0;
    virtual bool ForceBumpVersion(const std::string &object_id) = 0;
};

}  // namespace data_agent
