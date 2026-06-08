# Tree 事务与资源预占用机制迁移指南

本文档面向后续接手的 Agent，目标是把 Tree-DB 中已经实现的 Tree 事务与资源预占用机制迁移到原生 DBx1000，方便后续在 DBx1000 的 TPC-C/YCSB 框架中做开发和测试。

当前参考实现位置：

- Tree-DB 协议层：`/home/cht/Tree-DB/Server/session.cpp`
- Tree-DB API 层：`/home/cht/Tree-DB/Server/kv_api.cpp`
- Tree 事务状态定义：`/home/cht/Tree-DB/Storage/System/tree_txn.h`
- Tree 事务状态合并：`/home/cht/Tree-DB/Storage/System/tree_txn.cpp`
- Tree/OCC/reserve 核心实现：`/home/cht/Tree-DB/Storage/System/txn.cpp`
- Tree-DB 行管理增强：`/home/cht/Tree-DB/Storage/concurrency_control/occ.h`
- Tree-DB 行版本与锁实现：`/home/cht/Tree-DB/Storage/concurrency_control/occ_row.cpp`
- VitaBench KV 测试入口：`/home/cht/Tree-DB/scripts/vitabench_delivery_kv_test.py`
- TPC-C New-Order KV 测试入口：`/home/cht/Tree-DB/scripts/tpcc_new_order_kv_test.py`

原生 DBx1000 目标位置：

- 事务管理器：`system/txn.h`, `system/txn.cpp`
- 行管理器：`concurrency_control/row_occ.h`, `concurrency_control/row_occ.cpp`
- OCC 验证：`concurrency_control/occ.cpp`, `concurrency_control/occ.h`
- TPC-C 工作负载：`benchmarks/tpcc_query.*`, `benchmarks/tpcc_txn.cpp`, `benchmarks/tpcc_wl.cpp`
- 编译配置：`config.h`, `config-std.h`, `Makefile` 或 `CMakeLists.txt`

## 1. 机制的核心目标

这套机制服务于一类“多候选、只提交一个候选”的事务。典型例子是：

- 外卖配送：多个候选门店都可以完成订单，事务先评估多个候选，最后只选择一个门店扣库存、写订单。
- TPC-C New-Order 改造：多个候选供货仓库都可能满足订单，事务先评估候选仓库，最后只选择一个仓库扣减 Stock、写 Order。
- 座位/库存/配额类业务：多个候选资源都可满足请求，最终只占用一个资源。

传统事务模型通常需要把每个候选都当作独立事务尝试，或者在一个大事务中读取大量候选并最终提交一个候选。前者会产生很多计划性失败候选，后者会把非关键候选读集纳入冲突检测。Tree 事务的设计目标是：

1. 在一个事务上下文中表达多个候选分支。
2. 每个分支有自己的读写集和暂存写入。
3. 选择 winner 后，只合并 winner 路径上的严格读集与写集。
4. loser 分支被丢弃，不参与最终提交验证。
5. 对库存、座位、配额这类可交换资源，允许 winner 提前做资源预占用，减少最后提交阶段的冲突窗口。

## 2. Tree 事务语义

Tree 事务不是替换 DBx1000 的所有并发控制算法，而是在 `txn_man` 上增加一种事务执行模式。普通 DBx1000 事务仍然走原来的 `run_txn()`、`get_row()`、`finish()` 路径。

Tree 事务的最小生命周期：

1. `tree_begin()`
2. `tree_create_branch(parent)`
3. 在不同 branch 上执行 `tree_read()` 和 `tree_write()`
4. `tree_select_winner(branch_id)`
5. 可选：对 winner 执行 `tree_reserve(branch_id, row, delta)`
6. `tree_commit()` 或 `tree_abort()`

当前 Tree-DB 约束是两层结构：

- root branch id 固定为 `0`
- candidate branch 都直接挂在 root 下
- winner 必须是某个 candidate branch
- root 上不允许写入

## 3. 读集分类

当前实现把 Tree 读分为四类：

- `EXPLORATORY_READ`：探索性读取，默认非严格读。
- `RANK_READ`：排序读取，例如价格、距离、评分等，只用于分支排名。
- `FEASIBILITY_READ`：可行性读取，例如库存是否足够、容量是否足够。
- `STRICT_READ`：严格读取，必须参与最终提交验证。

