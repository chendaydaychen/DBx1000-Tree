# DBx1000-Tree 下一步改造计划：规则式语义混合并发控制版本

## 0. 文档目的

本文档用于指导后续 agent 在 `DBx1000-Tree` 当前版本基础上继续改造。当前阶段不引入强化学习训练，也不做复杂运行时策略学习。下一步目标是实现一个 **规则式语义混合并发控制机制**，使系统能够根据不同读写 intent 选择固定或阈值驱动的并发控制策略，并在 winner-only certification 阶段统一验证和提交。

当前项目已经具备以下基础：

```text
1. 基于 DBx1000 的 TPC-C / TEST 运行框架。
2. OCC_RESERVE 原型。
3. Agent 多分支事务接口。
4. AgentTxnManager 雏形。
5. DELTA / READ / CAS / XWRITE 等 intent 数据结构。
6. AET_RESERVE / AET_OCC / AET_HYBRID_RULE / AET_HYBRID_SILO 等配置项雏形。
7. CAS 与 XWRITE 的基础单元测试。
```

下一步不应继续把所有功能堆进 `OCC_RESERVE`，而应将系统稳定推进为：

```text
Agent Exploration Transaction
        +
Intent Layer
        +
Rule-based Semantic Hybrid CC
        +
Winner-only Certification
```

---

## 1. 当前版本定位

当前版本可以定位为：

```text
OCC_RESERVE 原型
  -> AET 多分支事务雏形
  -> Intent Layer 初步实现
```

它已经证明：

```text
1. 一个 Agent 逻辑事务可以包含多个 candidate branch。
2. loser branch 不应作为外部事务 abort 统计。
3. winner branch 才进入真实提交路径。
4. DELTA reservation 可以处理库存、额度、座位等增量资源。
5. CAS / XWRITE intent 可以初步表达条件覆盖写和普通覆盖写。
```

但当前仍然存在以下不足：

```text
1. AET policy 接口较薄，尚未形成完整策略执行路径。
2. Adaptive Policy Selector 还只是配置名或接口雏形。
3. READ intent 主要是记录，缺少完整 winner read validation。
4. CAS intent 需要补齐 expected version + expected value 验证。
5. XWRITE intent 需要补齐 version validation 或 winner lease。
6. TPC-C 层仍主要集中在 NewOrder，Delivery / OrderStatus / StockLevel 未充分用于验证新 intent。
7. 统计指标不足，无法区分 planned loser abort、resource abort、CAS abort、XWRITE abort、read validation abort。
```

---

## 2. 下一阶段目标

下一阶段目标不是训练一个 RL selector，而是实现一个 **规则式语义混合并发控制版本**。

系统应做到：

```text
1. workload 层显式声明 intent。
2. intent layer 统一保存不同读写语义。
3. rule-based selector 根据 intent 类型选择 CC 策略。
4. winner-only certification 只验证 winner branch。
5. loser branch 的 intent 被释放，不进入外部 abort 统计。
6. 新增 TPC-C workload 覆盖 DELTA / CAS / XWRITE / READ / PRED_READ。
7. 实验能够证明：
   - 多分支 winner-only commit 有效；
   - DELTA reservation 适合资源型增量写；
   - CAS 适合状态机更新；
   - XWRITE 适合覆盖式写；
   - READ / PRED_READ 可为后续 snapshot / predicate validation 提供基础。
```

---

## 3. 不做的事情

当前阶段明确不做：

```text
1. 不做强化学习训练 selector。
2. 不做复杂在线学习。
3. 不做完整 Serializable Snapshot Isolation。
4. 不做完整 SQL parser 或自动语义识别。
5. 不做复杂 predicate locking。
6. 不做分布式事务。
7. 不重写整个 DBx1000 存储层。
```

当前阶段采用 **显式 intent + 规则选择 + 保守验证** 的路线。

---

## 4. 总体架构

目标架构如下：

