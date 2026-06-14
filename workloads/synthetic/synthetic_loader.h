#pragma once

#include <stdint.h>

#include "data_agent/common/types.h"

namespace data_agent {

class SyntheticLoader {
public:
    void ResetAndLoad(uint64_t task_count, BackendMode backend_mode) const;
};

}  // namespace data_agent
