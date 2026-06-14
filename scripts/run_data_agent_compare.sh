#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TASKS="${TASKS:-50000}"
CANDIDATES="${CANDIDATES:-4}"
CONFLICT_PERIOD="${CONFLICT_PERIOD:-0}"
BACKEND="${BACKEND:-synthetic}"
OUT_DIR="${OUT_DIR:-results/raw/data_agent_compare}"

mkdir -p "$OUT_DIR"
make synthetic >/dev/null

SUMMARY_CSV="$OUT_DIR/summary.csv"
echo "family,task_count,candidate_count,conflict_period,backend,mode,committed_task_count,aborted_task_count,txn_count,committed_txn_count,aborted_txn_count_metric,planned_abort_txn_count,winner_commit_count,planned_loser_count,total_intent_count,applied_intent_count,validated_read_count,applied_write_count,conflict_abort_count,elapsed_ms,avg_latency_ms,throughput,avg_winner_score,avg_output_version,output_validation_ok" > "$SUMMARY_CSV"

for mode in agent_level_txn baseline_multi_txn; do
  case_csv="$OUT_DIR/${mode}.csv"
  ./data_agent_synthetic_runner \
    --tasks "$TASKS" \
    --candidates "$CANDIDATES" \
    --conflict-period "$CONFLICT_PERIOD" \
    --backend "$BACKEND" \
    --mode "$mode" \
    --csv-output "$case_csv"
  family="semantic_cc"
  if [[ "$mode" == "baseline_multi_txn" ]]; then
    family="traditional_cc"
  fi
  tail -n 1 "$case_csv" | sed "s/^/${family},/" >> "$SUMMARY_CSV"
done

echo "summary_csv=$SUMMARY_CSV"