```text
TPC-C / Agent Workload
  显式声明 intent
        |
        v
Intent Layer
  READ_INTENT
  DELTA_INTENT
  CAS_INTENT
  XWRITE_INTENT
  INSERT_INTENT
  DELETE_INTENT
  PRED_READ_INTENT
        |
        v
Rule-based Policy Selector
  根据 intent 类型选择固定 CC 策略
        |
        v
AET Hybrid CC
  Delta reservation
  CAS validation
  XWRITE version validation / winner lease
  Read validation
  Insert unique validation
  Delete version validation
        |
        v
Winner-only Certification
  只验证 winner branch
        |
        v
Commit Winner / Release Losers
```

---

## 5. 模块一：补齐 AET Policy 实现

### 5.1 目标

当前已有 `AETCCPolicy` 接口和若干配置项，但策略实现还不完整。下一步应补齐策略执行路径，让配置项真正生效。

建议新增或完善文件：

```text
concurrency_control/aet_cc_policy.cpp
concurrency_control/aet_reserve_policy.cpp
concurrency_control/aet_hybrid_rule_policy.cpp
concurrency_control/aet_occ_policy.cpp
```

如果不想拆太多文件，可以先只实现：

```text
concurrency_control/aet_cc_policy.cpp
```

---

### 5.2 需要实现的策略

#### 5.2.1 AET_RESERVE

用途：

```text
复现当前 OCC_RESERVE 行为。
处理 DELTA_INTENT。
支持 winner-only delta materialization。
```

执行逻辑：

```text
1. select_winner 后获取 winner branch。
2. 只物化 winner 的 DELTA_INTENT。
3. 释放 loser branch 的 pending delta。
4. 提交阶段仍走 OCC-style validate。
```

---

#### 5.2.2 AET_OCC

用途：

```text
作为不使用 reservation 的 AET baseline。
只保留多分支 winner-only 语义。
```

执行逻辑：

```text
1. branch 阶段只记录读写 intent。
2. winner 阶段将 winner intent 转换为普通 read/write set。
3. commit 阶段走 OCC validate。
4. loser branch 不进入外部 abort。
```

---

#### 5.2.3 AET_HYBRID_RULE

用途：

```text
当前阶段的主要目标策略。
根据 intent 类型使用固定规则选择 CC 方法。
```

第一版规则：

| Intent 类型 | 策略 |
|---|---|
| `DELTA_INTENT` | Delta reservation |
| `CAS_INTENT` | CAS validation |
| `XWRITE_INTENT` | Version validation |
| `READ_INTENT` | Winner read validation |
| `INSERT_INTENT` | Unique key validation |
| `DELETE_INTENT` | Version validation |
| 未知 intent | Conservative OCC validation |

---

### 5.3 建议接口

当前接口较薄，建议扩展为：

```cpp
class AETCCPolicy {
public:
    virtual RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn) = 0;

    virtual RC validate_winner(txn_man * txn, AgentTxnManager * agent_txn) = 0;

    virtual RC materialize_winner(txn_man * txn, AgentTxnManager * agent_txn) = 0;

    virtual void release_loser_branches(txn_man * txn, AgentTxnManager * agent_txn) = 0;

    virtual void abort_agent_txn(txn_man * txn, AgentTxnManager * agent_txn) = 0;
};
```

如果当前不想大改接口，也可以先保留现有 `prepare_winner()`，在内部串联：

```text
prepare_winner()
  -> validate_winner()
  -> materialize_winner()
  -> release_loser_branches()
```

---

### 5.4 验收标准

```text
1. CC_ALG = AET_RESERVE 可以复现当前 OCC_RESERVE 结果。
2. CC_ALG = AET_OCC 可以运行 Agent NewOrder。
3. CC_ALG = AET_HYBRID_RULE 可以处理 DELTA / CAS / XWRITE。
4. make clean && make -j 通过。
5. 原有 reserve / CAS / XWRITE 单元测试通过。
```

---

## 6. 模块二：补齐 Intent Layer

### 6.1 当前已有 intent

当前已经有：

```text
AGENT_INTENT_DELTA
AGENT_INTENT_COMPARE_AND_SET
AGENT_INTENT_EXCLUSIVE_WRITE
AGENT_INTENT_READ
```

下一步应在此基础上补齐：

```text
AGENT_INTENT_INSERT
AGENT_INTENT_DELETE
AGENT_INTENT_PRED_READ
```

