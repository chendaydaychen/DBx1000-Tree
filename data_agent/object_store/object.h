#pragma once

#include <stdint.h>

#include <string>

namespace data_agent {

enum class ObjectType {
    kTask,
    kDataRecord,
    kCandidateResult,
    kTxnMetadata,
};

struct Object {
    std::string object_id;
    ObjectType object_type;
    std::string payload;
    uint64_t version;

    Object()
        : object_type(ObjectType::kDataRecord), version(0) {}
};

}  // namespace data_agent
