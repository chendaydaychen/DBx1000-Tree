# DBx1000-Tree 向 Data Agent System 的增量改造计划

## 0. 改造目标

本计划面向现有 `DBx1000-Tree` 仓库进行增量改造，不新建独立项目，不推翻原生 DBx1000 的 benchmark、并发控制和运行脚本。

改造后的系统定位如下：

```text
Agent / Workload Driver
        ↓ submit_task
Data Agent Runtime
        ↓ 自动生成候选分支、执行分支、选择 winner
Agent-level Transaction / Branch / Intent Manager
        ↓ 编译为底层读写与提交操作
DBx1000 Backend
        ↓
Storage + Concurrency Control
```

核心目标不是只优化某一个 TPC-C 事务，而是在 DBx1000-Tree 上形成一个最小可运行的 **Data Agent System 原型**：

1. Agent 只提交任务目标，不显式调用 `begin / create_branch / commit`。
2. System 内部自动生成候选分支，并把一次探索式任务纳入一个 Agent-level transaction。
3. 每个分支产生候选结果和 staged writes。
4. 只有 winner branch 的写入最终提交到 DBx1000。
5. loser branch 被系统计划性释放，不作为真实事务失败。
6. DBx1000 继续作为底层存储与并发控制内核。

---

## 1. 当前仓库基础

现有仓库已经具备以下基础目录：

```text
DBx1000-Tree/
├── benchmarks/
├── concurrency_control/
├── docs/
├── libs/
├── logo/
├── scripts/
├── storage/
├── system/
├── CMakeLists.txt
├── Makefile
├── config.h
├── config.cpp
├── config-std.h
├── README
└── README.md
```

其中当前项目中已经有三块基础能力：

```text
benchmarks/             原生 YCSB / TPCC / test workload
concurrency_control/    原生 CC 算法以及 AET 相关并发控制扩展
system/                 DBx1000 事务、线程、统计、row/table 等核心结构
```

本次改造的原则是：

```text
保留原生 DBx1000 结构
新增 data_agent/ 作为上层 Data Agent System 原型
通过 adapter 调用 DBx1000 底层能力
逐步把现有 AgentTxn / branch / intent 逻辑整理成独立模块
```

---

## 2. 改造后的推荐目录结构

建议在现有仓库上增量变成如下结构：

```text
DBx1000-Tree/
├── README.md
├── Makefile
├── CMakeLists.txt
├── config.h
├── config.cpp
├── config-std.h
│
├── docs/
│   ├── data_agent_system_design.md
│   ├── transaction_model.md
│   ├── branch_management.md
│   ├── intent_model.md
│   ├── dbx1000_backend_mapping.md
│   └── experiment_plan.md
│
├── data_agent/
│   ├── runtime/
│   ├── operators/
│   ├── transaction/
│   ├── branch/
│   ├── intent/
│   ├── object_store/
│   ├── client/
│   └── common/
│
├── workloads/
│   ├── synthetic/
│   ├── vita_style/
│   └── tpcc_style/
│
├── benchmarks/
├── concurrency_control/
├── storage/
├── system/
├── scripts/
├── results/
└── tests/
```

新增目录的作用如下：

| 目录 | 作用 |
|---|---|
| `data_agent/` | Data Agent System 的上层框架主体 |
| `data_agent/runtime/` | 任务级运行时，负责一次 task 的端到端调度 |
| `data_agent/operators/` | 最小系统算子集合，由 system 自动调用 |
| `data_agent/transaction/` | Agent-level transaction 生命周期管理 |
| `data_agent/branch/` | 候选分支创建、执行、winner / loser 管理 |
| `data_agent/intent/` | 读写意图记录与提交策略分发 |
| `data_agent/object_store/` | 将上层对象访问映射到 DBx1000 key-value 读写 |
| `data_agent/client/` | Agent / workload driver 提交任务的接口 |
| `workloads/` | Data Agent workload 适配层 |
| `results/` | 统一保存实验输出 |
| `tests/` | 单元测试、集成测试和正确性测试 |

---

## 3. 各目录详细设计

### 3.1 `data_agent/runtime/`

作用：作为 Data Agent System 的运行时入口，负责把一个 task 从输入执行到最终结果。

建议文件：

