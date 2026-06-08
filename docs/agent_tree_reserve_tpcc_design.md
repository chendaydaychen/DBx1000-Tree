# Agent 多分支事务与资源预占用机制设计说明

本文说明当前 DBx1000 改造版中的两个核心机制：Agent 多候选决策事务模型，以及基于 OCC 扩展的资源预占用并发控制。文档也说明 TPC-C New-Order 实验如何设计，哪些实验用于公平性能对比，哪些实验用于展示资源约束语义。

## 1. 问题背景

传统数据库事务模型默认一次事务只有一条确定执行路径。对于 Agent 决策任务，这个假设不够自然。Agent 往往会生成多个候选方案，然后只选择一个方案真正生效。例如：

- 一个订单可以由多个仓库供货。
- 一个配送任务可以由多个门店完成。
- 一个座位请求可以匹配多个候选座位。

如果用传统事务模型表达这种任务，常见做法是把每个候选都当作一次独立事务尝试。对于 8 个候选，传统模型通常等价于执行 8 次候选事务：前 7 个候选执行后 abort，最后 1 个候选 commit。这些 abort 是模型表达方式带来的计划性 abort，不一定代表并发冲突。

我们的目标是把这种“多候选、只提交一个 winner”的语义放进事务层，让数据库能原生表达 Agent 决策任务，而不是把候选分支强行拆成多次传统事务。

## 2. 多分支事务模型

多分支事务把一个逻辑 Agent 请求表示为一个事务上下文，事务内部包含多个候选分支。每个分支都有自己的私有状态：

- 分支私有读写记录。
- 分支私有资源 delta。
- 分支私有临时行副本。

分支阶段只评估候选，不直接修改数据库已提交状态。最终选择一个 winner 分支后，系统只把 winner 的写集合并到事务的真实写集里，并通过 OCC 验证后提交。非 winner 分支被丢弃，不进入最终提交验证，也不会成为数据库层面的外部 abort。

当前在 `txn_man` 上加入的核心 API 包括：

- `begin_agent_branches(branch_cnt)`：开始一个多分支事务。
- `begin_agent_branch(branch_id)`：切换到某个候选分支。
- `reserve_agent_branch_delta(...)`：在分支中声明资源 delta，并做全局预占用检查。
- `reserve_agent_branch_delta_local(...)`：只记录分支本地 delta，不访问全局预占用表。
- `select_agent_winner(branch_id, materialize_global_reservations)`：选择 winner，并把 winner 分支物化到真实事务写集。
- `abort_agent_branch(...)` / `abort_agent_branches()`：丢弃 loser 分支并释放需要释放的预占用。

一个 4 分支例子：

```text
Agent 请求 R:
  branch 0: 尝试仓库 W1
  branch 1: 尝试仓库 W2
  branch 2: 尝试仓库 W3
  branch 3: 尝试仓库 W4

传统事务模型:
  Txn(W1) -> abort
  Txn(W2) -> abort
  Txn(W3) -> abort
  Txn(W4) -> commit

Tree/多分支事务模型:
  One Txn:
    branch W1: 私有评估
    branch W2: 私有评估
    branch W3: 私有评估
    branch W4: 私有评估
    winner = W4
    只提交 W4 的写集
```

因此，传统模型在 4 分支下会产生 3 次计划性 abort；多分支模型只产生 1 次逻辑事务提交。

## 3. 资源预占用并发控制

预占用机制基于 OCC 扩展实现，目前作为 `OCC_RESERVE` 与 DBx1000 原有 CC 算法并列，通过 `CC_ALG` 宏选择。它不是替代所有并发控制，也不是普通 2PL 锁，而是给库存、座位、配额这类资源型字段增加一个 pending delta ledger。

以库存为例，一个分支要扣减库存 `q`，会声明 delta：

```text
delta = -q
```

预占用表按 row 记录当前未提交事务持有的 pending delta。检查逻辑是：

```text
committed_value + pending_delta + new_delta >= 0
```

如果检查通过，事务获得该资源的预占用；如果失败，说明当前资源容量已经被已提交值和未提交预占用耗尽，本次候选失败，并计入 `resource_abort_cnt`。

