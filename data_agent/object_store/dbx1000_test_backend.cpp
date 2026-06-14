#include "data_agent/object_store/dbx1000_test_backend.h"

#include <algorithm>
#include <string.h>

#include "benchmarks/test.h"
#include "catalog.h"
#include "data_agent/object_store/dbx1000_row_bridge.h"
#include "index_hash.h"
#include "manager.h"
#include "mem_alloc.h"
#include "row.h"
#include "table.h"
#include "thread.h"
#include "txn.h"

#if CC_ALG == OCC || IS_OCC_AET
#include "occ.h"
#include "row_occ.h"
#elif IS_AET_HYBRID_CC
#include "row_aet_hybrid.h"
#elif IS_SILO_CC || IS_AET_HYBRID_CC
#include "row_silo.h"
#elif CC_ALG == TICTOC
#include "row_tictoc.h"
#endif

namespace data_agent {
namespace {

bool g_initialized = false;
uint64_t g_row_count = 0;
uint64_t g_next_txn_id = 1;
TestWorkload *g_workload = 0;
thread_t *g_thread = 0;

uint64_t current_row_version(row_t *row) {
#if CC_ALG == OCC || IS_OCC_AET
    return row->manager->get_version();
#elif IS_SILO_CC
    return row->manager->get_version();
#elif IS_AET_HYBRID_CC
    return row->manager->get_version();
#elif CC_ALG == TICTOC
    return row->manager->get_wts();
#else
    (void)row;
    return 0;
#endif
}

std::string decode_field_payload(row_t *row, int col_id) {
    uint32_t field_size = row->get_schema()->get_field_size(col_id);
    const char *field_ptr = row->get_value(col_id);
    std::string payload(field_ptr, field_ptr + field_size);
    while (!payload.empty() && payload[payload.size() - 1] == '\0') {
        payload.erase(payload.size() - 1);
    }
    return payload;
}

std::string encode_field_payload(row_t *row, int col_id, const std::string &payload) {
    uint32_t field_size = row->get_schema()->get_field_size(col_id);
    std::string buffer(field_size, '\0');
    uint32_t copy_size = static_cast<uint32_t>(
        std::min<size_t>(field_size, payload.size()));
    if (copy_size > 0) {
        memcpy(&buffer[0], payload.data(), copy_size);
    }
    return buffer;
}

void overwrite_row_field(row_t *row, int col_id, const std::string &payload) {
#if CC_ALG == OCC || IS_OCC_AET || IS_SILO_CC || IS_AET_HYBRID_CC
    row_t local_row;
    local_row.init(row->get_table(), row->get_part_id());
    local_row.copy(row);
    std::string buffer = encode_field_payload(row, col_id, payload);
    local_row.set_value(col_id, (void *)&buffer[0], buffer.size());
#if CC_ALG == OCC || IS_OCC_AET
    row->manager->latch();
#elif IS_SILO_CC
    row->manager->lock();
#elif IS_AET_HYBRID_CC
    row->manager->lock();
#endif
    uint64_t next_version = current_row_version(row) + 1;
    if (glob_manager != 0 && g_thread != 0) {
        uint64_t ts = glob_manager->get_ts(g_thread->_thd_id);
        if (ts > next_version) {
            next_version = ts;
        }
    }
    row->manager->write(&local_row, next_version);
    row->manager->release();
    local_row.free_row();
#elif CC_ALG == TICTOC
    row_t local_row;
    local_row.init(row->get_table(), row->get_part_id());
    local_row.copy(row);
    std::string buffer = encode_field_payload(row, col_id, payload);
    local_row.set_value(col_id, (void *)&buffer[0], buffer.size());
    row->manager->lock();
    uint64_t next_version = current_row_version(row) + 1;
    if (glob_manager != 0 && g_thread != 0) {
        uint64_t ts = glob_manager->get_ts(g_thread->_thd_id);
        if (ts > next_version) {
            next_version = ts;
        }
    }
    row->manager->write_data(&local_row, next_version);
    row->manager->release();
    local_row.free_row();
#else
    (void)row;
    (void)col_id;
    (void)payload;
#endif
}

bool lookup_binding(const std::string &object_id, DBx1000RowBinding *binding) {
    return DBx1000RowBridge::LookupBinding(object_id, binding);
}

}  // namespace

void DBx1000TestBackend::Initialize(uint64_t task_count) {
    if (!InitializeGlobals()) {
        return;
    }
    EnsureRowCount(task_count);
}

bool DBx1000TestBackend::IsInitialized() {
    return g_initialized;
}

bool DBx1000TestBackend::ReadObject(const std::string &object_id, Object *object) {
    if (object == 0) {
        return false;
    }
    DBx1000RowBinding binding;
    if (!lookup_binding(object_id, &binding)) {
        return false;
    }
    object->object_id = object_id;
    object->object_type = binding.object_type;
    object->payload = decode_field_payload(binding.row, binding.col_id);
    object->version = current_row_version(binding.row);
    return true;
}

bool DBx1000TestBackend::ForceBumpVersion(const std::string &object_id) {
    DBx1000RowBinding binding;
    if (!lookup_binding(object_id, &binding)) {
        return false;
    }
    overwrite_row_field(binding.row, binding.col_id,
        decode_field_payload(binding.row, binding.col_id));
    return true;
}

uint64_t DBx1000TestBackend::ObjectCount() {
    return g_row_count * 2;
}

txn_man *DBx1000TestBackend::BeginTxn(uint64_t txn_id) {
    if (!g_initialized) {
        return 0;
    }
    txn_man *txn = 0;
    if (g_workload->get_txn_man(txn, g_thread) != RCOK || txn == 0) {
        return 0;
    }
    glob_manager->set_txn_man(txn);
    txn->set_txn_id(txn_id == 0 ? g_next_txn_id++ : txn_id);
    txn->start_ts = glob_manager->get_ts(g_thread->_thd_id);
    return txn;
}

void DBx1000TestBackend::DestroyTxn(txn_man *txn) {
    if (txn == 0) {
        return;
    }
    txn->release();
    mem_allocator.free(txn, sizeof(TestTxnMan));
}

bool DBx1000TestBackend::BeginSemanticBranches(txn_man *txn, uint32_t branch_count) {
#if IS_AET_CC
    return txn != 0 && txn->begin_agent_branches(branch_count) == RCOK;
#else
    (void)txn;
    (void)branch_count;
    return false;
#endif
}

bool DBx1000TestBackend::BeginSemanticBranch(txn_man *txn, uint32_t branch_id) {
#if IS_AET_CC
    return txn != 0 && txn->begin_agent_branch(branch_id) == RCOK;
#else
    (void)txn;
    (void)branch_id;
    return false;
#endif
}

bool DBx1000TestBackend::RecordSemanticRead(txn_man *txn, const std::string &object_id) {
#if IS_AET_CC
    if (txn == 0) {
        return false;
    }
    DBx1000RowBinding binding;
    if (!lookup_binding(object_id, &binding)) {
        return false;
    }
    return txn->record_agent_read_intent(
        binding.row, binding.col_id, AGENT_READ_STRICT) == RCOK;
#else
    (void)txn;
    (void)object_id;
    return false;
#endif
}

bool DBx1000TestBackend::RecordSemanticIntent(txn_man *txn, const Intent &intent) {
#if IS_AET_CC
    if (txn == 0) {
        return false;
    }
    DBx1000RowBinding binding;
    if (!lookup_binding(intent.object_id, &binding)) {
        return false;
    }
    if (intent.type != IntentType::kOverwrite) {
        return false;
    }
    std::string buffer = encode_field_payload(binding.row, binding.col_id, intent.payload);
    return txn->record_agent_xwrite_intent(binding.row, binding.col_id,
        &buffer[0], static_cast<uint32_t>(buffer.size())) == RCOK;
#else
    (void)txn;
    (void)intent;
    return false;
#endif
}

RC DBx1000TestBackend::CommitSemanticWinner(txn_man *txn, uint32_t winner_branch_id) {
#if IS_AET_CC
    if (txn == 0) {
        return Abort;
    }
    RC rc = txn->select_agent_winner(winner_branch_id, false);
    return txn->finish(rc);
#else
    (void)txn;
    (void)winner_branch_id;
    return Abort;
#endif
}

TraditionalTxnResult DBx1000TestBackend::ExecuteTraditionalBranch(
        uint64_t txn_id,
        const TaskSpec &task,
        const Branch &branch,
        bool should_commit,
        bool inject_conflict) {
    TraditionalTxnResult result;
    txn_man *txn = BeginTxn(txn_id);
    if (txn == 0) {
        return result;
    }

    DBx1000RowBinding input_binding;
    DBx1000RowBinding output_binding;
    std::string input_object_id = "input:" + std::to_string(task.task_id);
    std::string output_object_id = "output:" + std::to_string(task.task_id);
    if (!lookup_binding(input_object_id, &input_binding) ||
            !lookup_binding(output_object_id, &output_binding)) {
        DestroyTxn(txn);
        return result;
    }

    row_t *row_local = txn->get_row(input_binding.row, WR);
    if (row_local == 0) {
        DestroyTxn(txn);
        return result;
    }

    std::string payload = branch.result.summary + "|score=" +
        std::to_string(branch.result.score);
    std::string buffer = encode_field_payload(output_binding.row,
        output_binding.col_id, payload);
    row_local->set_value(output_binding.col_id, (void *)&buffer[0], buffer.size());
    result.validated_read_count = 1;
    result.applied_write_count = 1;

    if (inject_conflict) {
        ForceBumpVersion(input_object_id);
    }

    result.rc = txn->finish(should_commit ? RCOK : Abort);
    if (result.rc == RCOK) {
        Object output_object;
        if (ReadObject(output_object_id, &output_object)) {
            result.output_version = output_object.version;
            result.output_payload = output_object.payload;
        }
    }

    DestroyTxn(txn);
    return result;
}

bool DBx1000TestBackend::InitializeGlobals() {
    if (g_initialized) {
        return true;
    }

    mem_allocator.init(g_part_cnt, MEM_SIZE / g_part_cnt);
    stats.init();
    glob_manager = (Manager *)_mm_malloc(sizeof(Manager), 64);
    glob_manager->init();
#if CC_ALG == OCC || IS_OCC_AET
    occ_man.init();
#endif
    mem_allocator.register_thread(0);
    stats.init(0);

    g_workload = new TestWorkload();
    g_workload->init();
    g_thread = (thread_t *)_mm_malloc(sizeof(thread_t), 64);
    g_thread->_thd_id = 0;
    g_thread->_wl = g_workload;
    g_row_count = 10;
    DBx1000RowBridge::ClearBindings();
    g_initialized = true;
    return true;
}

bool DBx1000TestBackend::EnsureRowCount(uint64_t task_count) {
    if (task_count <= g_row_count) {
        return true;
    }

    for (uint64_t primary_key = g_row_count; primary_key < task_count; ++primary_key) {
        if (!CreateRow(primary_key)) {
            return false;
        }
    }
    for (uint64_t primary_key = 0; primary_key < task_count; ++primary_key) {
        row_t *row = 0;
        if (!LookupRow(primary_key, &row) || row == 0) {
            return false;
        }
        DBx1000RowBridge::RegisterBinding(
            "input:" + std::to_string(primary_key + 1), row, 0, ObjectType::kDataRecord);
        DBx1000RowBridge::RegisterBinding(
            "output:" + std::to_string(primary_key + 1), row, 3, ObjectType::kDataRecord);
    }
    g_row_count = task_count;
    return true;
}

bool DBx1000TestBackend::CreateRow(uint64_t primary_key) {
    row_t *new_row = 0;
    uint64_t row_id = 0;
    RC rc = g_workload->the_table->get_new_row(new_row, 0, row_id);
    if (rc != RCOK || new_row == 0) {
        return false;
    }
    new_row->set_primary_key(primary_key);
    new_row->set_value(0, static_cast<SInt32>(primary_key + 1));
    new_row->set_value(1, 0.0);
    new_row->set_value(2, static_cast<uint64_t>(10));
    std::string empty_payload(new_row->get_schema()->get_field_size(3), '\0');
    new_row->set_value(3, (void *)&empty_payload[0], empty_payload.size());

    itemid_t *item = (itemid_t *)mem_allocator.alloc(sizeof(itemid_t), 0);
    item->type = DT_row;
    item->location = new_row;
    item->valid = true;
    return g_workload->the_index->index_insert(primary_key, item, 0) == RCOK;
}

bool DBx1000TestBackend::LookupRow(uint64_t primary_key, row_t **row) {
    if (row == 0) {
        return false;
    }
    itemid_t *item = 0;
    g_workload->the_index->index_read(primary_key, item, 0, 0);
    if (item == 0 || item->location == 0) {
        return false;
    }
    *row = (row_t *)item->location;
    return true;
}

}  // namespace data_agent
