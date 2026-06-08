# DBx1000-Tree 下一阶段改造计划：面向 Agent 探索事务的多分支并发控制框架

## 0. 文档目的

本文档用于指导后续 agent 在现有 `DBx1000-Tree` 仓库基础上继续改造。当前仓库已经实现了基于 OCC 的多分支事务与资源预占用原型，能够验证 winner only commit 语义，并在 TPC-C NewOrder 场景下展示传统多候选事务模型的 loser abort 放大问题。

下一阶段目标是：将当前 `OCC_RESERVE` 原型升级为一个更通用的 **面向 Agent 探索事务的多分支事务模型与分支感知并发控制框架**。该框架应减少对 OCC 的绑定，支持不同冲突率、不同操作语义、不同事务长度下的并发控制策略选择。

---

## 1. 当前版本定位

当前版本可以视为 proof of concept，主要完成了三件事：

1. 在原生 DBx1000 基础上新增了 `OCC_RESERVE`。
2. 在事务内部加入了多个候选 branch，并通过 `select_agent_winner()` 只物化 winner 分支。
3. 对库存、座位、额度等增量型资源字段实现了 delta reservation。

当前机制已经能够说明：

```text
传统模型：
  一个 Agent 请求拆成多个候选事务
  loser candidate 以 abort 形式体现
  branch 数越多，abort 数越高，success rate 越低

Tree / AET 模型：
  一个 Agent 请求是一个逻辑事务
  内部包含多个候选 branch
  loser branch 在事务内部释放
  只有 winner branch 进入真实提交路径
```

---

## 2. 当前机制的两个局限

### 2.1 与 OCC 绑定过深

当前 `OCC_RESERVE` 是在 OCC 基础上改造的。该实现方式存在三个问题：

1. 性能受 OCC 基础性能限制。
2. 高冲突场景下仍可能出现大量 validate abort。
3. 后续难以接入 SILO、TicToc、2PL、MVCC 等其他策略。

当前实现更像是：

```text
OCC + Branch API + Delta Reservation
```

下一阶段应改造成：

```text
Agent Exploration Transaction Model
+
Branch-aware Concurrency Control Framework
+
Generalized Intent Layer
```

---

### 2.2 预占用只适合增量写

当前 `reserve_row_delta()` 只适用于如下操作：

```text
stock -= quantity
seat_count -= 1
budget += delta
```

它不适合处理：

```text
覆盖式写：
  order.status = "cancelled"
  user.address = new_address

条件写：
  if status == "pending":
      status = "confirmed"

追加写：
  append message
  append tool log

读操作：
  read profile
  read policy
  read product list
  read memory
```

因此，当前 reservation 机制应从单一 delta reserve 升级为更通用的 intent 机制。

---

## 3. 下一阶段总体目标

建议将系统升级为：

```text
AET-CC: Concurrency Control for Agent Exploration Transactions
```

其中 AET 表示 **Agent Exploration Transaction**。

系统目标：

1. 将多分支事务语义从 OCC 中解耦。
2. 将 delta reservation 扩展为通用 intent layer。
3. 在提交阶段只验证和物化 winner branch。
4. 对不同操作类型使用不同并发控制策略。
5. 为复杂冲突率场景提供自适应策略选择能力。

目标架构：

```text
Agent Request
     |
     v
Agent Exploration Transaction
Root + Branches + Winner Rule
     |
     v
Branch Intent Collection Layer
ReadIntent / DeltaIntent / CASIntent / XWriteIntent / AppendIntent / PredicateReadIntent
     |
     v
Branch-aware CC Policy Selector
OCC | MVCC | 2PL | Semantic Reservation | Hybrid
     |
     v
Winner-only Certification
Only winner branch is validated and materialized
     |
     v
Commit Winner / Release Losers
```

---

## 4. 核心设计原则

### 4.1 多分支语义独立于并发控制算法

Branch 管理器只负责描述 Agent 事务内部结构，不直接绑定 OCC、2PL、MVCC 或 reservation。

应将如下语义抽象出来：

