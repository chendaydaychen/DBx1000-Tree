#pragma once

#include <stdint.h>

#include <string>

namespace data_agent {

enum class IntentType {
    kRead,
    kAppend,
    kDelta,
    kCas,
    kOverwrite,
};

struct Intent {
    uint64_t intent_id;
    uint64_t txn_id;
    uint64_t branch_id;
    std::string object_id;
    IntentType type;
    std::string payload;
    std::string condition;

    Intent()
        : intent_id(0),
          txn_id(0),
          branch_id(0),
          type(IntentType::kRead) {}
};

}  // namespace data_agent
