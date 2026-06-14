#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TASKS="${TASKS:-50000}"
CANDIDATES="${CANDIDATES:-1,2,4,8}"
CONFLICT_PERIOD="${CONFLICT_PERIOD:-0}"
BACKEND="${BACKEND:-synthetic}"
OUT_DIR="${OUT_DIR:-results/raw/data_agent_synthetic}"

mkdir -p "$OUT_DIR"

make synthetic >/dev/null

SUMMARY_CSV="$OUT_DIR/summary.csv"
echo "task_count,candidate_count,conflict_period,backend,mode,committed_task_count,aborted_task_count,txn_count,committed_txn_count,aborted_txn_count_metric,planned_abort_txn_count,winner_commit_count,planned_loser_count,total_intent_count,applied_intent_count,validated_read_count,applied_write_count,conflict_abort_count,elapsed_ms,avg_latency_ms,throughput,avg_winner_score,avg_output_version,output_validation_ok" > "$SUMMARY_CSV"

IFS=',' read -r -a candidate_list <<< "$CANDIDATES"
for candidate_count in "${candidate_list[@]}"; do
  candidate_count="${candidate_count//[[:space:]]/}"
  [[ -z "$candidate_count" ]] && continue
  CASE_CSV="$OUT_DIR/candidates_${candidate_count}.csv"
  ./data_agent_synthetic_runner \
    --tasks "$TASKS" \
    --candidates "$candidate_count" \
    --conflict-period "$CONFLICT_PERIOD" \
    --backend "$BACKEND" \
    --csv-output "$CASE_CSV"
  tail -n 1 "$CASE_CSV" >> "$SUMMARY_CSV"
done

echo "summary_csv=$SUMMARY_CSV"