```text
data_agent/runtime/
├── task_runtime.h
├── task_runtime.cpp
├── task_context.h
├── task_context.cpp
├── execution_plan.h
├── execution_plan.cpp
└── runtime_config.h
```

核心职责：

```text
接收 TaskSpec
创建 TaskContext
启动 Agent-level transaction
调用 CandidateGenerateOp
调用 BranchEvaluateOp
收集 CandidateResult
调用 WinnerSelectOp
调用 TransactionalApplyOp
返回 TaskResult
```

运行时对外不暴露底层事务指令。Agent 只看到：

```cpp
TaskResult submit_task(const TaskSpec &task);
```

不应该看到：

```cpp
begin();
create_branch();
select_winner();
commit();
```

---

### 3.2 `data_agent/operators/`

作用：定义最小执行算子集合。第一版不要做复杂查询优化，只实现固定闭环。

建议文件：

```text
data_agent/operators/
├── base_operator.h
├── task_ingest_op.h
├── task_ingest_op.cpp
├── candidate_generate_op.h
├── candidate_generate_op.cpp
├── branch_evaluate_op.h
├── branch_evaluate_op.cpp
├── candidate_collect_op.h
├── candidate_collect_op.cpp
├── winner_select_op.h
├── winner_select_op.cpp
├── transactional_apply_op.h
└── transactional_apply_op.cpp
```

最小算子集合：

| 算子 | 作用 |
|---|---|
| `TaskIngestOp` | 接收任务目标并标准化输入 |
| `CandidateGenerateOp` | 根据任务生成候选方案 |
| `BranchEvaluateOp` | 对每个候选方案执行分支评估 |
| `CandidateCollectOp` | 汇总候选分支结果 |
| `WinnerSelectOp` | 选择 winner branch |
| `TransactionalApplyOp` | 提交 winner branch，释放 loser branch |

注意：

```text
这些算子由 system 自动调度。
Agent 不显式调用这些算子。
这些算子也不是 begin/create_branch/commit 的简单换名。
```

---

### 3.3 `data_agent/transaction/`

作用：管理 Agent-level transaction。

建议文件：

```text
data_agent/transaction/
├── agent_txn.h
├── agent_txn.cpp
├── txn_context.h
├── txn_context.cpp
├── txn_state.h
├── txn_manager.h
└── txn_manager.cpp
```

核心结构：

```cpp
enum class AgentTxnState {
    CREATED,
    RUNNING,
    WAITING_WINNER,
    COMMITTING,
    COMMITTED,
    ABORTED
};

struct AgentTxnContext {
    uint64_t txn_id;
    uint64_t task_id;
    AgentTxnState state;
    std::vector<uint64_t> branch_ids;
    uint64_t winner_branch_id;
};
```

核心职责：

```text
为每个 task 创建事务上下文
维护 txn_id 与 task_id 的绑定
管理事务状态流转
触发 winner commit
处理整体 abort / retry
```

---

### 3.4 `data_agent/branch/`

作用：管理候选分支生命周期。

建议文件：

```text
data_agent/branch/
├── branch.h
├── branch.cpp
├── branch_state.h
├── branch_manager.h
├── branch_manager.cpp
├── branch_result.h
└── branch_result.cpp
```

分支状态：

```cpp
enum class BranchState {
    CREATED,
    RUNNING,
    STAGED,
    WINNER,
    LOSER,
    COMMITTED,
    DISCARDED
};
```

每个 branch 至少维护：

```cpp
struct Branch {
    uint64_t branch_id;
    uint64_t txn_id;
    BranchState state;
    std::vector<Intent> read_intents;
    std::vector<Intent> write_intents;
    CandidateResult result;
};
```

核心职责：

```text
创建候选分支
执行分支计划
记录局部读集
记录 staged writes
保存候选结果
标记 winner / loser
释放 loser branch
```

---

### 3.5 `data_agent/intent/`

作用：记录分支执行中产生的操作意图，并将意图路由到底层提交策略。

建议文件：

```text
data_agent/intent/
├── intent.h
├── intent.cpp
├── intent_type.h
├── intent_log.h
├── intent_log.cpp
├── intent_manager.h
├── intent_manager.cpp
├── policy_dispatcher.h
└── policy_dispatcher.cpp
```

第一版 intent 类型控制在 5 类：