读集分类的工程动机是减少不必要冲突：

- 非 winner 分支的读集不应该影响最终提交。
- winner 分支里不是决定最终正确性的上下文读，也不一定要参与 OCC 验证。
- 只有会影响最终写入正确性的关键状态才应该成为严格读。

迁移到原生 DBx1000 时，TPC-C New-Order 推荐如下划分：

- `STRICT_READ`：
  - `district.d_next_o_id`，因为提交要写回 `d_next_o_id + 1`。
  - 实际被写入的 `stock.s_quantity`，如果不用 reserve 而是普通读改写。
  - 插入新订单时的唯一性边界检查。
- `FEASIBILITY_READ`：
  - 候选仓库的 `stock.s_quantity`，用于判断该候选是否能满足订单。
  - 如果候选最终成为 winner，并且该库存要参与普通读改写，可以升级为 strict。
- `RANK_READ`：
  - 候选仓库距离、优先级、配送成本等扩展字段。
  - 原生 TPC-C 没有这些字段，可以先不实现。
- `EXPLORATORY_READ`：
  - 只用于构造候选上下文、不会影响最终提交正确性的读。

注意：这些分类不是数据库“自动知道”的语义，而是 benchmark/执行器在调用 Tree API 时显式标注。数据库内核只负责按 read kind 记录依赖和提交验证，不负责理解“库存”“评分”等业务含义。

## 4. 资源预占用语义

当前资源预占用不是传统 2PL 里的“提前申请锁”。它更接近资源 delta ledger：

1. 业务执行器明确告诉数据库：我要对某个资源行做 `delta`，例如库存 `-1`。
2. 数据库在全局 reservation 表中记录该 row 的 pending delta。
3. 预占用阶段检查 `committed_value + pending_delta + new_delta >= 0`。
4. 检查通过后，该 reservation 变成 ACTIVE。
5. 事务提交时，把 ACTIVE reservation 汇总为真实写入，并把 reservation 标记为 CONFIRMED。
6. 事务 abort/断连/超时时，把 ACTIVE reservation 从 pending delta 中释放，并标记为 RELEASED/EXPIRED。

关键点：

- 普通 `GET` 或普通 TPC-C 读不会自动看到 pending reservation delta。
- `tree_reserve()` 会在资源检查时看到 pending delta。
- 当前 Tree-DB 的 reserve 通过全局 `g_tree_reserved_delta[row_t*]` 实现 pending delta。
- 提交时再把 reservation delta 写入真实 row value。
- abort 时只释放 pending delta，不写真实 row。

这种设计把语义边界放在 API 上：数据库不知道 “stock” 的业务含义，但知道调用者要求对某个数值行应用一个有非负约束的 delta。

## 5. 当前 Tree-DB 实现结构

### 5.1 `tree_txn.h`

需要迁移的数据结构：

- `TreeBranchStatus`
- `TreeReadKind`
- `TreeOccMode`
- `NormalReadDep`
- `BoundaryReadDep`
- `StagedWrite`
- `BranchState`
- `TreeKeyLookupCacheEntry`
- `TreeCommitPlan`
- `TreeConflictInfo`
- `TreeReservation`
- `TreeTxnState`

其中最重要的是：

- `BranchState::staged_writes`：每个分支自己的写缓冲。
- `BranchState::normal_reads`：已存在行的读依赖，包含 read wts。
- `BranchState::boundary_reads`：读到“不存在”的边界依赖，用于 insert-absent 验证。
- `TreeTxnState::winner_branch_id`：最终 winner。
- `TreeTxnState::reservations`：本事务持有的预占用记录。

### 5.2 `tree_txn.cpp`

需要迁移的逻辑：

- `get_branch_state()`
- `find_visible_write()`
- `build_tree_commit_plan()`

`build_tree_commit_plan()` 的作用是只合并 root 到 winner 路径上的读写集。当前实现只支持 root + one child，所以合并逻辑很简单。迁移时不要把所有分支的读集都拿去验证，否则 Tree 模型的核心收益会消失。

### 5.3 `txn.cpp`

当前 Tree-DB 在 `txn_man` 上增加了以下方法：