提交和 abort 语义：

- commit 成功：winner 的预占用 delta 被确认，并转换为真实行更新。
- OCC 验证失败：释放本事务持有的预占用。
- 事务 abort：释放本事务持有的预占用，不修改真实行。

ACID 角度：

- Atomicity：只有 winner 分支的写入会被物化；事务失败时释放所有分支状态和预占用。
- Consistency：有界预占用模式可以保证资源字段不被扣成负数；标准 TPCC 模式保持原始 TPC-C 的库存更新公式。
- Isolation：最终提交仍走 OCC 验证；预占用表让资源容量声明在并发事务之间互相可见。
- Durability：DBx1000 本身是内存 benchmark，本改造继承它的持久化假设；提交后的行状态在 benchmark 内可见，但不提供磁盘 WAL 级持久化。

## 4. TPCC New-Order 改造

原生 TPC-C New-Order 主要会读写以下表：

- `WAREHOUSE`
- `DISTRICT`
- `CUSTOMER`
- `ITEM`
- `STOCK`
- `ORDER`
- `NEW_ORDER`
- `ORDER_LINE`

这里不是简单的 key/value copy。DBx1000 的 TPC-C row 是多列 row，库存字段只是 `STOCK` 行里的 `S_QUANTITY` 一列。因此预占用机制不是复制整行 key/value，而是在 row + column 粒度上对 `S_QUANTITY` 记录 delta，并在 winner 提交时更新本地 row 副本。

Agent-NewOrder 的候选分支被定义为不同供货仓库选择。假设当前订单来自仓库 `W1`，分支数为 4，则候选可以是：

```text
branch 0 -> supply warehouse W1
branch 1 -> supply warehouse W2
branch 2 -> supply warehouse W3
branch 3 -> supply warehouse W4
```

每个候选分支都执行 New-Order 中与该供货仓库相关的 stock 逻辑。最终 winner 提交一套 `ORDER / NEW_ORDER / ORDER_LINE / STOCK` 更新。

## 5. 传统方法实验

传统方法用于模拟“Agent 多候选负载在传统事务模型下如何执行”。它使用 DBx1000 原有并发控制算法，例如：

- `OCC`
- `NO_WAIT`
- `WAIT_DIE`
- `DL_DETECT`
- `MVCC`
- `HEKATON`
- `TICTOC`
- `SILO`

事务类型使用：

```text
TPCC_AGENT_NEW_ORDER_BASELINE
```

对于 `TPCC_AGENT_BRANCHES = N`，传统方法执行 N 个候选尝试，其中前 `N-1` 个候选按传统事务 abort，最后一个候选 commit。这样可以显式体现传统事务模型表达多候选 Agent 任务时的计划性 abort 开销。

例子：`N = 8`

```text
candidate 1 -> execute -> abort
candidate 2 -> execute -> abort
candidate 3 -> execute -> abort
candidate 4 -> execute -> abort
candidate 5 -> execute -> abort
candidate 6 -> execute -> abort
candidate 7 -> execute -> abort
candidate 8 -> execute -> commit
```

所以传统方法的成功率理论上接近：

```text
success_rate = 1 / N
```

这不是系统 bug，而是该实验口径下传统模型需要为 loser candidates 付出的事务执行成本。

推荐命令：

```bash
CASES=tpcc_agent_new_order_baseline_all_cc \
CC_ALGS=OCC,NO_WAIT,WAIT_DIE,DL_DETECT,MVCC,HEKATON,TICTOC,SILO \
BRANCHES=1,4,8 \
MAX_TXN=10000 STOP_ON_ATTEMPTS=1 \
WAREHOUSES=8 THREADS=4 \
OUT_DIR=output/exp_agent_baseline_all_cc_attempts \
scripts/run_tpcc_compare.sh
```

## 6. 我们的方法实验

我们的方法使用：

```text
CC_ALG = OCC_RESERVE
TPCC_TXN_TYPE = TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD
```