```cpp
enum class IntentType {
    READ,
    APPEND,
    DELTA,
    CAS,
    OVERWRITE
};
```

每个 intent 记录：

```cpp
struct Intent {
    uint64_t txn_id;
    uint64_t branch_id;
    std::string object_id;
    IntentType type;
    std::string payload;
    std::string condition;
};
```

策略分发：

| Intent | 含义 | 第一版提交策略 |
|---|---|---|
| `READ` | 只读对象 | 版本验证 |
| `APPEND` | 追加写入 | staged append + 唯一性校验 |
| `DELTA` | 增量修改 | 增量合并或版本校验 |
| `CAS` | 条件更新 | 条件验证 + 原子提交 |
| `OVERWRITE` | 覆盖写入 | 版本验证 + 覆盖写 |

第一版可以先把复杂策略简化为：

```text
READ        -> validate read version
APPEND      -> staged write + commit
DELTA       -> staged write + commit-time validation
CAS         -> condition check + commit
OVERWRITE   -> conservative validation + commit
```

后续再接入 AET Hybrid CC 的具体优化。

---

### 3.6 `data_agent/object_store/`

作用：对上层提供统一对象访问接口，对下层映射到 DBx1000。

建议文件：

```text
data_agent/object_store/
├── object.h
├── object.cpp
├── object_id.h
├── object_codec.h
├── object_codec.cpp
├── object_store.h
├── dbx1000_object_store.h
└── dbx1000_object_store.cpp
```

第一版对象模型不要复杂：

```cpp
enum class ObjectType {
    TASK,
    DATA_RECORD,
    CANDIDATE_RESULT,
    TXN_METADATA
};

struct Object {
    std::string object_id;
    ObjectType object_type;
    std::string payload;
    uint64_t version;
};
```

映射关系：

```text
object_id  -> DBx1000 key
payload    -> DBx1000 row/value
version    -> DBx1000 row version or metadata
```

第一版只支持：

```text
get(object_id)
put(object)
batch_get(object_ids)
apply_intent(intent)
```

不需要支持：

```text
scan
filter
join
graph traversal
vector search
```

---

### 3.7 `data_agent/client/`

作用：提供外部任务提交接口。

建议文件：

```text
data_agent/client/
├── task_api.h
├── task_api.cpp
├── json_task_parser.h
└── json_task_parser.cpp
```

对外接口：

```cpp
TaskResult submit_task(const TaskSpec &task);
```

任务格式：

```cpp
struct TaskSpec {
    uint64_t task_id;
    std::string task_type;
    std::vector<std::string> input_objects;
    std::string goal;
    uint32_t max_candidates;
};
```

第一版支持：

```text
synthetic_exploratory_task
tpcc_style_task
vita_style_task
```

---

### 3.8 `workloads/`

作用：把不同任务源映射成统一 `TaskSpec`。

建议结构：

```text
workloads/
├── synthetic/
│   ├── synthetic_task_generator.cpp
│   ├── synthetic_loader.cpp
│   └── synthetic_runner.cpp
│
├── vita_style/
│   ├── vita_task_parser.cpp
│   ├── vita_loader.cpp
│   └── vita_runner.cpp
│
└── tpcc_style/
    ├── tpcc_task_mapper.cpp
    ├── new_order_task.cpp
    └── delivery_task.cpp
```

职责划分：

| workload | 第一阶段作用 |
|---|---|
| `synthetic/` | 最小闭环验证，不依赖真实复杂数据 |
| `vita_style/` | 接入类 VitaBench 数据任务 |
| `tpcc_style/` | 复用现有 TPCC 改造经验作为验证案例 |

注意：

```text
workloads/ 只是任务适配层。
不要把系统逻辑写死在 workload 里。
```

---

## 4. 现有目录如何保留和接入

### 4.1 `benchmarks/`

保留原生 benchmark：

```text
benchmarks/
├── ycsb/
├── tpcc/
└── test_txn/
```

作用：

```text
保留原始 DBx1000 能力
继续跑 YCSB / TPCC baseline
提供传统 CC 对比
```

Data Agent workload 不建议放在这里，避免和原生 DB benchmark 混在一起。

---

### 4.2 `concurrency_control/`

继续作为底层并发控制算法目录。

保留现有文件：

