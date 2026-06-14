# Data Agent System Project Structure

## New Top-Level Additions

```text
data_agent/
  common/         shared task-facing types
  client/         submit_task API
  runtime/        task orchestration
  transaction/    agent-level transaction context
  branch/         branch metadata and state
  intent/         task-facing intent model
  object_store/   backend adapter interface

workloads/
  synthetic/      first end-to-end validation target
  tpcc_style/     future TPCC task adapter
  vita_style/     future Vita-style task adapter

results/
  raw/
  parsed/
  figures/
  logs/

tests/
```

## Phase 1 Scope

Phase 1 only requires:

- directory skeletons
- minimal types
- public headers
- no-op or placeholder runtime implementation

## Backend Relationship

The new structure is additive.

- `benchmarks/`, `system/`, `storage/`, and `concurrency_control/` remain the backend core.
- `data_agent/` becomes the task-facing semantic layer.
- `workloads/` becomes the adapter layer for future non-native workloads.
