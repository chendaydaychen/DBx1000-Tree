#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

THREADS="${THREADS:-4}"
MAX_TXN="${MAX_TXN:-100}"
BRANCHES="${BRANCHES:-1,4,8}"
TRADITIONAL_CC_ALGS="${TRADITIONAL_CC_ALGS:-OCC,SILO,TICTOC,NO_WAIT,MVCC}"
CONFLICT_LEVELS="${CONFLICT_LEVELS:-high:1,medium:4,low:8}"
OUT_DIR="${OUT_DIR:-output/tpcc_semantic_vs_traditional}"
STOP_ON_ATTEMPTS="${STOP_ON_ATTEMPTS:-0}"

mkdir -p "$OUT_DIR"

run_level() {
  local level_name="$1"
  local warehouses="$2"
  local level_dir="$OUT_DIR/$level_name"
  mkdir -p "$level_dir"

  THREADS="$THREADS" \
  MAX_TXN="$MAX_TXN" \
  WAREHOUSES="$warehouses" \
  OUT_DIR="$level_dir" \
  CASES="tpcc_agent_new_order_baseline_all_cc,tpcc_agent_new_order_aet_hybrid_cc_standard_b1,tpcc_agent_new_order_aet_hybrid_cc_standard_b4,tpcc_agent_new_order_aet_hybrid_cc_standard_b8" \
  CC_ALGS="$TRADITIONAL_CC_ALGS" \
  BRANCHES="$BRANCHES" \
  STOP_ON_ATTEMPTS="$STOP_ON_ATTEMPTS" \
  bash scripts/run_tpcc_compare.sh
}

IFS=',' read -r -a levels <<< "$CONFLICT_LEVELS"
for level in "${levels[@]}"; do
  level="${level//[[:space:]]/}"
  [[ -z "$level" ]] && continue
  level_name="${level%%:*}"
  warehouses="${level##*:}"
  run_level "$level_name" "$warehouses"
done

echo "output_dir=$OUT_DIR"