```text
begin_agent_txn
begin_branch
record_branch_read
record_branch_write_intent
select_winner
prepare_winner
commit_winner
release_losers
abort_agent_txn
```

---

### 4.2 loser branch 不进入数据库外部 abort 统计

loser branch 是 Agent 内部探索路径，不应被统计为数据库事务 abort。

建议区分三类 abort：

```text
planned_loser_abort:
  Agent 内部未被选择的候选分支释放

cc_abort:
  winner 分支在并发控制验证阶段失败

resource_abort:
  delta reservation 或语义约束提前拒绝
```

---

### 4.3 winner-only validation

提交阶段只验证 winner branch 的 read set、write intent 和 predicate dependency。

不应验证所有 loser branch，否则多分支事务会退化为多个普通事务的并集验证，造成 read set 和 write set 膨胀。

---

### 4.4 操作语义优先

并发控制策略应根据操作语义选择：

```text
增量写：
  使用 delta reservation / escrow

覆盖式写：
  使用 CAS / winner lease / exclusive write intent

追加写：
  使用 append intent / commutative log append

读操作：
  使用 MVCC snapshot / dependency validation

范围读或条件读：
  使用 predicate read intent / predicate validation
```

---

## 5. 模块一：Branch Manager 解耦

### 5.1 改造目标

将当前散落在 `txn.h`、`txn.cpp` 中的 agent branch 逻辑拆分到独立模块中，避免它继续和 OCC_RESERVE 强耦合。

建议新增文件：

```text
system/agent_txn.h
system/agent_txn.cpp
system/agent_branch.h
system/agent_branch.cpp
```

---

### 5.2 建议数据结构

```cpp
enum AgentBranchStatus {
    BRANCH_INIT,
    BRANCH_ACTIVE,
    BRANCH_FEASIBLE,
    BRANCH_INFEASIBLE,
    BRANCH_WINNER,
    BRANCH_LOSER,
    BRANCH_RELEASED
};

struct AgentBranch {
    uint32_t branch_id;
    AgentBranchStatus status;

    std::vector<ReadIntent> read_intents;
    std::vector<WriteIntent> write_intents;

    uint64_t start_ts;
    uint64_t end_ts;

    double score;
};
```

---

### 5.3 建议接口

```cpp
class AgentTxnManager {
public:
    RC begin_agent_txn(uint32_t branch_cnt);

    RC begin_branch(uint32_t branch_id);
    RC end_branch(uint32_t branch_id);

    RC record_read(row_t * row, uint32_t col, ReadMode mode);
    RC record_write_intent(const WriteIntent & intent);

    RC select_winner(uint32_t branch_id);

    RC prepare_winner();
    RC commit_winner();

    void release_loser_branches();
    void abort_agent_txn();

private:
    uint32_t branch_cnt;
    uint32_t current_branch;
    uint32_t winner_branch;

    std::vector<AgentBranch> branches;
};
```

---

### 5.4 验收标准

完成后应满足：

```text
1. 当前已有 TPCC_AGENT_NEW_ORDER_RESERVE 能继续运行。
2. begin_agent_branches 等接口可以通过 AgentTxnManager 调用。
3. OCC_RESERVE 不再直接维护所有 branch 内部状态。
4. loser branch release 逻辑统一由 AgentTxnManager 管理。
5. 原有单元测试通过。
```

---

## 6. 模块二：通用 Intent Layer

### 6.1 改造目标

将当前的 `reserve_row_delta()` 扩展为通用 intent 机制，使系统能表达增量写、覆盖写、条件写、追加写和谓词读。

---

### 6.2 Intent 类型

建议先支持六类 intent：


| Intent 类型          | 适用操作                    | 第一阶段是否实现 |
| ------------------ | ----------------------- | -------- |
| `DELTA_INTENT`     | 库存扣减、额度增减、座位占用          | 是        |
| `CAS_INTENT`       | 状态机更新、条件覆盖写             | 是        |
| `XWRITE_INTENT`    | 普通覆盖式写                  | 是        |
| `APPEND_INTENT`    | 日志追加、消息追加、tool trace 追加 | 第二阶段     |
| `READ_INTENT`      | 点读、版本读                  | 是        |
| `PRED_READ_INTENT` | 范围读、条件读、索引读             | 第三阶段     |