这个实验是主性能对比口径。它保留原始 TPC-C 的库存更新公式，不额外加入 `s_quantity - ol_quantity >= 0` 的业务约束，因此与传统 TPC-C New-Order 更公平。分支只记录本地 delta，winner 选出后才物化真实 row 副本并进入 OCC 提交路径。

例子：`N = 8`

```text
One logical Tree transaction:
  branch 1 -> private evaluation
  branch 2 -> private evaluation
  ...
  branch 8 -> private evaluation
  winner = branch 8
  materialize branch 8 writes
  OCC validate
  commit once
```

该模式下 loser 分支不会变成数据库外部 abort，因此 `abort_cnt` 通常为 0，`success_rate` 通常为 1。随着分支数增大，它避免的计划性 abort 越多，所以 b4、b8 更能体现优势；b1 没有多分支收益，主要体现模型自身开销。

推荐命令：

```bash
CASES=tpcc_agent_new_order_reserve_standard_b1,tpcc_agent_new_order_reserve_standard_b4,tpcc_agent_new_order_reserve_standard_b8 \
MAX_TXN=10000 STOP_ON_ATTEMPTS=1 \
WAREHOUSES=8 THREADS=4 \
OUT_DIR=output/exp_agent_tree_reserve_standard_fast2_attempts \
scripts/run_tpcc_compare.sh
```

## 7. 资源约束实验

如果要展示预占用机制的资源容量语义，使用：

```text
TPCC_AGENT_NEW_ORDER_RESERVE
```

该模式会额外执行：

```text
s_quantity - ol_quantity >= 0
```

这不是原始 TPC-C New-Order 的公平性能对比口径，而是资源语义实验。它用于证明预占用机制能在并发下阻止库存被扣成负数。由于库存可能耗尽，这类实验会产生 `resource_abort_cnt`，并且应使用 `STOP_ON_ATTEMPTS=1`，避免 commit-only 终止条件在库存耗尽后长时间等待。

推荐命令：

```bash
CASES=tpcc_agent_new_order_reserve_b1,tpcc_agent_new_order_reserve_b4,tpcc_agent_new_order_reserve_b8 \
MAX_TXN=10000 STOP_ON_ATTEMPTS=1 \
WAREHOUSES=8 THREADS=4 \
OUT_DIR=output/exp_agent_tree_reserve_bounded_attempts \
scripts/run_tpcc_compare.sh
```

## 8. 指标解释

`scripts/run_tpcc_compare.sh` 生成的 `summary.csv` 包含：

- `txn_cnt`：成功提交的逻辑事务数。
- `abort_cnt`：事务 abort 数。传统多候选 baseline 中包含计划性 loser abort。
- `resource_abort_cnt`：资源预占用失败数。
- `success_rate`：`txn_cnt / (txn_cnt + abort_cnt)`。
- `throughput`：提交吞吐，`txn_cnt / run_time`。
- `attempt_throughput`：尝试吞吐，`(txn_cnt + abort_cnt) / run_time`。
- `avg_latency`：平均延迟。

主性能结论建议看 `throughput`，因为一个 Agent 请求最终只需要一个 winner commit。传统模型和 Tree 模型都以 logical commit 作为有效完成量。`attempt_throughput` 可以作为辅助指标，用来说明传统模型虽然做了更多 attempt，但很多 attempt 是计划性 abort，并不产生有效业务提交。

## 9. 当前推荐对比口径

主对比：

```text
传统事务 + 传统 CC:
  TPCC_AGENT_NEW_ORDER_BASELINE
  CC_ALG in OCC/NO_WAIT/WAIT_DIE/DL_DETECT/MVCC/HEKATON/TICTOC/SILO

Tree 事务 + 预占用 CC 标准模式:
  TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD
  CC_ALG = OCC_RESERVE
```

资源语义消融：

```text
Tree 事务 + 有界预占用:
  TPCC_AGENT_NEW_ORDER_RESERVE
  CC_ALG = OCC_RESERVE
```

不要把有界预占用模式直接和原始 TPC-C baseline 当作公平性能对比，因为前者额外加入了库存非负约束，而原始 TPC-C New-Order 不要求 `S_QUANTITY` 永远不低于 0。
