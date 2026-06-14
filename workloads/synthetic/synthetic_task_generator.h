#pragma once

#include <vector>

#include "data_agent/common/types.h"

namespace data_agent {

class SyntheticTaskGenerator {
public:
    std::vector<TaskSpec> Generate(uint64_t task_count, uint32_t max_candidates,
        uint32_t conflict_period) const;
};

}  // namespace data_agent