```text
concurrency_control/
├── occ.cpp
├── silo.cpp
├── tictoc.cpp
├── aet_reserve.cpp
├── aet_hybrid.cpp
├── aet_hybrid_cc.cpp
├── row_aet_hybrid.cpp
└── ...
```

后续改造重点：

```text
1. 接收 data_agent/intent/ 编译出的底层操作
2. 为 winner branch 提供原子提交支持
3. 支持 staged writes 的最终提交
4. 区分 planned loser 和 real abort
5. 继续统计 AET Hybrid CC 的性能指标
```

不要把上层 task / branch 生成逻辑写进 `concurrency_control/`。

---

### 4.3 `system/`

继续放 DBx1000 内核基础设施。

现有 `system/` 中如果已有 `agent_txn.h / agent_txn.cpp`，建议分两步处理：

第一阶段：

```text
保留 system/agent_txn.*
作为底层执行支持
新增 data_agent/transaction/ 作为上层事务语义
两者通过 adapter 调用
```

第二阶段：

```text
把不依赖 DBx1000 内部 row/txn 的语义逻辑迁移到 data_agent/transaction/
system/ 中只保留和底层事务执行强相关的结构
```

这样可以避免一次性大改导致编译和实验断裂。

---

### 4.4 `storage/`

现有 `storage/` 保持底层存储职责。

新增时可以放：

```text
storage/
├── object_serializer.h
├── object_serializer.cpp
├── object_row.h
└── object_row.cpp
```

作用：

```text
把上层 Object 编码为 DBx1000 row/value
提供 object_id 到 key 的映射
```

第一版不要在这里做复杂对象模型。

---

### 4.5 `scripts/`

补充 Data Agent System 运行脚本：

```text
scripts/
├── build.sh
├── run_dbx1000_native.sh
├── run_data_agent_synthetic.sh
├── run_data_agent_vita.sh
├── run_data_agent_tpcc_style.sh
├── run_all_data_agent_experiments.sh
├── parse_data_agent_results.py
└── plot_data_agent_results.py
```

---

### 4.6 `results/`

新增统一实验输出目录：

```text
results/
├── raw/
├── parsed/
├── figures/
└── logs/
```

不要再把实验结果散落到根目录或不同脚本目录里。

---

## 5. 推荐实现路线

### Phase 0：整理当前代码边界

目标：不改功能，先明确已有代码归属。

任务：

```text
1. 确认 system/agent_txn.*、concurrency_control/aet_* 当前职责
2. 列出现有 TPCC NewOrder / Agent NewOrder 入口
3. 保留所有原 benchmark 可运行
4. 新建 docs/data_agent_system_design.md
```

产物：

```text
docs/data_agent_system_design.md
docs/current_code_mapping.md
```

完成标准：

```text
make -j 能通过
原生 YCSB / TPCC 能继续运行
已有 Agent NewOrder 实验不受影响
```

---

### Phase 1：新增 `data_agent/` 骨架

目标：建立上层 Data Agent System 框架目录。

新增：

```text
data_agent/runtime/
data_agent/operators/
data_agent/transaction/
data_agent/branch/
data_agent/intent/
data_agent/object_store/
data_agent/client/
data_agent/common/
```

先实现空结构和最小类：

```text
TaskSpec
TaskResult
TaskContext
AgentTxnContext
Branch
Intent
Object
TaskRuntime
```

产物：

```text
data_agent/common/types.h
data_agent/client/task_api.h
data_agent/runtime/task_runtime.h
data_agent/transaction/txn_context.h
data_agent/branch/branch.h
data_agent/intent/intent.h
data_agent/object_store/object.h
```

完成标准：

```text
新增代码能编译
不影响原有 rundb
可以构造一个 TaskSpec 对象
```

---

### Phase 2：实现 synthetic 端到端闭环

目标：先不接 VitaBench，不接完整 TPCC，只跑通一个最小探索式事务。

任务流程：

```text
submit_task
  ↓
CandidateGenerateOp 生成 K 个候选
  ↓
BranchManager 创建 K 个 branch
  ↓
BranchEvaluateOp 为每个 branch 读取对象、生成 staged write
  ↓
WinnerSelectOp 选择一个 winner
  ↓
TransactionalApplyOp 提交 winner，丢弃 losers
```

新增：

