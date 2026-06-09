#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

THREADS="${THREADS:-4}"
MAX_TXN="${MAX_TXN:-100}"
WAREHOUSES="${WAREHOUSES:-8}"
OUT_DIR="${OUT_DIR:-output/tpcc_compare}"
CASES="${CASES:-all}"
CC_ALGS="${CC_ALGS:-OCC,NO_WAIT,WAIT_DIE,DL_DETECT,MVCC,HEKATON,TICTOC,SILO}"
BRANCHES="${BRANCHES:-1,4,8}"
STOP_ON_ATTEMPTS="${STOP_ON_ATTEMPTS:-0}"
mkdir -p "$OUT_DIR"

CONFIG_BACKUP="$(mktemp)"
cp config.h "$CONFIG_BACKUP"
restore_config() {
  cp "$CONFIG_BACKUP" config.h
  rm -f "$CONFIG_BACKUP"
}
trap restore_config EXIT

patch_config() {
  local cc_alg="$1"
  local tpcc_type="$2"
  local branches="$3"
  python - "$cc_alg" "$tpcc_type" "$THREADS" "$MAX_TXN" "$WAREHOUSES" "$branches" "$STOP_ON_ATTEMPTS" <<'PY'
import re
import sys

cc_alg, tpcc_type, threads, max_txn, warehouses, branches, stop_on_attempts = sys.argv[1:8]
path = "config.h"
text = open(path).read()
stop_literal = "true" if stop_on_attempts not in ("0", "false", "False", "") else "false"
repls = {
    r"#define\s+WORKLOAD\s+.*": "#define WORKLOAD \t\t\t\t\tTPCC",
    r"#define\s+CC_ALG\s+.*": "#define CC_ALG \t\t\t\t\t\t%s" % cc_alg,
    r"#define\s+THREAD_CNT\s+.*": "#define THREAD_CNT\t\t\t\t\t%s" % threads,
    r"#define\s+MAX_TXN_PER_PART\s+.*": "#define MAX_TXN_PER_PART \t\t\t%s" % max_txn,
    r"#define\s+STOP_ON_ATTEMPTS\s+.*": "#define STOP_ON_ATTEMPTS\t\t\t%s" % stop_literal,
    r"#define\s+NUM_WH\s+.*": "#define NUM_WH \t\t\t\t\t\t%s" % warehouses,
    r"#define\s+TPCC_TXN_TYPE\s+.*": "#define TPCC_TXN_TYPE\t\t\t\t%s" % tpcc_type,
    r"#define\s+TPCC_AGENT_BRANCHES\s+.*": "#define TPCC_AGENT_BRANCHES\t\t\t%s" % branches,
}
for pattern, replacement in repls.items():
    text, count = re.subn(pattern, replacement, text)
    if count != 1:
        raise SystemExit("failed to patch %s; replacements=%d" % (pattern, count))
open(path, "w").write(text)
PY
}