---

### 6.3 建议数据结构

```cpp
enum IntentType {
    INTENT_DELTA,
    INTENT_COMPARE_AND_SET,
    INTENT_EXCLUSIVE_WRITE,
    INTENT_APPEND,
    INTENT_READ,
    INTENT_PREDICATE_READ
};

struct ReadIntent {
    row_t * row;
    uint32_t col;
    uint64_t observed_version;
    uint64_t snapshot_ts;
};

struct WriteIntent {
    IntentType type;

    row_t * row;
    uint32_t col;

    int64_t delta;

    uint64_t expected_version;
    uint64_t observed_version;

    char * old_value;
    char * new_value;

    PredicateDesc predicate;
};
```

---

### 6.4 DELTA_INTENT

对应当前 reservation 机制。

语义：

```text
committed_value + pending_delta + new_delta >= lower_bound
```

适用：

```text
stock -= quantity
seat -= 1
quota -= x
budget += y
```

应保留当前 pending delta ledger，但从 `reserve_row_delta()` 改名或包装为：

```cpp
RC register_delta_intent(row_t * row, uint32_t col, int64_t delta, Constraint constraint);
```

---

### 6.5 CAS_INTENT

用于条件覆盖写。

示例：

```text
if order.status == "pending":
    order.status = "confirmed"
```

可以表达为：

```text
CAS(row, expected_version, expected_value, new_value)
```

建议接口：

```cpp
RC register_cas_intent(
    row_t * row,
    uint32_t col,
    uint64_t expected_version,
    char * expected_value,
    char * new_value
);
```

提交时验证：

```text
row.version == expected_version
AND current_value == expected_value
```

通过后安装 `new_value`。

---

### 6.6 XWRITE_INTENT

用于普通覆盖式写。

示例：

```text
task.result = generated_answer
user.address = new_address
```

第一阶段可以采用 OCC-style version validation：

```text
winner commit 时验证 row.version 未变化
```

第二阶段可加入 winner lease：

```text
select_winner 后短暂持有 row-level write lease
```

建议接口：

```cpp
RC register_xwrite_intent(
    row_t * row,
    uint32_t col,
    char * new_value
);
```

---

### 6.7 READ_INTENT

用于记录 branch 读依赖。

```cpp
RC register_read_intent(
    row_t * row,
    uint32_t col,
    uint64_t observed_version,
    uint64_t snapshot_ts
);
```

winner commit 时只验证 winner branch 的 read intent。

---

### 6.8 验收标准

完成后应满足：

```text
1. DELTA_INTENT 能复用当前 reservation 机制。
2. CAS_INTENT 能支持状态机类更新。
3. XWRITE_INTENT 能支持覆盖式写。
4. winner branch 的 intent 能被物化为真实 write set。
5. loser branch 的 intent 能被释放，不进入外部 abort 统计。
6. 原有 TPCC reserve 实验结果不应明显退化。
```

---

## 7. 模块三：CCPolicy 接口

### 7.1 改造目标

新增统一并发控制策略接口，使 AET 事务不再固定绑定 OCC。

建议新增文件：

```text
concurrency_control/aet_cc_policy.h
concurrency_control/aet_occ.cpp
concurrency_control/aet_reserve.cpp
concurrency_control/aet_hybrid.cpp
```

---

### 7.2 建议接口

```cpp
class CCPolicy {
public:
    virtual RC on_begin_agent_txn(AgentTxnManager * txn) = 0;

    virtual RC on_branch_read(
        AgentTxnManager * txn,
        AgentBranch * branch,
        row_t * row,
        uint32_t col
    ) = 0;

    virtual RC on_branch_write_intent(
        AgentTxnManager * txn,
        AgentBranch * branch,
        const WriteIntent & intent
    ) = 0;

    virtual RC on_select_winner(
        AgentTxnManager * txn,
        AgentBranch * winner
    ) = 0;

    virtual RC validate_winner(
        AgentTxnManager * txn,
        AgentBranch * winner
    ) = 0;

    virtual RC commit_winner(
        AgentTxnManager * txn,
        AgentBranch * winner
    ) = 0;

    virtual void release(
        AgentTxnManager * txn
    ) = 0;
};
```

