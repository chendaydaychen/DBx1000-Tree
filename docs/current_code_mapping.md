# Current Code Mapping

## Purpose

This document maps the existing `DBx1000` code to the planned Data Agent System architecture so the refactor can proceed without losing track of what already exists.

## Existing Backend Capabilities

### Branch and intent data model

`system/agent_txn.h`

- defines `AgentBranchStatus`
- defines backend intent structs: `AgentReadIntent`, `AgentDeltaIntent`, `AgentWriteIntent`
- defines `AgentBranch`
- defines `AgentTxnManager`

This is the current backend representation of branch-local staged state.

### Transaction-facing backend API

`system/txn.h`
`system/txn.cpp`

`txn_man` currently exposes the backend branch API:

- `begin_agent_branches`
- `begin_agent_branch`
- `record_agent_read_intent`
- `reserve_agent_branch_delta*`
- `record_agent_tpcc_stock_update*`
- `record_agent_cas_intent*`
- `record_agent_xwrite_intent*`
- `select_agent_winner`
- `abort_agent_branch`

This is the current execution bridge between transaction logic and branch-aware backend operations.

### Winner materialization and loser release

`concurrency_control/aet_reserve.cpp`
`concurrency_control/aet_hybrid.cpp`

These files currently own:

- read / write / delta intent validation
- winner selection preparation
- loser branch release
- planned loser statistics
- handoff to `txn_man::materialize_agent_*`

This is the current backend commit policy layer.

### Native benchmark and TPCC entrypoints

`benchmarks/tpcc_query.cpp`
`benchmarks/tpcc_txn.cpp`
`scripts/run_tpcc_compare.sh`

Current agent-style TPCC evaluation still enters through benchmark transaction code. It is not yet routed through a task runtime.

### Correctness smoke tests

`benchmarks/test_txn.cpp`

Current test cases already validate:

- CAS winner application
- XWRITE winner application
- read-version abort
- CAS-version abort
- XWRITE-version abort

These tests cover the backend AET branch primitive and should remain valid during the upper-layer refactor.

## Planned Ownership After Refactor

### Stays in `system/`

- row-level backend data access
- transaction lifetime in `txn_man`
- backend materialization helpers
- low-level reservation bookkeeping

### Stays in `concurrency_control/`

- AET reserve / hybrid commit policy
- backend validation rules
- row-version and lock management

### Moves conceptually to `data_agent/`

- task-oriented submission API
- agent-level transaction semantics
- branch metadata independent of `row_t *`
- workload-facing intent model
- task runtime orchestration

## Immediate Refactor Rule

Before any backend logic is moved, the new `data_agent/` layer must call into the existing backend instead of replacing it.

That means Phase 1 should:

1. create new task-facing types
2. keep backend APIs intact
3. avoid editing TPCC transaction behavior
4. preserve `make -j` and existing `rundb` behavior