- `tree_begin()`
- `tree_create_branch()`
- `tree_read()`
- `tree_write()`
- `tree_set_occ_mode()`
- `tree_select_winner()`
- `tree_reserve()`
- `tree_commit()`
- `tree_commit_retryable()`
- `tree_last_conflict_reason()`
- `tree_abort()`

辅助函数集中在 `txn.cpp` 匿名 namespace，包括：

- `load_committed_row()`
- `load_tree_committed_row_cached()`
- `make_row_copy()`
- `parse_row_int_value()`
- `reserve_tree_record()`
- `release_tree_reservations()`
- `prepare_tree_reservation_updates()`
- `confirm_tree_reservations()`

迁移时应先实现 Tree 事务最小闭环，再实现 reserve。

## 6. 原生 DBx1000 改造路线

### 阶段 A：只实现 Tree 事务，不做 reserve

目标：在 DBx1000 的 TPC-C New-Order 上支持多候选分支，但最终只提交 winner。

建议步骤：

1. 把 Tree 状态文件引入原生 DBx1000：
  - 新增 `system/tree_txn.h`
  - 新增 `system/tree_txn.cpp`
  - 更新 `Makefile` 或 `CMakeLists.txt`
2. 修改 `system/txn.h`：
  - include `tree_txn.h`
  - 给 `txn_man` 增加 `TreeTxnState *tree_state`
  - 增加 `std::string last_tree_conflict_summary`
  - 声明 Tree 方法
3. 修改 `system/txn.cpp`：
  - `txn_man::init()` 中初始化 `tree_state = nullptr`
  - `txn_man::release()` 中调用 `reset_tree_state()`
  - 实现 `tree_begin()`、`tree_create_branch()`、`tree_read()`、`tree_write()`、`tree_select_winner()`、`tree_commit()`、`tree_abort()`
4. 不要改普通事务路径：
  - `run_txn()` 不变
  - `get_row()` 不变
  - `finish()` 不变
  - 原始 TPC-C/YCSB baseline 必须还能正常跑
5. 在 TPC-C 里新增一种事务类型或新增测试开关：
  - 保留原 `TPCC_NEW_ORDER`
  - 新增 `TPCC_AGENT_NEW_ORDER` 或用配置开关选择 Tree New-Order

阶段 A 的提交语义：

- Tree 分支读写只暂存在 `TreeTxnState` 中。
- `tree_select_winner()` 后丢弃 loser 分支。
- `tree_commit()` 构造 winner commit plan。
- 对 strict read 的 row，按 row pointer 排序后加 latch。
- 验证 `current_wts == read_wts`。
- 对 insert-absent 写，锁住 index bucket 并检查 key 仍不存在。
- 验证通过后写入 winner 的 staged writes。

### 阶段 B：加入资源预占用

目标：把库存/座位/配额这类资源的最终扣减提前到 winner 阶段做资源约束检查。

建议步骤：

1. 在 `tree_txn.h` 加入 `TreeReservation` 与 `TreeTxnState::reservations`。
2. 在 `txn.cpp` 加入全局 reservation 表：
  - `std::mutex g_tree_reservation_mutex`
  - `uint64_t g_tree_next_reservation_id`
  - `std::unordered_map<row_t *, int64_t> g_tree_reserved_delta`
  - `std::unordered_map<uint64_t, GlobalTreeReservation> g_tree_reservation_records`
3. 实现：
  - `reserve_tree_record()`
  - `release_tree_reservations()`
  - `prepare_tree_reservation_updates()`
  - `confirm_tree_reservations()`
  - `gc_tree_reservation_records_locked()`
4. 在 `tree_commit()` 中：
  - 把 reservation row 加入 validation/latch 集合。
  - `prepare_tree_reservation_updates()` 生成真实 row update。
  - 写入成功后 `confirm_tree_reservations()`。
  - 如果 abort，则 `reset_tree_state()` 释放 reservation。
5. 在 `tree_abort()`、`release()` 和异常路径里确保 reservation 会释放。

阶段 B 的关键正确性要求：

- reserve 只允许在 winner branch 上执行。
- reserve 检查必须看到该 row 的 pending delta。
- reserve 不应该直接写 committed row，否则 abort 无法恢复。
- commit 时必须把 reservation 变成真实写入。
- abort/断连/超时时必须释放 pending delta。

### 阶段 C：Agent-NewOrder 多候选版本