run_case() {
  local mode="$1"
  local cc_alg="$2"
  local tpcc_type="$3"
  local branches="${4:-1}"
  local outfile="$OUT_DIR/${mode}.txt"
  local buildlog="$OUT_DIR/${mode}.build.log"

  patch_config "$cc_alg" "$tpcc_type" "$branches"
  make -j >"$buildlog" 2>&1
  ./rundb -Tb"$branches" -o "$outfile"

  python - "$mode" "$cc_alg" "$tpcc_type" "$THREADS" "$MAX_TXN" "$WAREHOUSES" "$branches" "$outfile" "$OUT_DIR/summary.csv" <<'PY'
import os
import re
import sys

mode, cc_alg, tpcc_type, threads, max_txn, warehouses, branches, outfile, csv_path = sys.argv[1:10]
summary = ""
for line in open(outfile):
    if line.startswith("[summary]"):
        summary = line.strip()
if not summary:
    raise SystemExit("no summary line found in %s" % outfile)

fields = dict(re.findall(r"([A-Za-z0-9_]+)=([0-9.]+)", summary))
txn_cnt = float(fields.get("txn_cnt", 0))
abort_cnt = float(fields.get("abort_cnt", 0))
resource_abort_cnt = float(fields.get("resource_abort_cnt", 0))
agent_txn_cnt = float(fields.get("agent_txn_cnt", 0))
branch_attempt_cnt = float(fields.get("branch_attempt_cnt", 0))
winner_commit_cnt = float(fields.get("winner_commit_cnt", 0))
planned_loser_abort_cnt = float(fields.get("planned_loser_abort_cnt", 0))
read_validate_abort_cnt = float(fields.get("read_validate_abort_cnt", 0))
cas_abort_cnt = float(fields.get("cas_abort_cnt", 0))
xwrite_abort_cnt = float(fields.get("xwrite_abort_cnt", 0))
insert_abort_cnt = float(fields.get("insert_abort_cnt", 0))
delete_abort_cnt = float(fields.get("delete_abort_cnt", 0))
predicate_abort_cnt = float(fields.get("predicate_abort_cnt", 0))
aet_policy_read_cnt = float(fields.get("aet_policy_read_cnt", 0))
aet_policy_delta_cnt = float(fields.get("aet_policy_delta_cnt", 0))
aet_policy_cas_cnt = float(fields.get("aet_policy_cas_cnt", 0))
aet_policy_xwrite_cnt = float(fields.get("aet_policy_xwrite_cnt", 0))
aet_policy_insert_cnt = float(fields.get("aet_policy_insert_cnt", 0))
aet_policy_delete_cnt = float(fields.get("aet_policy_delete_cnt", 0))
aet_policy_pred_read_cnt = float(fields.get("aet_policy_pred_read_cnt", 0))
run_time = float(fields.get("run_time", 0))
latency = float(fields.get("latency", 0))
total_attempts = txn_cnt + abort_cnt
throughput = txn_cnt / run_time if run_time > 0 else 0
attempt_throughput = total_attempts / run_time if run_time > 0 else 0
success_rate = txn_cnt / total_attempts if total_attempts > 0 else 0

header = "mode,cc_alg,tpcc_type,threads,max_txn,warehouses,branches,txn_cnt,abort_cnt,resource_abort_cnt,agent_txn_cnt,branch_attempt_cnt,winner_commit_cnt,planned_loser_abort_cnt,read_validate_abort_cnt,cas_abort_cnt,xwrite_abort_cnt,insert_abort_cnt,delete_abort_cnt,predicate_abort_cnt,aet_policy_read_cnt,aet_policy_delta_cnt,aet_policy_cas_cnt,aet_policy_xwrite_cnt,aet_policy_insert_cnt,aet_policy_delete_cnt,aet_policy_pred_read_cnt,success_rate,throughput,attempt_throughput,avg_latency,output_file\n"
row = "%s,%s,%s,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.9f,%s\n" % (
    mode, cc_alg, tpcc_type, threads, max_txn, warehouses, branches,
    txn_cnt, abort_cnt, resource_abort_cnt,
    agent_txn_cnt, branch_attempt_cnt, winner_commit_cnt, planned_loser_abort_cnt,
    read_validate_abort_cnt, cas_abort_cnt, xwrite_abort_cnt,
    insert_abort_cnt, delete_abort_cnt, predicate_abort_cnt,
    aet_policy_read_cnt, aet_policy_delta_cnt, aet_policy_cas_cnt, aet_policy_xwrite_cnt,
    aet_policy_insert_cnt, aet_policy_delete_cnt, aet_policy_pred_read_cnt,
    success_rate, throughput, attempt_throughput, latency, outfile
)
write_header = not os.path.exists(csv_path)
with open(csv_path, "a") as f:
    if write_header:
        f.write(header)
    f.write(row)
PY
}

case_enabled() {
  local mode="$1"
  if [[ "$CASES" == "all" ]]; then
    return 0
  fi
  [[ ",$CASES," == *",$mode,"* ]]
}

lower_name() {
  printf "%s" "$1" | tr '[:upper:]' '[:lower:]'
}

run_agent_baseline_all_cc() {
  local alg
  local branch
  IFS=',' read -ra algs <<< "$CC_ALGS"
  IFS=',' read -ra branches <<< "$BRANCHES"
  for alg in "${algs[@]}"; do
    alg="${alg//[[:space:]]/}"
    [[ -z "$alg" ]] && continue
    local alg_lc
    alg_lc="$(lower_name "$alg")"
    for branch in "${branches[@]}"; do
      branch="${branch//[[:space:]]/}"
      [[ -z "$branch" ]] && continue
      run_case "tpcc_agent_new_order_baseline_${alg_lc}_b${branch}" "$alg" "TPCC_AGENT_NEW_ORDER_BASELINE" "$branch"
    done
  done
}

