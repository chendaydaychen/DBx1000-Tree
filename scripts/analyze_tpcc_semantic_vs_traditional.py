#!/usr/bin/env python3

import csv
import sys
from pathlib import Path


def load_rows(base_dir: Path):
    rows = []
    for summary in sorted(base_dir.glob("*/summary.csv")):
        conflict_level = summary.parent.name
        with summary.open() as f:
            for row in csv.DictReader(f):
                row["conflict_level"] = conflict_level
                rows.append(row)
    return rows


def traditional_key(row):
    return (
        row["conflict_level"],
        row["branches"],
        row["cc_alg"],
    )


def semantic_key(row):
    return (
        row["conflict_level"],
        row["branches"],
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: analyze_tpcc_semantic_vs_traditional.py OUTPUT_DIR")

    base_dir = Path(sys.argv[1])
    rows = load_rows(base_dir)
    if not rows:
        raise SystemExit("no summary.csv files found under %s" % base_dir)

    semantic_rows = {}
    traditional_rows = {}
    for row in rows:
        if row["cc_alg"] == "AET_HYBRID_CC":
            semantic_rows[semantic_key(row)] = row
        else:
            traditional_rows[traditional_key(row)] = row

    print(
        "conflict_level,branches,semantic_throughput,semantic_success_rate,"
        "traditional_cc,traditional_throughput,traditional_success_rate,"
        "semantic_over_traditional,semantic_branch_attempt_cnt,"
        "semantic_planned_loser_abort_cnt,traditional_abort_cnt"
    )
    for key in sorted(traditional_rows):
        conflict_level, branches, cc_alg = key
        semantic = semantic_rows.get((conflict_level, branches))
        if semantic is None:
            continue
        traditional = traditional_rows[key]
        semantic_tp = float(semantic["throughput"])
        traditional_tp = float(traditional["throughput"])
        ratio = semantic_tp / traditional_tp if traditional_tp > 0 else 0.0
        print(
            "{},{},{:.6f},{:.6f},{},{:.6f},{:.6f},{:.6f},{},{},{}".format(
                conflict_level,
                branches,
                semantic_tp,
                float(semantic["success_rate"]),
                cc_alg,
                traditional_tp,
                float(traditional["success_rate"]),
                ratio,
                semantic["branch_attempt_cnt"],
                semantic["planned_loser_abort_cnt"],
                traditional["abort_cnt"],
            )
        )


if __name__ == "__main__":
    main()