目标：把 TPC-C New-Order 改造成多候选仓库版本。

推荐先实现一个独立事务类型，不直接覆盖原始 `run_new_order()`：

- 原始 baseline：`TPCC_NEW_ORDER`
- Tree 单候选 + reserve：`TPCC_NEW_ORDER_RESERVE`
- Agent 多候选 + reserve：`TPCC_AGENT_NEW_ORDER`

执行流程：

1. 生成一个 New-Order query：
  - `w_id`, `d_id`, `c_id`
  - 5 到 15 条 order line
  - item id 用 DBx1000 原有 `NURand(8191, 1, g_max_items)`
  - quantity 用 1 到 10
  - 去重重复 item
2. 为每个任务生成候选 supply warehouse：
  - baseline 只用原始 `supply_w_id`
  - agent 版本生成 `candidate_w_ids`
3. `tree_begin()`
4. 为每个候选仓库创建 branch。
5. 每个 branch 用 `FEASIBILITY_READ` 读取候选 stock。
6. 选择第一个可行候选，或按自定义 rank 选择最好候选。
7. `tree_select_winner(winner_branch)`
8. 对 winner：
  - strict/feasibility read `district.d_next_o_id`
  - 写 `district.d_next_o_id + 1`
  - 对每个 order line 的 stock 做 `tree_reserve(..., -quantity)`
  - 写 order/new_order/order_line 相关行
9. `tree_commit()`

## 7. 和原生 DBx1000 TPC-C 的关系

原生 DBx1000 已经有 TPC-C New-Order：

- query 生成：`benchmarks/tpcc_query.cpp`
- 执行逻辑：`benchmarks/tpcc_txn.cpp::run_new_order()`

迁移时应尽量复用这些逻辑：

- 仓库、地区、客户、item、stock 的 key 生成继续用 `tpcc_helper.h`。
- New-Order 的 item 数、order line 数、remote item 规则继续跟 `tpcc_query::gen_new_order()` 对齐。
- stock 更新公式先保留 DBx1000/TPC-C 原逻辑。

但要注意：标准 TPC-C 的 `s_quantity` 更新不是简单“库存不能小于 0”。DBx1000 原始实现通常会按 TPC-C 规则：

```text
if s_quantity > ol_quantity + 10:
    s_quantity = s_quantity - ol_quantity
else:
    s_quantity = s_quantity - ol_quantity + 91
```

如果研究“资源耗尽/预占用成功率”，需要一个 bounded-resource profile，把 stock 改成非负约束：

```text
next = s_quantity - ol_quantity
if next < 0:
    resource abort
```

建议实验同时保留两组：

- `dbx1000_tpcc_profile`：尽量贴近 DBx1000 原 TPC-C。
- `bounded_resource_profile`：用于验证资源预占用机制。

## 8. 迁移到原生 DBx1000

### 内嵌到 DBx1000 benchmark

优点：

- 不需要 TCP/KV server。
- 能直接复用 DBx1000 原有线程模型、统计和并发控制算法。
- 更适合论文实验。

缺点：

- 需要改 `tpcc_query` 和 `tpcc_txn`。
- 需要处理 DBx1000 多种 `CC_ALG` 的编译宏。

## 9. 需要从 Tree-DB 迁移的底层能力

Tree-DB 的实现依赖一些原生 DBx1000 可能没有的 helper，需要补齐或改写。

### 9.1 Row manager 能力

Tree-DB 在 `Row_occ` 上增加了：

- `read_committed(row_t *data, uint64_t *wts_out, uint64_t *rts_out)`
- `latch()`
- `try_latch()`
- `current_wts()`
- `current_rts()`
- `extend_rts(ts)`
- `write(row_t *data, uint64_t ts)`

原生 DBx1000 的 `row_occ`/`row_silo`/`row_tictoc` 是按 `CC_ALG` 拆分的。迁移时有两种方式：

1. 最小方案：先只支持 `CC_ALG=OCC`，在 `row_occ` 加上述方法。
2. 完整方案：抽象一个 `row_manager_common` 接口，让 Tree 事务可以跨 OCC/Silo/TicToc/MVCC 使用。

建议先做最小方案，把 Tree 机制跑通，再考虑多 CC_ALG。

### 9.2 Index bucket latch