---

### 7.3 第一阶段实现策略

第一阶段实现三个策略：

```text
AET_OCC:
  基于 OCC 的 winner-only validation

AET_RESERVE:
  当前 OCC_RESERVE 的重构版，支持 DELTA_INTENT

AET_HYBRID_RULE:
  基于规则选择 DELTA / CAS / XWRITE 的不同控制方式
```

---

### 7.4 验收标准

```text
1. 配置文件中可选择 CC_ALG = AET_OCC / AET_RESERVE / AET_HYBRID_RULE。
2. 当前 OCC_RESERVE 行为可通过 AET_RESERVE 复现。
3. AET_OCC 可在不使用 reservation 的情况下运行多分支事务。
4. AET_HYBRID_RULE 能根据 intent 类型选择处理逻辑。
```

---

## 8. 模块四：CAS 与覆盖式写支持

### 8.1 改造目标

解决当前预占用机制无法处理覆盖式写的问题。

---

### 8.2 优先实现 CAS_INTENT

CAS 是覆盖式写中最容易落地的版本，适合状态机类 workload。

示例场景：

```text
order.status: pending -> confirmed
task.state: running -> finished
reservation.state: active -> cancelled
```

提交规则：

```text
只有 winner branch 可以执行 CAS
CAS 提交前验证 expected_version / expected_value
验证失败则产生 cc_abort
验证成功则安装 new_value
loser branch 不执行 CAS
```

---

### 8.3 再实现 XWRITE_INTENT

第一版 XWRITE 可以只做版本验证：

```text
read version at branch execution
winner commit validates version
if unchanged, overwrite value
```

后续加入 winner lease：

```text
select_winner 后获取 row-level winner lease
lease 持有期间完成 validation 和 write
commit 后释放 lease
```

---

### 8.4 单元测试建议

新增测试：

```text
TEST_CAS_SUCCESS:
  初始 status = pending
  branch winner CAS pending -> confirmed
  commit 成功

TEST_CAS_VERSION_CONFLICT:
  branch 读 status version = v
  并发事务更新 status
  winner CAS validate 失败

TEST_XWRITE_WINNER_ONLY:
  多个 branch 都写 task.result
  只有 winner 的 result 被安装

TEST_XWRITE_LOSER_RELEASE:
  loser branch 的覆盖写不影响最终数据
```

---

## 9. 模块五：MVCC Snapshot Read

### 9.1 改造目标

解决 Agent 事务中长读、读多写少、多分支共享读上下文的问题。

当前 reserve 对读没有作用。如果 Agent 事务存在较长思考时间或多轮工具调用，基于单版本 OCC 的 read set validation 会导致 abort 增多。

建议引入轻量 MVCC snapshot：

```text
Root 阶段获取 snapshot_ts
所有 branch 共享 snapshot_ts
branch read 读取 <= snapshot_ts 的 committed version
winner commit 时验证必要依赖
```

---

### 9.2 最小实现方案

如果完整 MVCC 改造成本较高，可以先做简化版本：

```text
每行维护：
  committed_version
  committed_value
  optional old_value buffer

事务维护：
  snapshot_ts
  winner_read_set
  winner_write_set
```

第一阶段只支持点读，不支持范围读。

---

### 9.3 提交验证

winner commit 时检查：

```text
write-write conflict:
  winner 要写的 row 是否被并发事务更新

read-write dependency:
  winner 依赖的 read row 是否在 snapshot_ts 后被修改

predicate dependency:
  第三阶段再实现
```

---

### 9.4 验收标准

```text
1. AET 事务可以共享 root snapshot。
2. loser branch 的读集不进入验证。
3. winner branch 的读依赖可被验证。
4. 读多写少场景下，AET_MVCC 相比 AET_OCC abort 更少。
```

---

## 10. 模块六：Adaptive Policy Selector

