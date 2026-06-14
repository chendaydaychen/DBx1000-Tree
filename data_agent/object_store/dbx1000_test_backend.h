#pragma once

#include <stdint.h>

#include <string>

#include "data_agent/branch/branch.h"
#include "data_agent/common/types.h"
#include "data_agent/intent/intent.h"
#include "data_agent/object_store/object.h"
#include "global.h"

class TestWorkload;
class thread_t;
class txn_man;
class row_t;

namespace data_agent {

struct TraditionalTxnResult {
    RC rc;
    uint32_t validated_read_count;
    uint32_t applied_write_count;
    uint64_t output_version;
    std::string output_payload;

    TraditionalTxnResult()
        : rc(Abort),
          validated_read_count(0),
          applied_write_count(0),
          output_version(0) {}
};

class DBx1000TestBackend {
public:
    static void Initialize(uint64_t task_count);
    static bool IsInitialized();
    static bool ReadObject(const std::string &object_id, Object *object);
    static bool ForceBumpVersion(const std::string &object_id);
    static uint64_t ObjectCount();

    static txn_man *BeginTxn(uint64_t txn_id);
    static void DestroyTxn(txn_man *txn);
    static bool BeginSemanticBranches(txn_man *txn, uint32_t branch_count);
    static bool BeginSemanticBranch(txn_man *txn, uint32_t branch_id);
    static bool RecordSemanticRead(txn_man *txn, const std::string &object_id);
    static bool RecordSemanticIntent(txn_man *txn, const Intent &intent);
    static RC CommitSemanticWinner(txn_man *txn, uint32_t winner_branch_id);

    static TraditionalTxnResult ExecuteTraditionalBranch(
        uint64_t txn_id,
        const TaskSpec &task,
        const Branch &branch,
        bool should_commit,
        bool inject_conflict);

private:
    static bool InitializeGlobals();
    static bool EnsureRowCount(uint64_t task_count);
    static bool CreateRow(uint64_t primary_key);
    static bool LookupRow(uint64_t primary_key, row_t **row);
};

}  // namespace data_agent