如果时间有限，优先级为：

```text
P1: INSERT_INTENT
P2: DELETE_INTENT
P3: PRED_READ_INTENT
```

---

### 6.2 Intent 总表

| Intent | 语义 | 第一阶段是否必须 |
|---|---|---|
| `READ_INTENT` | 点读版本依赖 | 是 |
| `DELTA_INTENT` | 增量写 | 是 |
| `CAS_INTENT` | 条件覆盖写 | 是 |
| `XWRITE_INTENT` | 普通覆盖写 | 是 |
| `INSERT_INTENT` | 插入记录 | 是 |
| `DELETE_INTENT` | 删除记录 | 建议做 |
| `PRED_READ_INTENT` | 范围读 / 条件读 | 可先记录，后续验证 |
| `APPEND_INTENT` | 追加写 | 暂缓 |

---

### 6.3 建议数据结构

```cpp
enum AgentIntentType {
    AGENT_INTENT_READ,
    AGENT_INTENT_DELTA,
    AGENT_INTENT_COMPARE_AND_SET,
    AGENT_INTENT_EXCLUSIVE_WRITE,
    AGENT_INTENT_INSERT,
    AGENT_INTENT_DELETE,
    AGENT_INTENT_PRED_READ
};

struct AgentReadIntent {
    row_t * row;
    uint32_t col_id;
    uint64_t observed_version;
    uint64_t snapshot_ts;
};

struct AgentDeltaIntent {
    row_t * row;
    uint32_t col_id;
    int64_t delta;
    bool non_negative;
};

struct AgentWriteIntent {
    AgentIntentType type;
    row_t * row;
    uint32_t col_id;

    uint64_t expected_version;
    char expected_value[MAX_FIELD_SIZE];
    char new_value[MAX_FIELD_SIZE];

    bool has_expected_value;
    bool has_expected_version;
};

struct AgentInsertIntent {
    table_t * table;
    uint64_t key;
    char row_data[MAX_ROW_SIZE];
};

struct AgentDeleteIntent {
    row_t * row;
    uint64_t expected_version;
};

struct AgentPredReadIntent {
    INDEX * index;
    PredicateDesc pred;
    uint64_t snapshot_ts;
};
```

---

## 7. 模块三：规则式 Policy Selector

### 7.1 目标

不做 RL，不做学习。只做规则式选择。

Selector 的输入：

```text
1. intent 类型
2. 表名和字段名
3. 是否热点对象
4. 是否多分支事务
5. 是否资源约束字段
6. 是否覆盖写字段
7. 是否只读或范围读
```

Selector 的输出：

```text
具体 CC 策略
```

---

### 7.2 第一版固定规则

| Intent | 默认策略 | 说明 |
|---|---|---|
| `READ_INTENT` | `POLICY_READ_VERSION_VALIDATE` | winner 提交时验证版本 |
| `DELTA_INTENT` | `POLICY_DELTA_RESERVATION` | 用 pending delta ledger |
| `CAS_INTENT` | `POLICY_CAS_VALIDATE` | 验证 expected value + version |
| `XWRITE_INTENT` | `POLICY_XWRITE_VERSION_VALIDATE` | 验证版本未变后覆盖 |
| `INSERT_INTENT` | `POLICY_INSERT_UNIQUE_VALIDATE` | 验证 key 未存在 |
| `DELETE_INTENT` | `POLICY_DELETE_VERSION_VALIDATE` | 验证 row 存在且版本未变 |
| `PRED_READ_INTENT` | `POLICY_PRED_RECORD_ONLY` | 第一阶段只记录，后续再验证 |
| unknown | `POLICY_CONSERVATIVE_OCC` | 保守路径 |

---

### 7.3 第二版阈值规则

后续可以加简单阈值，但仍不训练：

```text
if XWRITE object conflict_rate > threshold:
    use winner lease

if READ transaction read_set_size > threshold:
    use snapshot read

if DELTA resource abort_rate high:
    use strict reservation

if object is cold:
    use OCC version validation
```

当前文档只要求第一版固定规则。

---

### 7.4 建议代码