### 10.1 改造目标

解决固定 OCC_RESERVE 难以适应复杂冲突率的问题。

不建议第一版直接做强化学习。先实现规则式 selector。

---

### 10.2 运行时统计

建议维护对象级统计：

```cpp
struct ObjectStats {
    uint64_t access_count;
    uint64_t read_count;
    uint64_t write_count;

    double abort_rate;
    double wait_time;
    double write_conflict_rate;
    double read_write_conflict_rate;

    bool delta_compatible;
    bool overwrite_hot;
};
```

事务级统计：

```cpp
struct TxnStats {
    uint32_t branch_cnt;
    uint32_t read_set_size;
    uint32_t write_set_size;

    uint64_t estimated_reasoning_cost;
    uint64_t execution_time;
};
```

---

### 10.3 策略选择规则

第一版规则：

```text
if intent.type == DELTA_INTENT and object.delta_compatible:
    use semantic reservation

else if intent.type == CAS_INTENT:
    use versioned conditional write

else if intent.type == XWRITE_INTENT and object.overwrite_hot:
    use winner lease or 2PL-style exclusive control

else if txn is read-heavy or long-running:
    use MVCC snapshot read

else:
    use OCC/SILO-style validation
```

---

### 10.4 策略输出

selector 输出：

```cpp
enum AETPolicyType {
    AET_POLICY_OCC,
    AET_POLICY_RESERVE,
    AET_POLICY_CAS,
    AET_POLICY_XWRITE_LEASE,
    AET_POLICY_MVCC,
    AET_POLICY_HYBRID
};
```

---

### 10.5 验收标准

```text
1. selector 能根据 intent 类型选择策略。
2. selector 能根据 object hotness 切换覆盖式写策略。
3. selector 的选择结果可记录到日志。
4. 实验结果能区分各策略触发次数。
```

---

## 11. Workload 改造计划

### 11.1 保留现有 TPC-C NewOrder

继续保留以下任务：

```text
TPCC_AGENT_NEW_ORDER_BASELINE
TPCC_AGENT_NEW_ORDER_RESERVE
TPCC_AGENT_NEW_ORDER_RESERVE_STANDARD
```

用途：

```text
验证多分支 winner-only commit
验证 delta reservation
和传统模型比较 planned loser abort
```

---

### 11.2 新增状态机 workload

用于验证 CAS_INTENT。

建议设计：

```text
TPCC_AGENT_ORDER_STATUS_CAS
```

事务逻辑：

```text
Root:
  read order status

Branch A:
  try status pending -> paid

Branch B:
  try status pending -> cancelled

Branch C:
  try status pending -> manual_review

Winner:
  only one status transition is committed
```

实验指标：

```text
cas_success_cnt
cas_conflict_cnt
cc_abort_cnt
planned_loser_abort_cnt
logical_throughput
```

---

### 11.3 新增覆盖式写 workload

用于验证 XWRITE_INTENT。

建议设计：

```text
AGENT_TASK_RESULT_XWRITE
```

事务逻辑：

```text
Root:
  read task metadata

Branch A:
  generate candidate answer A

Branch B:
  generate candidate answer B

Branch C:
  generate candidate answer C

Winner:
  write task.result = selected candidate
```

实验指标：

```text
winner_write_cnt
loser_release_cnt
xwrite_conflict_cnt
logical_throughput
```

---

### 11.4 新增读多写少 workload

用于验证 MVCC snapshot。

建议设计：

```text
AGENT_READ_HEAVY_SNAPSHOT
```

事务逻辑：

```text
Root:
  read user profile
  read policy
  read item list

Branches:
  read different candidate records
  generate branch score

Winner:
  write final decision
```

实验指标：

```text
read_set_size
winner_read_set_size
snapshot_abort_cnt
occ_validate_abort_cnt
logical_throughput
```

---

## 12. 实验设计

### 12.1 实验一：多分支语义验证

目的：

```text
验证 winner-only commit 能避免 branch 数增加导致的 success rate 稀释
```

配置：

