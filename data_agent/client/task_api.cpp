#include "data_agent/client/task_api.h"

#include "data_agent/runtime/task_runtime.h"

namespace data_agent {

TaskResult submit_task(const TaskSpec &task, RuntimeMode mode) {
    TaskRuntime runtime;
    return runtime.SubmitTask(task, mode);
}

}  // namespace data_agent
