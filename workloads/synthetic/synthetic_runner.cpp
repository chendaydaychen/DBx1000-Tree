#include <stdint.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "data_agent/client/task_api.h"
#include "workloads/synthetic/synthetic_loader.h"
#include "workloads/synthetic/synthetic_task_generator.h"

namespace {

struct RunnerConfig {
    uint64_t task_count;
    uint32_t max_candidates;
    uint32_t conflict_period;
    data_agent::RuntimeMode mode;
    data_agent::BackendMode backend_mode;
    std::string csv_output;

    RunnerConfig()
        : task_count(1000),
          max_candidates(4),
          conflict_period(0),
          mode(data_agent::RuntimeMode::kAgentLevelTxn),
          backend_mode(data_agent::BackendMode::kSynthetic) {}
};

bool parse_uint64(const std::string &value, uint64_t *out) {
    if (out == 0) {
        return false;
    }
    std::istringstream iss(value);
    iss >> *out;
    return !iss.fail();
}

bool parse_uint32(const std::string &value, uint32_t *out) {
    uint64_t parsed = 0;
    if (!parse_uint64(value, &parsed)) {
        return false;
    }
    *out = static_cast<uint32_t>(parsed);
    return true;
}

void print_usage() {
    std::cout
        << "usage: data_agent_synthetic_runner [--tasks N] [--candidates K] [--conflict-period N] [--mode agent_level_txn|baseline_multi_txn] [--backend synthetic|dbx1000_test] [--csv-output PATH]\n";
}

bool parse_mode(const std::string &value, data_agent::RuntimeMode *out) {
    if (value == "agent_level_txn") {
        *out = data_agent::RuntimeMode::kAgentLevelTxn;
        return true;
    }
    if (value == "baseline_multi_txn") {
        *out = data_agent::RuntimeMode::kBaselineMultiTxn;
        return true;
    }
    return false;
}

bool parse_backend_mode(const std::string &value, data_agent::BackendMode *out) {
    if (value == "synthetic") {
        *out = data_agent::BackendMode::kSynthetic;
        return true;
    }
    if (value == "dbx1000_test") {
        *out = data_agent::BackendMode::kDbx1000Test;
        return true;
    }
    return false;
}

bool parse_args(int argc, char **argv, RunnerConfig *config) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--tasks" && i + 1 < argc) {
            ++i;
            if (!parse_uint64(argv[i], &config->task_count)) {
                return false;
            }
        } else if (arg == "--candidates" && i + 1 < argc) {
            ++i;
            if (!parse_uint32(argv[i], &config->max_candidates)) {
                return false;
            }
        } else if (arg == "--csv-output" && i + 1 < argc) {
            ++i;
            config->csv_output = argv[i];
        } else if (arg == "--conflict-period" && i + 1 < argc) {
            ++i;
            if (!parse_uint32(argv[i], &config->conflict_period)) {
                return false;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            ++i;
            if (!parse_mode(argv[i], &config->mode)) {
                return false;
            }
        } else if (arg == "--backend" && i + 1 < argc) {
            ++i;
            if (!parse_backend_mode(argv[i], &config->backend_mode)) {
                return false;
            }
        } else if (arg == "--help") {
            print_usage();
            return false;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    RunnerConfig config;
    if (!parse_args(argc, argv, &config)) {
        print_usage();
        return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 1;
    }

    data_agent::SyntheticTaskGenerator generator;
    std::vector<data_agent::TaskSpec> tasks =
        generator.Generate(config.task_count, config.max_candidates, config.conflict_period);
    for (uint64_t i = 0; i < tasks.size(); ++i) {
        tasks[i].backend_mode = config.backend_mode;
    }

    uint64_t committed_task_count = 0;
    uint64_t aborted_task_count = 0;
    uint64_t winner_commit_count = 0;
    uint64_t planned_loser_count = 0;
    uint64_t total_intent_count = 0;
    uint64_t applied_intent_count = 0;
    uint64_t validated_read_count = 0;
    uint64_t applied_write_count = 0;
    uint64_t output_version_sum = 0;
    uint64_t conflict_abort_count = 0;
    uint64_t txn_count = 0;
    uint64_t committed_txn_count = 0;
    uint64_t aborted_txn_count_metric = 0;
    uint64_t planned_abort_txn_count = 0;
    double winner_score_sum = 0.0;
    bool output_validation_ok = true;

    data_agent::SyntheticLoader loader;
    loader.ResetAndLoad(config.task_count, config.backend_mode);

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < tasks.size(); ++i) {
        data_agent::TaskResult result = data_agent::submit_task(tasks[i], config.mode);
        if (result.status == data_agent::TaskStatus::kCommitted) {
            committed_task_count += 1;
        } else {
            aborted_task_count += 1;
        }
        winner_commit_count += result.committed_branch_count;
        planned_loser_count += result.discarded_branch_count;
        total_intent_count += result.total_intent_count;
        applied_intent_count += result.applied_intent_count;
        validated_read_count += result.validated_read_count;
        applied_write_count += result.applied_write_count;
        output_version_sum += result.output_version;
        conflict_abort_count += result.conflict_abort_count;
        txn_count += result.txn_count;
        committed_txn_count += result.committed_txn_count;
        aborted_txn_count_metric += result.aborted_txn_count;
        planned_abort_txn_count += result.planned_abort_txn_count;
        winner_score_sum += result.winner_score;
        if (result.status == data_agent::TaskStatus::kCommitted) {
            output_validation_ok = output_validation_ok &&
                result.output_version > 0 &&
                !result.output_payload.empty();
        }
    }
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    double elapsed_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli> >(end - begin).count();
    double throughput =
        elapsed_ms > 0.0 ? (static_cast<double>(committed_task_count) * 1000.0 / elapsed_ms) : 0.0;
    double avg_latency_ms =
        committed_task_count > 0 ? (elapsed_ms / static_cast<double>(committed_task_count)) : 0.0;
    double avg_winner_score =
        committed_task_count > 0 ? (winner_score_sum / static_cast<double>(committed_task_count)) : 0.0;
    double avg_output_version =
        committed_task_count > 0 ? (static_cast<double>(output_version_sum) / static_cast<double>(committed_task_count)) : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "task_count=" << config.task_count
              << ", candidate_count=" << config.max_candidates
              << ", conflict_period=" << config.conflict_period
              << ", backend=" << (config.backend_mode == data_agent::BackendMode::kSynthetic ?
                  "synthetic" : "dbx1000_test")
              << ", mode=" << (config.mode == data_agent::RuntimeMode::kAgentLevelTxn ?
                  "agent_level_txn" : "baseline_multi_txn")
              << ", committed_task_count=" << committed_task_count
              << ", aborted_task_count=" << aborted_task_count
              << ", txn_count=" << txn_count
              << ", committed_txn_count=" << committed_txn_count
              << ", aborted_txn_count_metric=" << aborted_txn_count_metric
              << ", planned_abort_txn_count=" << planned_abort_txn_count
              << ", winner_commit_count=" << winner_commit_count
              << ", planned_loser_count=" << planned_loser_count
              << ", total_intent_count=" << total_intent_count
              << ", applied_intent_count=" << applied_intent_count
              << ", validated_read_count=" << validated_read_count
              << ", applied_write_count=" << applied_write_count
              << ", conflict_abort_count=" << conflict_abort_count
              << ", elapsed_ms=" << elapsed_ms
              << ", avg_latency_ms=" << avg_latency_ms
              << ", throughput=" << throughput
              << ", avg_winner_score=" << avg_winner_score
              << ", avg_output_version=" << avg_output_version
              << ", output_validation_ok=" << (output_validation_ok ? 1 : 0)
              << "\n";

    if (!config.csv_output.empty()) {
        std::ofstream csv(config.csv_output.c_str());
        csv << "task_count,candidate_count,conflict_period,backend,mode,committed_task_count,aborted_task_count,txn_count,committed_txn_count,aborted_txn_count_metric,planned_abort_txn_count,winner_commit_count,"
               "planned_loser_count,total_intent_count,applied_intent_count,"
               "validated_read_count,applied_write_count,conflict_abort_count,elapsed_ms,avg_latency_ms,"
               "throughput,avg_winner_score,avg_output_version,output_validation_ok\n";
        csv << config.task_count << ","
            << config.max_candidates << ","
            << config.conflict_period << ","
            << (config.backend_mode == data_agent::BackendMode::kSynthetic ?
                "synthetic" : "dbx1000_test") << ","
            << (config.mode == data_agent::RuntimeMode::kAgentLevelTxn ?
                "agent_level_txn" : "baseline_multi_txn") << ","
            << committed_task_count << ","
            << aborted_task_count << ","
            << txn_count << ","
            << committed_txn_count << ","
            << aborted_txn_count_metric << ","
            << planned_abort_txn_count << ","
            << winner_commit_count << ","
            << planned_loser_count << ","
            << total_intent_count << ","
            << applied_intent_count << ","
            << validated_read_count << ","
            << applied_write_count << ","
            << conflict_abort_count << ","
            << elapsed_ms << ","
            << avg_latency_ms << ","
            << throughput << ","
            << avg_winner_score << ","
            << avg_output_version << ","
            << (output_validation_ok ? 1 : 0) << "\n";
    }

    return 0;
}