```text
warehouses = 8
threads = 32
branches = 1 / 2 / 4 / 8 / 16
CC_ALG = OCC, SILO, TICTOC, AET_RESERVE
```

指标：

```text
success_rate
logical_throughput
attempt_throughput
abort_cnt
planned_loser_abort_cnt
```

预期：

```text
传统 baseline:
  success_rate 近似随 1 / branch_cnt 下降

AET:
  success_rate 基本稳定
  abort_cnt 不随 branch_cnt 线性放大
```

---

### 12.2 实验二：资源预占用验证

目的：

```text
验证 DELTA_INTENT 能提前拒绝资源不足分支，减少无效执行
```

配置：

```text
low stock
high ol_quantity
hot item access
warehouses = 1 / 2 / 8
threads = 16 / 32 / 64
branches = 4 / 8
```

指标：

```text
resource_abort_cnt
cc_abort_cnt
committed_stock_nonnegative
logical_throughput
invalid_attempt_cnt
```

预期：

```text
AET_RESERVE:
  resource_abort_cnt 上升
  committed stock 保持非负
  无效提交尝试减少
```

---

### 12.3 实验三：CAS 覆盖写验证

目的：

```text
验证状态机类覆盖写可以通过 CAS_INTENT 支持
```

配置：

```text
hot order status
threads = 16 / 32 / 64
branches = 2 / 4 / 8
```

指标：

```text
cas_success_cnt
cas_conflict_cnt
cc_abort_cnt
logical_throughput
final_state_correctness
```

预期：

```text
只有 winner branch 更新状态
并发冲突通过 CAS validate 捕获
loser branch 不产生外部 abort
```

---

### 12.4 实验四：读多写少与长事务验证

目的：

```text
验证 MVCC snapshot read 能减少长事务和读多写少场景下的 abort
```

配置：

```text
read_set_size = 10 / 50 / 100
thinking_time = 0us / 100us / 1ms / 10ms
threads = 16 / 32 / 64
branches = 4 / 8
```

指标：

```text
snapshot_abort_cnt
occ_validate_abort_cnt
logical_throughput
avg_latency
p95_latency
winner_read_set_size
```

预期：

```text
AET_MVCC 在 thinking_time 增加时比 AET_OCC 更稳定
winner-only validation 减少 read set 膨胀
```

---

### 12.5 实验五：混合策略验证

目的：

```text
验证 AET_HYBRID_RULE 能在不同操作语义和冲突率下选择更合适策略
```

混合 workload：

```text
30% delta resource update
30% CAS status update
20% XWRITE result update
20% read-heavy decision
```

指标：

```text
policy_selection_count
per_policy_abort_cnt
per_policy_throughput
overall_logical_throughput
success_rate
```

预期：

```text
AET_HYBRID_RULE 在混合负载下优于固定 OCC_RESERVE
```

---

## 13. 指标体系改造

建议在统计中增加以下指标：

```text
agent_txn_cnt:
  Agent 逻辑事务数量

branch_attempt_cnt:
  branch 执行数量

winner_commit_cnt:
  winner 成功提交数量

planned_loser_abort_cnt:
  未被选中的 branch 数量

resource_abort_cnt:
  资源约束导致的提前失败

cas_conflict_cnt:
  CAS 验证失败次数

xwrite_conflict_cnt:
  覆盖写冲突次数

snapshot_abort_cnt:
  MVCC / snapshot dependency 失败次数

cc_abort_cnt:
  并发控制验证失败次数

policy_selection_count:
  各策略被 selector 选择的次数
```

成功率建议分成三种：

```text
logical_success_rate = winner_commit_cnt / agent_txn_cnt

branch_feasible_rate = feasible_branch_cnt / branch_attempt_cnt

cc_success_rate = winner_commit_cnt / winner_validation_cnt
```

吞吐建议分成两种：

```text
logical_throughput:
  每秒成功完成的 Agent 逻辑事务数

attempt_throughput:
  每秒执行的底层 branch / candidate attempt 数
```

论文和实验报告中应以 `logical_throughput` 为主，`attempt_throughput` 只作为辅助指标。

---

## 14. 代码改造优先级

