# Data Agent Real Backend Status

## What Is Real Now

The `data_agent_synthetic_runner` now supports:

- `--backend synthetic`
- `--backend dbx1000_test`

The `dbx1000_test` backend is no longer an in-memory adapter. It executes against real DBx1000 components:

- `row_t`
- `Row_occ`
- `txn_man`
- `TestWorkload`
- `occ_man.validate()`
- real row-version bumps and validation failures

`agent_level_txn` on this backend uses the semantic agent path:

- `begin_agent_branches`
- `begin_agent_branch`
- `record_agent_read_intent`
- `record_agent_xwrite_intent`
- `select_agent_winner`
- `finish`

`baseline_multi_txn` on this backend uses traditional transactions:

- one real DBx1000 txn per candidate
- winner commits with `finish(RCOK)`
- loser executes and then aborts with `finish(Abort)`

## What This Enables

We can now measure on a real DBx1000 backend:

- semantic agent-level path vs traditional per-candidate txns
- real OCC validation under version changes
- commit/abort counts under controlled conflict injection

This is a meaningful integration milestone because the comparison is no longer based on a fake object map.

## What Is Still Missing For Paper-Grade Evaluation

This path is still not sufficient as a primary paper result.

Remaining gaps:

- It uses `benchmarks/test` rather than TPCC or a task-shaped production workload.
- It is currently single-threaded, so it does not capture real concurrent contention patterns.
- Candidate evaluation work is still synthetic; only the backend commit path is real.
- The current comparison is on `OCC_RESERVE`-based DBx1000 semantics, not a full matrix of traditional CC algorithms such as `SILO` / `TicToc`.
- The output object is bound to a test-schema field, so the benchmark proves backend correctness and cost shape, not end-application semantics.

## Recommended Next Milestone

To make this publishable, the next implementation target should be:

1. move the real backend path from `benchmarks/test` to a TPCC-style adapter
2. add multi-thread execution with per-worker DBx1000 txn contexts
3. rerun semantic vs traditional comparison under real contention
4. add at least one traditional CC baseline build such as `SILO` or `TicToc`

Until those are done, the current real-backend results should be treated as backend-integration evidence, not final headline numbers.