```cpp
enum AETPolicyType {
    POLICY_READ_VERSION_VALIDATE,
    POLICY_DELTA_RESERVATION,
    POLICY_CAS_VALIDATE,
    POLICY_XWRITE_VERSION_VALIDATE,
    POLICY_INSERT_UNIQUE_VALIDATE,
    POLICY_DELETE_VERSION_VALIDATE,
    POLICY_PRED_RECORD_ONLY,
    POLICY_CONSERVATIVE_OCC
};

AETPolicyType choose_policy(const AgentIntent & intent) {
    switch (intent.type) {
        case AGENT_INTENT_READ:
            return POLICY_READ_VERSION_VALIDATE;

        case AGENT_INTENT_DELTA:
            return POLICY_DELTA_RESERVATION;

        case AGENT_INTENT_COMPARE_AND_SET:
            return POLICY_CAS_VALIDATE;

        case AGENT_INTENT_EXCLUSIVE_WRITE:
            return POLICY_XWRITE_VERSION_VALIDATE;

        case AGENT_INTENT_INSERT:
            return POLICY_INSERT_UNIQUE_VALIDATE;

        case AGENT_INTENT_DELETE:
            return POLICY_DELETE_VERSION_VALIDATE;

        case AGENT_INTENT_PRED_READ:
            return POLICY_PRED_RECORD_ONLY;

        default:
            return POLICY_CONSERVATIVE_OCC;
    }
}
```

---

## 8. 模块四：Winner-only Certification

### 8.1 目标

统一所有 intent 的验证入口。只验证 winner branch。

不要验证 loser branch 的 read set 或 write set。

---

### 8.2 验证顺序

建议顺序：

```text
1. validate_read_intents()
2. validate_delta_intents()
3. validate_cas_intents()
4. validate_xwrite_intents()
5. validate_insert_intents()
6. validate_delete_intents()
7. materialize_winner()
8. release_loser_branches()
```

---

### 8.3 READ_INTENT 验证

第一阶段采用版本验证：

```text
observed_version == current_version
```

如果不满足：

```text
read_validate_abort_cnt++
return Abort
```

注意：

```text
只验证 winner branch 的 read intent。
loser branch 的 read intent 直接释放。
```

---

### 8.4 DELTA_INTENT 验证

沿用当前 reservation：

```text
committed_value + pending_delta + delta >= lower_bound
```

通过后：

```text
winner delta materialize into local write set
```

失败：

```text
resource_abort_cnt++
return Abort
```

---

### 8.5 CAS_INTENT 验证

需要补齐两项：

```text
current_value == expected_value
current_version == expected_version
```

如果没有 expected_version，可以先只验证 value，但建议统一补上版本验证。

失败：

```text
cas_abort_cnt++
return Abort
```

通过后：

```text
install new_value into local write set
```

---

### 8.6 XWRITE_INTENT 验证

第一阶段使用版本验证：

```text
current_version == observed_version
```

通过后：

```text
install new_value into local write set
```

失败：

```text
xwrite_abort_cnt++
return Abort
```

第二阶段可加入 winner lease，但当前文档不要求必须实现。

---

### 8.7 INSERT_INTENT 验证

第一阶段：

```text
key not exists
```

通过后：

```text
insert row
```

失败：

```text
insert_abort_cnt++
return Abort
```

---

### 8.8 DELETE_INTENT 验证

第一阶段：

```text
row exists
current_version == expected_version
```

通过后：

```text
mark delete or remove row
```

失败：

```text
delete_abort_cnt++
return Abort
```

---

## 9. 模块五：TPC-C 语义映射

DBx1000 没有 SQL parser，数据库不能自动识别业务语义。语义由 workload 层显式声明。

### 9.1 NewOrder

