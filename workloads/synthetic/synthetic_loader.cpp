#include "workloads/synthetic/synthetic_loader.h"

#include "data_agent/object_store/dbx1000_object_store.h"
#include "data_agent/object_store/dbx1000_test_backend.h"

namespace data_agent {

void SyntheticLoader::ResetAndLoad(uint64_t task_count, BackendMode backend_mode) const {
    if (backend_mode == BackendMode::kDbx1000Test) {
        DBx1000ObjectStore::ClearRowBindings();
        DBx1000TestBackend::Initialize(task_count);
        return;
    }
    DBx1000ObjectStore::Reset();
    for (uint64_t i = 0; i < task_count; ++i) {
        Object input_object;
        input_object.object_id = "input:" + std::to_string(i + 1);
        input_object.object_type = ObjectType::kDataRecord;
        input_object.payload = "seed_input_" + std::to_string(i + 1);
        input_object.version = 0;
        DBx1000ObjectStore::Seed(input_object);
    }
}

}  // namespace data_agent
