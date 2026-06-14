#pragma once

#include <stdint.h>

#include <map>
#include <string>

#include "data_agent/object_store/object.h"

class row_t;

namespace data_agent {

struct DBx1000RowBinding {
    row_t *row;
    int col_id;
    ObjectType object_type;

    DBx1000RowBinding()
        : row(0), col_id(0), object_type(ObjectType::kDataRecord) {}
};

class DBx1000RowBridge {
public:
    static void ClearBindings();
    static bool RegisterBinding(const std::string &object_id, row_t *row,
        int col_id, ObjectType object_type);
    static bool LookupBinding(const std::string &object_id, DBx1000RowBinding *binding);
    static bool ReadBoundObject(const std::string &object_id, Object *object);
    static bool OverwriteBoundObject(const std::string &object_id,
        const std::string &payload);
    static bool ForceBumpVersion(const std::string &object_id);
    static bool ValidateBoundVersion(const std::string &object_id, uint64_t version);

private:
    static std::map<std::string, DBx1000RowBinding> bindings_;
};

}  // namespace data_agent
