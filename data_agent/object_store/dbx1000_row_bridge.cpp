#include "data_agent/object_store/dbx1000_row_bridge.h"

#include <algorithm>
#include <string.h>

#ifndef DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
#define DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE 0
#endif

#if DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
#include "catalog.h"
#include "row_occ.h"
#include "row_silo.h"
#include "row_aet_hybrid.h"
#include "row.h"
#endif

namespace data_agent {

#if DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
namespace {

uint64_t current_semantic_version(row_t *row, int col_id) {
#if IS_AET_HYBRID_CC
    return row->manager->get_col_version(col_id);
#else
    (void)col_id;
    return row->manager->get_version();
#endif
}

bool validate_semantic_version(row_t *row, int col_id, uint64_t version) {
#if IS_AET_HYBRID_CC
    return row->manager->validate_col(col_id, version);
#else
    (void)col_id;
    return row->manager->get_version() == version;
#endif
}

void lock_semantic_row(row_t *row) {
#if CC_ALG == AET_HYBRID_SILO || IS_AET_HYBRID_CC
    row->manager->lock();
#else
    row->manager->latch();
#endif
}

void release_semantic_row(row_t *row) {
    row->manager->release();
}

void write_semantic_row(row_t *row, row_t *local_row, uint64_t version) {
#if IS_AET_HYBRID_CC
    row->manager->write(local_row, version);
#elif IS_SILO_CC
    row->manager->write(local_row, version);
#elif CC_ALG == OCC || (IS_OCC_AET && !IS_AET_HYBRID_CC)
    row->manager->write(local_row, version);
#else
    row->copy(local_row);
    (void)version;
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

void encode_field_payload(row_t *row, int col_id, const std::string &payload) {
    uint32_t field_size = row->get_schema()->get_field_size(col_id);
    std::string buffer(field_size, '\0');
    uint32_t copy_size = static_cast<uint32_t>(std::min<size_t>(field_size, payload.size()));
    if (copy_size > 0) {
        memcpy(&buffer[0], payload.data(), copy_size);
    }
    row->set_value(col_id, &buffer[0], field_size);
}

}  // namespace
#endif

std::map<std::string, DBx1000RowBinding> DBx1000RowBridge::bindings_;

void DBx1000RowBridge::ClearBindings() {
    bindings_.clear();
}

bool DBx1000RowBridge::RegisterBinding(const std::string &object_id, row_t *row,
        int col_id, ObjectType object_type) {
    if (row == 0) {
        return false;
    }
    DBx1000RowBinding binding;
    binding.row = row;
    binding.col_id = col_id;
    binding.object_type = object_type;
    bindings_[object_id] = binding;
    return true;
}

bool DBx1000RowBridge::LookupBinding(const std::string &object_id,
        DBx1000RowBinding *binding) {
    std::map<std::string, DBx1000RowBinding>::const_iterator it = bindings_.find(object_id);
    if (it == bindings_.end()) {
        return false;
    }
    if (binding != 0) {
        *binding = it->second;
    }
    return true;
}

bool DBx1000RowBridge::ReadBoundObject(const std::string &object_id, Object *object) {
#if !DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
    (void)object_id;
    (void)object;
    return false;
#else
    if (object == 0) {
        return false;
    }
    DBx1000RowBinding binding;
    if (!LookupBinding(object_id, &binding)) {
        return false;
    }
    object->object_id = object_id;
    object->object_type = binding.object_type;
    object->payload = decode_field_payload(binding.row, binding.col_id);
    object->version = current_semantic_version(binding.row, binding.col_id);
    return true;
#endif
}

bool DBx1000RowBridge::OverwriteBoundObject(const std::string &object_id,
        const std::string &payload) {
#if !DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
    (void)object_id;
    (void)payload;
    return false;
#else
    DBx1000RowBinding binding;
    if (!LookupBinding(object_id, &binding)) {
        return false;
    }

    lock_semantic_row(binding.row);
    uint64_t next_version = current_semantic_version(binding.row, binding.col_id) + 1;
    row_t local_row;
    local_row.init(binding.row->get_table(), binding.row->get_part_id());
    local_row.copy(binding.row);
    encode_field_payload(&local_row, binding.col_id, payload);
    write_semantic_row(binding.row, &local_row, next_version);
    release_semantic_row(binding.row);
    local_row.free_row();
    return true;
#endif
}

bool DBx1000RowBridge::ForceBumpVersion(const std::string &object_id) {
#if !DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
    (void)object_id;
    return false;
#else
    DBx1000RowBinding binding;
    if (!LookupBinding(object_id, &binding)) {
        return false;
    }

    lock_semantic_row(binding.row);
    uint64_t next_version = current_semantic_version(binding.row, binding.col_id) + 1;
    row_t local_row;
    local_row.init(binding.row->get_table(), binding.row->get_part_id());
    local_row.copy(binding.row);
    write_semantic_row(binding.row, &local_row, next_version);
    release_semantic_row(binding.row);
    local_row.free_row();
    return true;
#endif
}

bool DBx1000RowBridge::ValidateBoundVersion(const std::string &object_id,
        uint64_t version) {
#if !DATA_AGENT_ENABLE_DBX1000_ROW_BRIDGE
    (void)object_id;
    (void)version;
    return false;
#else
    DBx1000RowBinding binding;
    if (!LookupBinding(object_id, &binding)) {
        return false;
    }
    return validate_semantic_version(binding.row, binding.col_id, version);
#endif
}

}  // namespace data_agent