Tree-DB 的 commit 支持 insert-absent 边界验证，需要：

- 定位 hash bucket
- latch bucket
- 检查 bucket 是否存在 exact key
- 在已 latch bucket 内插入 item

原生 DBx1000 的 hash index 不一定暴露这些接口。迁移时需要在 `storage/index_hash.*` 或原生 `storage/index_hash.*` 中增加：

- `locate_bucket(key, part_id)`
- `latch_bucket(bucket)`
- `unlatch_bucket(bucket)`
- `bucket_contains_exact_key(bucket, key, table_name)`
- `bucket->insert_item(...)` 或等价的 no-reenter insert

如果第一阶段只更新已存在行，不插入新 row，可以暂时跳过 insert-absent 逻辑。但 New-Order 最终需要写 order/new_order/order_line，最好还是补齐。

### 9.3 行复制和写入

需要可靠地创建临时 row copy：

- `make_row_copy(orig_row, value)`
- 对 TPC-C 多列 row，不能只拷贝单一 value 字符串。

Tree-DB 当前 KV 模型只有两列：key/value。迁移到原生 TPC-C 时，不能照搬 KV 的 `make_row_copy()`。应按 TPC-C schema 复制整行，然后修改特定字段：

- stock row 修改 `S_QUANTITY`
- district row 修改 `D_NEXT_O_ID`
- order/new_order/order_line 插入完整 row

这是迁移中最容易出错的点。

## 10. 正确性边界

### 10.1 ACID 怎么保证

Tree 事务最终提交的是一个普通事务等价的 winner plan。只要满足：

- loser 分支没有真实写入。
- winner 的 strict read 和 write base version 都经过验证。
- 所有真实写入在持有 row latch/bucket latch 后完成。
- reserve 的真实写入只在 commit 阶段发生。
- abort 释放 pending reservation。

那么 Tree 提交可以看作一个原子提交的普通事务。

### 10.2 普通事务看不到 pending reservation 是否破坏隔离性

如果把 reservation 看成 committed data 的一部分，那么普通读看不到 pending delta 会像违反隔离。

但当前设计不是这样定义的。pending reservation 是并发控制元数据，不是 committed tuple version。普通事务读取 committed version；reserve API 在资源约束检查时额外考虑 pending delta。也就是说：

- 普通事务：只看 committed data。
- Tree reserve 事务：资源检查看 committed data + pending reservation delta。
- commit 后：delta 才进入 committed data。

这类似“资源准入控制”或“语义化 delta 预留”，不是传统锁，也不是普通 MVCC version。

### 10.3 必须避免的错误实现

不要在 `tree_reserve()` 里直接修改 committed row。如果这样做：

- abort 时需要补偿写，容易和其他事务交错。
- 普通 GET 会看到未提交扣减。
- 事务隔离语义会变得不清晰。

正确做法是：

- `tree_reserve()` 只写 pending reservation 表。
- `tree_commit()` 才把 aggregated delta 写入 row。
- `tree_abort()` 释放 pending reservation。

## 11. 建议测试计划

### 11.1 单元测试

1. 单事务 Tree：
  - 创建两个 branch。
  - 两个 branch 读不同 row。
  - 选择 branch 1。
  - 只提交 branch 1 写入。
2. loser 不影响提交：
  - loser 读一个高冲突 row。
  - 并发事务修改该 row。
  - winner 不读该 row。
  - Tree commit 应成功。
3. strict read 冲突：
  - winner strict read row A。
  - 并发事务修改 A。
  - Tree commit 应 abort。
4. reserve 成功：
  - stock=10。
  - Tree reserve -3。
  - commit 后 stock=7。
5. reserve abort 释放：
  - stock=10。
  - Tree reserve -8。
  - abort。
  - 另一个 Tree reserve -8 应成功。
6. reserve 防超卖：
  - stock=10。
  - T1 reserve -8。
  - T2 reserve -3。
  - T2 应 resource abort。

### 11.2 TPC-C smoke test

建议先跑小规模：

```bash
./rundb -t4 -n1000 -w1 -d1
```

具体参数以当前原生 DBx1000 的 `test.py` 或 README 为准。

对比模式：

- Original New-Order
- Tree New-Order，无 reserve
- Tree New-Order + reserve
- Agent New-Order + reserve

