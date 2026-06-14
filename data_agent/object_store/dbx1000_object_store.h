#pragma once

#include <map>
#include <string>
#include <vector>

#include "data_agent/object_store/object_store.h"

class row_t;
class txn_man;

namespace data_agent {

class DBx1000ObjectStore : public ObjectStore {
public:
    DBx1000ObjectStore();

    bool Get(const std::string &object_id, Object *object);
    bool Put(const Object &object);
    bool ApplyIntent(const Intent &intent);
    uint64_t ObjectCount() const;
    uint64_t BeginAgentTxn(const TaskSpec &task);
    bool CreateBranch(uint64_t txn_id, uint32_t branch_id,
        const std::string &candidate_id);
    bool RecordRead(uint64_t txn_id, uint32_t branch_id,
        const std::string &object_id);
    bool RecordIntent(uint64_t txn_id, uint32_t branch_id,
        const Intent &intent);
    bool SelectWinner(uint64_t txn_id, uint32_t branch_id);
    BackendCommitResult CommitWinner(uint64_t txn_id);
    void AbortTxn(uint64_t txn_id);
    bool ForceBumpVersion(const std::string &object_id);
    static void ClearRowBindings();
    static bool RegisterRowBinding(const std::string &object_id, row_t *row,
        int col_id, ObjectType object_type);

    static void Reset();
    static bool Seed(const Object &object);

private:
    struct BackendReadIntent {
        std::string object_id;
        bool observed_exists;
        uint64_t observed_version;

        BackendReadIntent()
            : observed_exists(false), observed_version(0) {}
    };

    struct BackendBranch {
        uint32_t branch_id;
        bool released;
        std::string candidate_id;
        std::vector<BackendReadIntent> reads;
        std::vector<Intent> intents;

        BackendBranch()
            : branch_id(0), released(false) {}
    };

    struct BackendTxn {
        uint64_t txn_id;
        uint64_t task_id;
        bool use_real_backend;
        bool winner_selected;
        uint32_t winner_branch_id;
        txn_man *real_txn;
        std::map<uint32_t, BackendBranch> branches;

        BackendTxn()
            : txn_id(0),
              task_id(0),
              use_real_backend(false),
              winner_selected(false),
              winner_branch_id(0),
              real_txn(0) {}
    };

    static uint64_t next_txn_id_;
    static std::map<std::string, Object> objects_;
    static std::map<uint64_t, BackendTxn> txns_;

    static bool ApplyIntentToObjects(const Intent &intent);
};

}  // namespace data_agent