```text
workloads/synthetic/synthetic_task_generator.cpp
workloads/synthetic/synthetic_runner.cpp
scripts/run_data_agent_synthetic.sh
```

完成标准：

```text
一个 task 能生成多个 branch
只有 winner branch 的写入生效
loser branch 的 staged write 不落库
输出 task_id / branch_cnt / winner_id / committed / discarded
```

---

### Phase 3：接入 DBx1000 ObjectStore Adapter

目标：让上层 Data Agent System 真正使用 DBx1000 作为后端。

实现：

```text
data_agent/object_store/dbx1000_object_store.h
data_agent/object_store/dbx1000_object_store.cpp
```

提供接口：

```cpp
Object get(const std::string &object_id);
bool put(const Object &object);
bool apply_intent(const Intent &intent);
bool commit_winner(uint64_t txn_id, uint64_t winner_branch_id);
```

底层映射：

```text
object_id -> DBx1000 key
payload   -> row data
intent    -> DBx1000 read/write operation
```

完成标准：

```text
synthetic task 的读写实际进入 DBx1000
winner commit 由 DBx1000 后端完成
可以统计 DBx1000 backend 的 abort / commit / latency
```

---

### Phase 4：对接 AET Hybrid CC

目标：把 intent 和底层 AET Hybrid CC 连接起来。

工作内容：

```text
1. PolicyDispatcher 根据 IntentType 选择后端操作
2. READ 映射到底层读验证
3. APPEND / DELTA / CAS / OVERWRITE 映射到已有或新增 AET 操作
4. winner branch commit 调用 AET Hybrid CC 提交流程
5. loser branch 不进入真实 commit，只释放 staged intent
```

建议第一版策略：

```text
READ      -> 版本验证
APPEND    -> staged write
DELTA     -> staged delta
CAS       -> 条件验证
OVERWRITE -> 保守写
```

完成标准：

```text
synthetic workload 能在 OCC / SILO / AET_HYBRID_CC 下运行
可以对比 planned loser 与 real abort
```

---

### Phase 5：接入 tpcc_style workload

目标：将现有 NewOrder / Agent NewOrder 经验迁移到统一框架下。

新增：

```text
workloads/tpcc_style/tpcc_task_mapper.cpp
workloads/tpcc_style/new_order_task.cpp
workloads/tpcc_style/delivery_task.cpp
```

改造方式：

```text
不要让 TPCC 事务直接控制 branch。
而是把 TPCC-style 任务映射成 TaskSpec。
由 data_agent/runtime 自动生成 branch、intent 和 winner commit。
```

完成标准：

```text
NewOrder-style task 通过 submit_task 跑通
原有 TPCC benchmark 仍可独立运行
tpcc_style 只作为 workload adapter 存在
```

---

### Phase 6：接入 vita_style workload

目标：把 VitaBench 类任务映射到统一 TaskSpec。

新增：

```text
workloads/vita_style/vita_task_parser.cpp
workloads/vita_style/vita_loader.cpp
workloads/vita_style/vita_runner.cpp
```

处理流程：

```text
VitaBench JSON
  ↓
TaskSpec
  ↓
CandidateGenerateOp
  ↓
BranchEvaluateOp
  ↓
WinnerSelectOp
  ↓
TransactionalApplyOp
```

完成标准：

```text
能够加载一批 vita_style task
每个 task 生成多个候选分支
输出 winner / loser / commit 统计
```

---

### Phase 7：实验脚本与指标

补充指标：

```text
task_cnt
agent_txn_cnt
branch_attempt_cnt
winner_commit_cnt
planned_loser_cnt
real_abort_cnt
staged_write_cnt
intent_cnt
commit_latency
branch_eval_latency
backend_validation_latency
throughput
```

脚本：

```text
scripts/run_data_agent_synthetic.sh
scripts/run_data_agent_tpcc_style.sh
scripts/run_data_agent_vita.sh
scripts/run_all_data_agent_experiments.sh
scripts/parse_data_agent_results.py
scripts/plot_data_agent_results.py
```

完成标准：

```text
能输出统一 CSV
能画 throughput / abort / branch_cnt / latency 图
能和原生 DBx1000 baseline 对比
```

---

## 6. 最小闭环版本

最小闭环只需要完成以下模块：