| 操作 | Intent | 策略 |
|---|---|---|
| 读 `WAREHOUSE.W_TAX` | `READ_INTENT` | 版本验证 |
| 读 `CUSTOMER.C_DISCOUNT` | `READ_INTENT` | 版本验证 |
| 读 `DISTRICT.D_TAX` | `READ_INTENT` | 版本验证 |
| `DISTRICT.D_NEXT_O_ID += 1` | `DELTA_INTENT` 或 `ALLOC_INTENT` | 原子增量 |
| 读 `ITEM` | `READ_INTENT` | 版本验证 |
| `STOCK.S_QUANTITY -= OL_QUANTITY` | `DELTA_INTENT` | reservation |
| `STOCK.S_YTD += OL_QUANTITY` | `DELTA_INTENT` | delta |
| `STOCK.S_ORDER_CNT += 1` | `DELTA_INTENT` | delta |
| `STOCK.S_REMOTE_CNT += 1` | `DELTA_INTENT` | delta |
| 插入 `ORDERS` | `INSERT_INTENT` | unique validation |
| 插入 `NEW_ORDER` | `INSERT_INTENT` | unique validation |
| 插入 `ORDER_LINE` | `INSERT_INTENT` | unique validation |

NewOrder 主要用于验证：

```text
多分支 winner-only commit
DELTA reservation
INSERT intent
```

---

### 9.2 Payment

| 操作 | Intent | 策略 |
|---|---|---|
| `WAREHOUSE.W_YTD += amount` | `DELTA_INTENT` | delta |
| `DISTRICT.D_YTD += amount` | `DELTA_INTENT` | delta |
| `CUSTOMER.C_BALANCE -= amount` | `DELTA_INTENT` | delta |
| `CUSTOMER.C_YTD_PAYMENT += amount` | `DELTA_INTENT` | delta |
| `CUSTOMER.C_PAYMENT_CNT += 1` | `DELTA_INTENT` | delta |
| 更新 `CUSTOMER.C_DATA` | `XWRITE_INTENT` | version validation |
| 插入 `HISTORY` | `INSERT_INTENT` | insert validation |

Payment 用于验证：

```text
DELTA + XWRITE + INSERT
```

---

### 9.3 Delivery

| 操作 | Intent | 策略 |
|---|---|---|
| 找最小 `NEW_ORDER.NO_O_ID` | `PRED_READ_INTENT` | 第一阶段可记录 |
| 删除 `NEW_ORDER` | `DELETE_INTENT` | version validation |
| 设置 `ORDERS.O_CARRIER_ID` | `CAS_INTENT` | CAS validation |
| 设置 `ORDER_LINE.OL_DELIVERY_D` | `XWRITE_INTENT` | version validation |
| `CUSTOMER.C_BALANCE += sum_amount` | `DELTA_INTENT` | delta |
| `CUSTOMER.C_DELIVERY_CNT += 1` | `DELTA_INTENT` | delta |

Delivery 是下一步最应打开的 TPC-C 事务，因为它能覆盖：

```text
CAS
XWRITE
DELETE
PRED_READ
DELTA
```

---

### 9.4 OrderStatus

| 操作 | Intent | 策略 |
|---|---|---|
| 读 customer | `READ_INTENT` | 版本验证 |
| 查询最近订单 | `PRED_READ_INTENT` | 记录或谓词验证 |
| 读 order line | `READ_INTENT` | 版本验证 |

OrderStatus 用于验证：

```text
READ
PRED_READ
读多写少事务
```

---

### 9.5 StockLevel

| 操作 | Intent | 策略 |
|---|---|---|
| 读 `D_NEXT_O_ID` | `READ_INTENT` | 版本验证 |
| 查询最近订单行 | `PRED_READ_INTENT` | 记录或谓词验证 |
| 读 `S_QUANTITY` | `READ_INTENT` | 版本验证 |
| 聚合低库存数量 | `PRED_READ_INTENT` | 记录或谓词验证 |

StockLevel 用于验证：

```text
PRED_READ
READ
范围查询
```

---

## 10. 模块六：新增或完善统计指标

当前统计无法支撑完整分析，需要新增：

```text
agent_txn_cnt
branch_attempt_cnt
winner_commit_cnt
planned_loser_abort_cnt
resource_abort_cnt
read_validate_abort_cnt
cas_abort_cnt
xwrite_abort_cnt
insert_abort_cnt
delete_abort_cnt
predicate_abort_cnt
aet_policy_read_cnt
aet_policy_delta_cnt
aet_policy_cas_cnt
aet_policy_xwrite_cnt
aet_policy_insert_cnt
aet_policy_delete_cnt
```

