#include "workloads/synthetic/synthetic_task_generator.h"

namespace data_agent {

std::vector<TaskSpec> SyntheticTaskGenerator::Generate(uint64_t task_count,
        uint32_t max_candidates, uint32_t conflict_period) const {
    std::vector<TaskSpec> tasks;
    tasks.reserve(task_count);
    for (uint64_t i = 0; i < task_count; ++i) {
        TaskSpec task;
        task.task_id = i + 1;
        task.task_type = "synthetic_exploratory_task";
        task.input_objects.push_back("input:" + std::to_string(i + 1));
        task.goal = "maximize_branch_score";
        task.max_candidates = max_candidates;
        task.conflict_period = conflict_period;
        tasks.push_back(task);
    }
    return tasks;
}

}  // namespace data_agent