统计指标：

- committed txn/task
- abort count
- resource abort count
- throughput
- p50/p95 latency
- stock invariant violation count

### 11.3 公平性检查

必须区分三类失败：

- CC abort：由于版本变化、锁冲突、验证失败。
- resource abort：库存/座位/配额不足。
- planned loser：多候选模型中主动丢弃的 loser，不应该算作并发控制 abort。

过去实验里曾经出现过“Tree + reserve 成功率 100%”的质疑，本质原因通常是资源不够紧、统计口径不公平或 resource abort 没有纳入 task success/failure。因此后续 DBx1000 实验必须输出三类失败的分项统计。

## 12. 推荐开发顺序

推荐按下面顺序迁移，避免一次性改太多：

1. 在原生 DBx1000 新增 `tree_txn.h/cpp`，只编译通过。
2. 在 `txn_man` 上挂 `TreeTxnState *tree_state`，实现 begin/abort/release。
3. 实现 branch create、read、write、winner select。
4. 只支持更新已存在行，先不支持 insert-absent。
5. 实现 Tree commit 的 strict read/version validation。
6. 在 TPC-C 中做一个 Tree 单候选 New-Order，只更新 district 和 stock，不插入 order line。
7. 加入完整 New-Order 写集合。
8. 加入 reserve，但先只对 stock 行做。
9. 加入 Agent 多候选供货仓库。
10. 加入完整统计：CC abort、resource abort、planned loser、throughput、latency。

## 13. 迁移时的高风险点

1. KV row copy 不能直接用于 TPC-C row。
  Tree-DB 的 KV 表只有 key/value 两列；原生 TPC-C 是多列 schema。所有 staged write 必须保存“修改哪个 row 的哪个字段”，或者保存一份完整 row copy。
2. loser 分支不能留下真实副作用。
  在 winner 前，branch write 只能进入 staged write，不能调用 `get_row(..., WR)` 后直接修改真实 row。
3. reserve 不能变成普通锁。
  如果只是提前加锁，那实验贡献会退化成 2PL 变体。reserve 的核心是 pending delta + resource constraint。
4. pending reservation 必须可恢复。
  所有 `Abort`、异常返回、txn release 都必须释放 reservation。
5. bucket latch 顺序必须稳定。
  row latch 和 bucket latch 都要排序，避免死锁。
6. 原生 DBx1000 的 CC_ALG 是编译期宏。
  不要一开始追求 Tree 跨所有 CC_ALG。先在 OCC 下跑通，再逐步扩展。
7. 统计口径必须公平。
  loser 不是 abort；resource exhausted 不是 CC abort；commit validation abort 才是 CC abort。

## 14. 面向论文实验的建议

迁移到原生 DBx1000 后，建议至少保留三类 workload：

1. 原生 TPC-C New-Order：
  - 证明没有破坏 DBx1000 原有 workload。
2. Bounded-resource New-Order：
  - stock 改为有限资源扣减。
  - 用于展示 reserve 在资源竞争下的效果。
3. Agent-NewOrder：
  - 多候选仓库。
  - 展示 Tree 模型减少候选探索冲突、减少无效候选事务的能力。

建议对比：

- OCC
- NO_WAIT/2PL
- Silo
- TicToc
- MVCC
- Tree
- Tree + reserve
- Agent-NewOrder + reserve

如果时间有限，先完成：

- OCC baseline
- 原生 New-Order
- Tree New-Order
- Tree + reserve
- Agent-NewOrder + reserve

然后再补其他并发控制算法。

## 15. 给后续 Agent 的最小任务定义

第一轮迁移不要追求完整论文系统。最小可交付目标：

1. 原生 DBx1000 能编译。
2. 原始 TPC-C New-Order 能照常运行。
3. 新增 Tree New-Order 单候选模式，能提交并保持 stock/district 正确。
4. 新增 stock reserve，能防止 bounded stock 超卖。
5. 新增 Agent-NewOrder 多候选模式，能选择一个可行候选并只提交该候选。
6. 输出 CSV：
  - mode
  - total
  - success
  - success_rate
  - throughput
  - avg/p50/p95 latency
  - cc_abort
  - resource_abort
  - invariant_ok

完成这 6 点后，再进入性能优化和多 CC_ALG 对比。