### 10.1 指标定义

| 指标 | 含义 |
|---|---|
| `agent_txn_cnt` | Agent 逻辑事务数量 |
| `branch_attempt_cnt` | 执行过的候选 branch 数量 |
| `winner_commit_cnt` | winner 成功提交数量 |
| `planned_loser_abort_cnt` | 未被选择的 loser branch 数量 |
| `resource_abort_cnt` | 资源预占用失败 |
| `read_validate_abort_cnt` | winner 读验证失败 |
| `cas_abort_cnt` | CAS 验证失败 |
| `xwrite_abort_cnt` | 覆盖写版本验证失败 |
| `insert_abort_cnt` | 插入唯一键冲突 |
| `delete_abort_cnt` | 删除版本验证失败 |
| `predicate_abort_cnt` | 谓词验证失败 |
| `aet_policy_*_cnt` | 各策略触发次数 |

### 10.2 成功率定义

建议区分：

```text
logical_success_rate = winner_commit_cnt / agent_txn_cnt

branch_feasible_rate = feasible_branch_cnt / branch_attempt_cnt

cc_success_rate = winner_commit_cnt / winner_validation_cnt
```

实验报告主指标用：

```text
logical_throughput
logical_success_rate
```

不要把 `attempt_throughput` 作为主指标。

---

## 11. 模块七：单元测试计划

### 11.1 必须保留

```text
RESERVE_SUCCESS
RESERVE_ABORT_RELEASE
RESERVE_OVERDRAW
AET_CAS
AET_XWRITE
```

### 11.2 新增测试

#### TEST_READ_VALIDATE_SUCCESS

```text
branch read row version v
无并发更新
winner validate 成功
```

#### TEST_READ_VALIDATE_ABORT

```text
branch read row version v
并发事务更新 row 到 v+1
winner validate 失败
read_validate_abort_cnt++
```

#### TEST_CAS_VERSION_SUCCESS

```text
expected_value 匹配
expected_version 匹配
commit 成功
```

#### TEST_CAS_VERSION_ABORT

```text
expected_value 匹配
expected_version 不匹配
CAS 失败
cas_abort_cnt++
```

#### TEST_XWRITE_VERSION_ABORT

```text
winner branch 记录 observed_version
并发事务修改 row
winner xwrite validate 失败
xwrite_abort_cnt++
```

#### TEST_INSERT_UNIQUE_ABORT

```text
两个事务插入同一个 key
其中一个成功，另一个 insert_abort
```

#### TEST_DELETE_VERSION_ABORT

```text
branch 记录待删除 row version
并发事务更新该 row
delete validate 失败
```

#### TEST_LOSER_INTENT_RELEASE

```text
branch 0 写 loser_value
branch 1 写 winner_value
select winner = branch 1
commit 后只能看到 winner_value
loser intent 不泄漏
planned_loser_abort_cnt 增加
```

---

## 12. 模块八：实验计划

### 12.1 实验一：多分支 winner-only commit

目的：

```text
验证 branch 数增加时，传统 baseline 成功率下降，而 AET 成功率稳定。
```

配置：

```text
warehouses = 8
threads = 32
branches = 1 / 2 / 4 / 8 / 16
CC_ALG = OCC / SILO / TICTOC / AET_RESERVE / AET_OCC
```

指标：

```text
logical_throughput
logical_success_rate
abort_cnt
planned_loser_abort_cnt
attempt_throughput
```

---

### 12.2 实验二：DELTA reservation

目的：

```text
验证资源型增量写可以提前拒绝不可行分支。
```

配置：

```text
low stock
high quantity
hot item
threads = 16 / 32 / 64
branches = 4 / 8
```

指标：

```text
resource_abort_cnt
logical_throughput
committed_stock_nonnegative
cc_abort_cnt
```

---

### 12.3 实验三：CAS 状态机写

目的：

```text
验证 CAS_INTENT 适合状态机更新。
```

建议 workload：

```text
TPCC_AGENT_DELIVERY_CAS
```

指标：