```text
data_agent/common/types.h
data_agent/client/task_api.h
data_agent/runtime/task_runtime.*
data_agent/operators/candidate_generate_op.*
data_agent/operators/branch_evaluate_op.*
data_agent/operators/winner_select_op.*
data_agent/operators/transactional_apply_op.*
data_agent/transaction/txn_manager.*
data_agent/branch/branch_manager.*
data_agent/intent/intent_manager.*
data_agent/object_store/dbx1000_object_store.*
workloads/synthetic/synthetic_runner.cpp
scripts/run_data_agent_synthetic.sh
```

最小执行流程：

```text
1. workload runner 创建 TaskSpec
2. TaskRuntime 接收 task
3. TxnManager 创建 Agent-level transaction
4. CandidateGenerateOp 生成 K 个候选
5. BranchManager 创建 K 个 branch
6. BranchEvaluateOp 执行每个 branch，产生 read intents 和 staged write intents
7. WinnerSelectOp 选择 winner branch
8. TransactionalApplyOp 提交 winner branch
9. DBx1000 backend 执行最终写入和提交验证
10. loser branch 被标记为 discarded
```

第一阶段不需要：

```text
复杂 scan / filter
图数据
文本数据
向量数据
真实 LLM 调用
复杂 query optimizer
完整 VitaBench
完整 TPCC 五类事务
```

---

## 7. 推荐 commit 拆分

建议按以下 commit 粒度推进：

```text
Commit 1: add data_agent skeleton and common types
Commit 2: add transaction / branch / intent managers
Commit 3: add object store adapter interface
Commit 4: add synthetic workload runner
Commit 5: connect synthetic runner to DBx1000 backend
Commit 6: connect intent dispatcher to AET Hybrid CC
Commit 7: add tpcc_style workload adapter
Commit 8: add vita_style workload parser
Commit 9: add scripts and result parser
Commit 10: add docs and end-to-end usage guide
```

每个 commit 都要保证：

```text
能编译
不破坏原生 DBx1000 benchmark
至少保留一个可运行脚本
```

---

## 8. 论文叙事和代码模块对应关系

| 论文概念 | 代码位置 |
|---|---|
| Data Agent System | `data_agent/runtime/` |
| Agent-level Transaction | `data_agent/transaction/` |
| Branch Management | `data_agent/branch/` |
| Semantic Intent | `data_agent/intent/` |
| Object Store Backend | `data_agent/object_store/` |
| DBx1000 Backend | `system/`, `storage/`, `concurrency_control/` |
| AET Hybrid CC | `concurrency_control/aet_hybrid_cc.*` |
| Workload Adapter | `workloads/` |
| Baseline Benchmark | `benchmarks/` |
| Experiment Pipeline | `scripts/`, `results/` |

---

## 9. 改造后的最终形态

最终系统应该可以这样运行：

```bash
# 原生 DBx1000 baseline
./scripts/run_dbx1000_native.sh

# Data Agent synthetic workload
./scripts/run_data_agent_synthetic.sh

# Data Agent TPCC-style workload
./scripts/run_data_agent_tpcc_style.sh

# Data Agent Vita-style workload
./scripts/run_data_agent_vita.sh
```

用户或 Agent 看到的接口是：

```text
submit_task(task_spec)
```

系统内部执行：

```text
TaskRuntime
  ↓
CandidateGenerateOp
  ↓
BranchManager
  ↓
BranchEvaluateOp
  ↓
IntentManager
  ↓
WinnerSelectOp
  ↓
TransactionalApplyOp
  ↓
DBx1000 Backend
```

最终输出：

```text
task_id
agent_txn_id
branch_count
winner_branch_id
planned_loser_count
real_abort_count
commit_status
latency
throughput
```

---

## 10. 一句话总结

这次改造不是把 DBx1000 改成完整 Agent 平台，也不是只给 NewOrder 加特判，而是在现有 DBx1000-Tree 仓库上新增一个轻量 Data Agent Runtime。上层 Runtime 负责任务、候选分支、winner 和意图管理；底层 DBx1000 继续负责对象存储、版本验证、并发控制和提交。这样既保留现有仓库和实验基础，又能把项目叙事从“DB 内多分支事务优化”提升为“面向 Data Agent System 的多分支事务管理框架”。