### P0：保持现有功能可运行

在任何重构前，先保证：

```text
make -j
现有 TPCC_AGENT_NEW_ORDER_RESERVE 能运行
现有 summary.csv 生成脚本能运行
现有单元测试通过
```

---

### P1：Branch Manager 解耦

任务：

```text
1. 新增 AgentTxnManager。
2. 将 branch 状态从 txn_man 中拆出。
3. 保留旧 API 的 wrapper，避免 workload 大规模改动。
4. 将 select_winner / release_loser 统一放入 AgentTxnManager。
```

验收：

```text
现有 TPC-C reserve 实验结果与重构前接近。
```

---

### P2：Intent Layer

任务：

```text
1. 新增 IntentType。
2. 将当前 reservation 封装为 DELTA_INTENT。
3. 新增 READ_INTENT。
4. 新增 CAS_INTENT。
5. 新增 XWRITE_INTENT。
```

验收：

```text
DELTA_INTENT 可复现当前 reservation 行为。
CAS_INTENT 和 XWRITE_INTENT 有独立单元测试。
```

---

### P3：CCPolicy 接口

任务：

```text
1. 新增 CCPolicy 抽象类。
2. 实现 AET_OCC。
3. 实现 AET_RESERVE。
4. 实现 AET_HYBRID_RULE。
5. 配置文件中加入新的算法选项。
```

验收：

```text
不同 AET 策略可通过配置切换。
```

---

### P4：新增 Workload

任务：

```text
1. 新增 TPCC_AGENT_ORDER_STATUS_CAS。
2. 新增 AGENT_TASK_RESULT_XWRITE。
3. 新增 AGENT_READ_HEAVY_SNAPSHOT。
4. 更新脚本生成 summary.csv。
```

验收：

```text
每个 workload 可独立运行并输出新指标。
```

---

### P5：MVCC Snapshot

任务：

```text
1. 为 row 增加简化版本信息。
2. 为 Agent txn 分配 snapshot_ts。
3. branch read 基于 snapshot_ts。
4. winner commit 验证 read dependency。
```

验收：

```text
读多写少 workload 下 AET_MVCC 比 AET_OCC 更稳定。
```

---

### P6：Adaptive Selector

任务：

```text
1. 增加 ObjectStats。
2. 增加 TxnStats。
3. 实现规则式策略选择。
4. 输出 policy_selection_count。
```

验收：

```text
混合 workload 下 AET_HYBRID_RULE 能按预期选择不同策略。
```

---

## 15. 风险与注意事项

### 15.1 不要把所有机制继续塞进 OCC_RESERVE

`OCC_RESERVE` 应保留为兼容模式或 baseline，不应继续无限扩展。

---

### 15.2 不要让 loser branch 污染 read set 和 write set

只验证 winner branch。loser branch 的 intent 应释放。

---

### 15.3 区分计划性 loser abort 和并发控制 abort

实验中必须单独统计：

```text
planned_loser_abort_cnt
cc_abort_cnt
resource_abort_cnt
```

否则会误判系统性能。

---

### 15.4 MVCC 不要第一版做太大

第一版只做 snapshot read + winner validation。暂时不要做完整 SSI、predicate locking 或复杂 GC。

---

### 15.5 自适应策略不要第一版做强化学习

先做规则式 selector，方便调试和解释。后续可扩展为学习型策略。

---

## 16. 最终交付物

后续 agent 应至少交付以下内容：

```text
1. 重构后的 Branch Manager 代码。
2. Intent Layer 代码。
3. CCPolicy 接口和至少三个策略实现：
   - AET_OCC
   - AET_RESERVE
   - AET_HYBRID_RULE
4. CAS_INTENT 和 XWRITE_INTENT 单元测试。
5. 至少三个新增 workload：
   - TPCC_AGENT_ORDER_STATUS_CAS
   - AGENT_TASK_RESULT_XWRITE
   - AGENT_READ_HEAVY_SNAPSHOT
6. 更新后的实验脚本。
7. 更新后的 summary.csv 指标字段。
8. 一份简短实现说明文档。
```

---