```text
cas_abort_cnt
winner_commit_cnt
logical_success_rate
final_state_correctness
```

---

### 12.4 实验四：XWRITE 覆盖写

目的：

```text
验证普通覆盖写只物化 winner branch。
```

建议 workload：

```text
AGENT_TASK_RESULT_XWRITE
或 Delivery 中的 ORDER_LINE.OL_DELIVERY_D
```

指标：

```text
xwrite_abort_cnt
winner_write_cnt
loser_release_cnt
final_value_correctness
```

---

### 12.5 实验五：混合事务

目的：

```text
验证 AET_HYBRID_RULE 能在一个事务中同时处理多种 intent。
```

建议 workload：

```text
TPCC_AGENT_DELIVERY_HYBRID
```

包含：

```text
PRED_READ
DELETE
CAS
XWRITE
DELTA
```

指标：

```text
aet_policy_read_cnt
aet_policy_delta_cnt
aet_policy_cas_cnt
aet_policy_xwrite_cnt
aet_policy_delete_cnt
logical_throughput
logical_success_rate
```

---

## 13. 建议工程顺序

### P0：确认当前代码可编译

```bash
make clean
make -j
```

如果出现 AET policy 链接错误，优先补齐：

```text
get_aet_policy()
get_aet_reserve_policy()
get_aet_hybrid_policy()
```

---

### P1：补齐 AET Policy

```text
1. 实现 AET_RESERVE。
2. 实现 AET_OCC。
3. 实现 AET_HYBRID_RULE。
4. 保证配置项可切换。
```

---

### P2：补齐 winner certification

```text
1. validate_read_intents。
2. validate_delta_intents。
3. validate_cas_intents。
4. validate_xwrite_intents。
5. materialize_winner。
6. release_loser_branches。
```

---

### P3：完善 CAS / XWRITE

```text
1. CAS 加 expected_version 验证。
2. XWRITE 加 observed_version 验证。
3. 补齐对应 abort 统计。
```

---

### P4：新增 INSERT / DELETE intent

```text
1. insert_intent。
2. delete_intent。
3. unique validation。
4. delete version validation。
```

---

### P5：打开 Delivery

```text
1. 在 tpcc_txn.cpp 中启用 Delivery 或新增 Agent Delivery。
2. 将 O_CARRIER_ID 映射为 CAS。
3. 将 OL_DELIVERY_D 映射为 XWRITE。
4. 将 CUSTOMER 余额和次数映射为 DELTA。
5. 将 NEW_ORDER 出队映射为 DELETE。
```

---

### P6：补统计和脚本

```text
1. stats.h 增加新指标。
2. stats.cpp 输出新指标。
3. parser / scripts 增加 AET_HYBRID_RULE 实验。
4. summary.csv 增加新字段。
```

---

## 14. 最短可交付路线

如果时间有限，建议最短路线如下：

```text
Step 1:
  补齐 AET_HYBRID_RULE policy，使它能处理 DELTA / CAS / XWRITE / READ。

Step 2:
  CAS 加 expected_version 验证。

Step 3:
  XWRITE 加 observed_version 验证。

Step 4:
  新增 planned_loser_abort_cnt、cas_abort_cnt、xwrite_abort_cnt、read_validate_abort_cnt。

Step 5:
  新增或完善 TEST workload，证明：
    - loser intent 不泄漏；
    - winner CAS 正确；
    - winner XWRITE 正确；
    - read validation 可触发 abort。

Step 6:
  打开 Delivery 的一个简化版本，至少覆盖：
    - CAS: O_CARRIER_ID
    - XWRITE: OL_DELIVERY_D
    - DELTA: C_BALANCE
```

这条路线完成后，就能支撑论文表述：

```text
我们从单一 OCC_RESERVE 扩展到规则式语义混合并发控制原型。系统能够根据 intent 类型对增量写、条件覆盖写、普通覆盖写和读依赖采用不同验证方法，并通过 winner-only certification 保证 Agent 逻辑事务的原子提交。
```

---

## 15. 论文表述建议

可以写成：

