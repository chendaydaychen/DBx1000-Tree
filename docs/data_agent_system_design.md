# Data Agent System Design

## Goal

This document defines the first integration boundary for turning `DBx1000` into a minimal Data Agent System prototype without replacing the existing DBMS execution path.

The target stack is:

```text
Agent / workload driver
  -> submit_task(TaskSpec)
Data Agent runtime
  -> candidate generation
  -> branch evaluation
  -> winner selection
  -> transactional apply
DBx1000 backend
  -> storage
  -> concurrency control
```

## Design Principles

1. Keep the native `DBx1000` benchmark and transaction paths runnable.
2. Add `data_agent/` as an upper layer instead of rewriting `system/` or `concurrency_control/`.
3. Treat current `system/agent_txn.*` as a backend execution primitive, not as the public Data Agent API.
4. Hide branch lifecycle control behind `submit_task`, so future workloads do not call `begin_branch` or `select_winner` directly.

## Phase 0 Boundary

The current repository already has backend support for:

- branch-local read / delta / write intent recording
- winner validation and materialization
- loser branch release
- planned loser accounting versus real abort accounting

What is still missing is the task-facing control plane:

- `TaskSpec` and `TaskResult`
- task runtime orchestration
- agent-level transaction context independent from `txn_man`
- workload adapters outside `benchmarks/`
- object-store style adapter API

## Initial Module Split

### `data_agent/runtime/`

Owns task orchestration. It receives `TaskSpec`, constructs a `TaskContext`, invokes operators in order, and returns `TaskResult`.

### `data_agent/transaction/`

Owns agent-level transaction state and the mapping between `task_id`, `txn_id`, branch ids, and winner selection state.

### `data_agent/branch/`

Owns branch metadata, branch state transitions, and candidate result bookkeeping.

### `data_agent/intent/`

Owns Data Agent intent types and the translation boundary between high-level intent semantics and backend operations.

### `data_agent/object_store/`

Defines the backend-facing object access surface. The first implementation can remain minimal and only expose point get / put / apply-intent primitives.

### `data_agent/client/`

Defines the public entry point for workload drivers:

```cpp
TaskResult submit_task(const TaskSpec &task);
```

## Integration Strategy

Phase 1 does not move any existing execution logic out of `system/` or `concurrency_control/`.

Instead it introduces a parallel semantic layer:

- `data_agent/*` owns task-level concepts
- `system/agent_txn.*` remains the backend branch-execution primitive
- `txn_man` remains the place where backend commit / abort still happens

This keeps the refactor incremental and keeps `rundb` stable while the new runtime surface is built out.

## Non-Goals For Phase 1

- no TPCC transaction rewrites
- no Vita-style parser yet
- no object scan / filter / join support
- no LLM integration
- no migration of AET policy code out of `concurrency_control/`

## Expected Next Step

After this skeleton lands, the next implementation step should be a synthetic end-to-end path:

1. create a `TaskSpec`
2. generate K synthetic candidates
3. create K branch records
4. mark one winner
5. return a structured `TaskResult`

Only after that should the runtime be connected to real DBx1000 row access and AET-backed commit.
