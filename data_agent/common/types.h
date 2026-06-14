#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace data_agent {

enum class TaskStatus {
    kPending,
    kRunning,
    kCommitted,
    kAborted,
    kNotImplemented,
};

enum class RuntimeMode {
    kAgentLevelTxn,
    kBaselineMultiTxn,
};

enum class BackendMode {
    kSynthetic,
    kDbx1000Test,
};

struct TaskSpec {
    uint64_t task_id;
    std::string task_type;
    std::vector<std::string> input_objects;
    std::string goal;
    uint32_t max_candidates;
    uint32_t conflict_period;
    BackendMode backend_mode;

    TaskSpec()
        : task_id(0),
          max_candidates(1),
          conflict_period(0),
          backend_mode(BackendMode::kSynthetic) {}
};

struct CandidateSpec {
    uint64_t candidate_id;
    std::string summary;

    CandidateSpec()
        : candidate_id(0) {}
};

struct TaskResult {
    uint64_t task_id;
    TaskStatus status;
    uint64_t txn_id;
    uint64_t winner_branch_id;
    uint32_t candidate_count;
    uint32_t committed_branch_count;
    uint32_t discarded_branch_count;
    uint32_t total_intent_count;
    uint32_t applied_intent_count;
    uint32_t validated_read_count;
    uint32_t applied_write_count;
    uint32_t backend_object_count;
    uint32_t backend_planned_loser_count;
    uint32_t conflict_abort_count;
    uint32_t txn_count;
    uint32_t committed_txn_count;
    uint32_t aborted_txn_count;
    uint32_t planned_abort_txn_count;
    double winner_score;
    uint64_t output_version;
    std::string output_object_id;
    std::string output_payload;
    std::string message;

    TaskResult()
        : task_id(0),
          status(TaskStatus::kPending),
          txn_id(0),
          winner_branch_id(0),
          candidate_count(0),
          committed_branch_count(0),
          discarded_branch_count(0),
          total_intent_count(0),
          applied_intent_count(0),
          validated_read_count(0),
          applied_write_count(0),
          backend_object_count(0),
          backend_planned_loser_count(0),
          conflict_abort_count(0),
          txn_count(0),
          committed_txn_count(0),
          aborted_txn_count(0),
          planned_abort_txn_count(0),
          winner_score(0.0),
          output_version(0) {}
};

}  // namespace data_agent