```text
本文当前阶段不采用强化学习训练策略选择器，而是实现规则式语义混合并发控制。事务执行层通过 intent API 显式声明访问语义，包括点读、增量写、条件覆盖写、普通覆盖写、插入和删除。并发控制层根据 intent 类型采用固定验证策略：增量写使用 delta reservation，条件覆盖写使用 CAS validation，普通覆盖写使用版本验证，点读使用 winner read validation，插入使用唯一键验证，删除使用版本验证。所有策略最终汇入 winner-only certification，只对被选中的 winner branch 进行验证和物化，loser branch 的 intent 被释放，不作为外部事务 abort 统计。
```

---

## 16. 最终判断

下一步的重点不是做智能 selector，而是把 **规则式语义混合机制的闭环** 做完整：

```text
intent 声明
  -> policy 选择
  -> winner 验证
  -> winner 物化
  -> loser 释放
  -> 细粒度统计
  -> TPC-C workload 覆盖
```

只要这个闭环完成，项目就能从“基于 OCC 的预占用优化”升级为“面向 Agent 探索事务的规则式混合并发控制原型”。

---

## 17. 当前完成状态

本轮已经完成最短可交付路线中的核心闭环：

```text
intent 声明
  -> rule-based policy selector
  -> winner-only read / CAS / XWRITE validation
  -> winner materialization
  -> loser release
  -> 细粒度统计
  -> TEST + TPC-C Agent NewOrder smoke 验证
```

已完成项：

```text
1. AET_HYBRID_RULE / AET_HYBRID_SILO 均支持 DELTA / READ / CAS / XWRITE。
2. 新增 rule-based selector：READ、DELTA、CAS、XWRITE、INSERT、DELETE、PRED_READ 均有固定策略枚举。
3. READ_INTENT 增加 winner-only version validation。
4. CAS_INTENT 增加 expected version + expected value validation。
5. XWRITE_INTENT 增加 observed version validation。
6. 新增细粒度统计：
   agent_txn_cnt
   branch_attempt_cnt
   winner_commit_cnt
   planned_loser_abort_cnt
   read_validate_abort_cnt
   cas_abort_cnt
   xwrite_abort_cnt
   insert_abort_cnt
   delete_abort_cnt
   predicate_abort_cnt
   aet_policy_*_cnt
7. scripts/run_tpcc_compare.sh 已输出新增统计字段到 summary.csv。
8. 新增 TEST case：
   AET_READ_VALIDATE_ABORT
   AET_CAS_VERSION_ABORT
   AET_XWRITE_VERSION_ABORT
9. 保留并通过原有 AET_CAS / AET_XWRITE winner-only 测试。
```

验证结果：

```text
make clean && make -j 通过。

AET_HYBRID_RULE:
  AET_CAS TEST PASSED
  AET_XWRITE TEST PASSED
  AET_READ_VALIDATE_ABORT TEST PASSED
  AET_CAS_VERSION_ABORT TEST PASSED
  AET_XWRITE_VERSION_ABORT TEST PASSED

AET_HYBRID_SILO:
  AET_CAS TEST PASSED
  AET_XWRITE TEST PASSED
  AET_READ_VALIDATE_ABORT TEST PASSED
  AET_CAS_VERSION_ABORT TEST PASSED
  AET_XWRITE_VERSION_ABORT TEST PASSED

TPC-C Agent NewOrder smoke:
  AET_HYBRID_RULE b4 passed
  AET_HYBRID_SILO b4 passed
```

TPC-C smoke 中新增统计已生效。例如 b4 下：

```text
agent_txn_cnt = committed logical Agent transactions
branch_attempt_cnt ~= 4 * agent_txn_cnt
planned_loser_abort_cnt ~= 3 * agent_txn_cnt
aet_policy_read_cnt / delta_cnt / cas_cnt 均随 winner intent 触发
```

仍未完成、留作下一阶段：

```text
1. INSERT_INTENT / DELETE_INTENT 的真实物化路径。
2. PRED_READ_INTENT 的 workload 接入和谓词验证。
3. TPC-C Delivery 的 Agent 化版本。
4. Payment / OrderStatus / StockLevel 的语义化 intent 映射。
5. INSERT unique validation 和 DELETE version validation 的完整单元测试。
```