rm -f "$OUT_DIR/summary.csv"
case_enabled "tpcc_agent_new_order_baseline_all_cc" && run_agent_baseline_all_cc
case_enabled "tpcc_occ_new_order" && run_case "tpcc_occ_new_order" "OCC" "TPCC_NEW_ORDER"
case_enabled "tpcc_occ_reserve_standard_new_order" && run_case "tpcc_occ_reserve_standard_new_order" "OCC_RESERVE" "TPCC_NEW_ORDER_RESERVE_STANDARD"
case_enabled "tpcc_occ_reserve_new_order" && run_case "tpcc_occ_reserve_new_order" "OCC_RESERVE" "TPCC_NEW_ORDER_RESERVE"
case_enabled "tpcc_agent_new_order_baseline_occ_b1" && run_case "tpcc_agent_new_order_baseline_occ_b1" "OCC" "TPCC_AGENT_NEW_ORDER_BASELINE" "1"
case_enabled "tpcc_agent_new_order_baseline_occ_b4" && run_case "tpcc_agent_new_order_baseline_occ_b4" "OCC" "TPCC_AGENT_NEW_ORDER_BASELINE" "4"
case_enabled "tpcc_agent_new_order_baseline_occ_b8" && run_case "tpcc_agent_new_order_baseline_occ_b8" "OCC" "TPCC_AGENT_NEW_ORDER_BASELINE" "8"
case_enabled "tpcc_agent_new_order_baseline_reserve_b1" && run_case "tpcc_agent_new_order_baseline_reserve_b1" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_BASELINE" "1"
case_enabled "tpcc_agent_new_order_baseline_reserve_b4" && run_case "tpcc_agent_new_order_baseline_reserve_b4" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_BASELINE" "4"
case_enabled "tpcc_agent_new_order_baseline_reserve_b8" && run_case "tpcc_agent_new_order_baseline_reserve_b8" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_BASELINE" "8"
case_enabled "tpcc_agent_new_order_reserve_b1" && run_case "tpcc_agent_new_order_reserve_b1" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE" "1"
case_enabled "tpcc_agent_new_order_reserve_b4" && run_case "tpcc_agent_new_order_reserve_b4" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE" "4"
case_enabled "tpcc_agent_new_order_reserve_b8" && run_case "tpcc_agent_new_order_reserve_b8" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE" "8"
case_enabled "tpcc_agent_new_order_reserve_standard_b1" && run_case "tpcc_agent_new_order_reserve_standard_b1" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "1"
case_enabled "tpcc_agent_new_order_reserve_standard_b4" && run_case "tpcc_agent_new_order_reserve_standard_b4" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "4"
case_enabled "tpcc_agent_new_order_reserve_standard_b8" && run_case "tpcc_agent_new_order_reserve_standard_b8" "OCC_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "8"
case_enabled "tpcc_agent_new_order_aet_reserve_standard_b1" && run_case "tpcc_agent_new_order_aet_reserve_standard_b1" "AET_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "1"
case_enabled "tpcc_agent_new_order_aet_reserve_standard_b4" && run_case "tpcc_agent_new_order_aet_reserve_standard_b4" "AET_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "4"
case_enabled "tpcc_agent_new_order_aet_reserve_standard_b8" && run_case "tpcc_agent_new_order_aet_reserve_standard_b8" "AET_RESERVE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "8"
case_enabled "tpcc_agent_new_order_aet_occ_standard_b1" && run_case "tpcc_agent_new_order_aet_occ_standard_b1" "AET_OCC" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "1"
case_enabled "tpcc_agent_new_order_aet_occ_standard_b4" && run_case "tpcc_agent_new_order_aet_occ_standard_b4" "AET_OCC" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "4"
case_enabled "tpcc_agent_new_order_aet_occ_standard_b8" && run_case "tpcc_agent_new_order_aet_occ_standard_b8" "AET_OCC" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "8"
case_enabled "tpcc_agent_new_order_aet_hybrid_standard_b1" && run_case "tpcc_agent_new_order_aet_hybrid_standard_b1" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "1"
case_enabled "tpcc_agent_new_order_aet_hybrid_standard_b4" && run_case "tpcc_agent_new_order_aet_hybrid_standard_b4" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "4"
case_enabled "tpcc_agent_new_order_aet_hybrid_standard_b8" && run_case "tpcc_agent_new_order_aet_hybrid_standard_b8" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "8"
case_enabled "tpcc_agent_new_order_aet_hybrid_silo_standard_b1" && run_case "tpcc_agent_new_order_aet_hybrid_silo_standard_b1" "AET_HYBRID_SILO" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "1"
case_enabled "tpcc_agent_new_order_aet_hybrid_silo_standard_b4" && run_case "tpcc_agent_new_order_aet_hybrid_silo_standard_b4" "AET_HYBRID_SILO" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "4"
case_enabled "tpcc_agent_new_order_aet_hybrid_silo_standard_b8" && run_case "tpcc_agent_new_order_aet_hybrid_silo_standard_b8" "AET_HYBRID_SILO" "TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD" "8"
case_enabled "tpcc_agent_new_order_aet_hybrid_reserve_b1" && run_case "tpcc_agent_new_order_aet_hybrid_reserve_b1" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE" "1"
case_enabled "tpcc_agent_new_order_aet_hybrid_reserve_b4" && run_case "tpcc_agent_new_order_aet_hybrid_reserve_b4" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE" "4"
case_enabled "tpcc_agent_new_order_aet_hybrid_reserve_b8" && run_case "tpcc_agent_new_order_aet_hybrid_reserve_b8" "AET_HYBRID_RULE" "TPCC_AGENT_NEW_ORDER_RESERVE" "8"

echo "Wrote $OUT_DIR/summary.csv"
