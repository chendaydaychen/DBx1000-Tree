# AET Hybrid CC 汇报总结：多分支事务与语义感知并发控制

## 1. 核心目标

本阶段目标是把 DBx1000 中的 AET/Tree 事务从“语义层 + SILO/OCC 包装”推进到一种可以和 OCC、SILO、TicToc、MVCC、2PL 类算法并列比较的语义感知混合并发控制方案。

最终形成的方案是 `AET_HYBRID_CC`：

- 上层事务模型支持 Agent 多候选分支，所有候选分支在同一个事务内部隔离，只有 winner 分支进入提交。
- 底层并发控制不再调用 SILO validate，而是使用独立的 `Row_aet_hybrid` row manager 和 `validate_aet_hybrid_cc()` 提交协议。
- 不同语义采用不同控制策略：普通读写保守认证，纯增量提交期合并，TPC-C 库存更新提交期按最新值重算。

## 2. 机制创新点

### 2.1 多分支事务模型

传统 Agent 多候选执行可以理解为多个事务候选分别执行，最后只有一个提交，其余候选 abort。这样在 4/8 分支时会引入大量外部 abort。

AET/Tree 模型把这些候选放入一个事务内部：

- 每个候选分支有私有 intent/read/write 集。
- loser 分支只在事务内部释放，不作为外部事务 abort 放大系统冲突。
- winner 分支被选择后才进入提交路径。

实验中 `planned_loser_abort_cnt` 统计的是内部 loser 分支释放，不等价于传统 CC 的外部事务 abort。

### 2.2 语义感知混合 CC

当前支持的主要语义包括：

- `READ`：记录语义读版本，必要时验证。
- `DELTA`：数值增量，适合计数器、分配器等可交换更新。
- `TPCC_STOCK_UPDATE`：TPC-C NewOrder 中 `S_QUANTITY` 的专用库存回绕语义。
- `CAS/XWRITE`：接口存在，用于比较交换和独占覆盖写。

本阶段最关键的优化是把 TPC-C NewOrder 的库存更新从“读旧值后写本地副本”改为“提交阶段基于最新列值计算”：

```text
if latest_s_quantity > ol_quantity + 10:
    latest_s_quantity -= ol_quantity
else:
    latest_s_quantity = latest_s_quantity - ol_quantity + 91
```

这个语义保证了 TPC-C 的库存回绕规则，同时避免因为旧版本读导致事务 abort。

### 2.3 和 AET_HYBRID_SILO 的区别

`AET_HYBRID_SILO` 是语义 intent 层加 SILO 最终认证。

`AET_HYBRID_CC` 是独立并发控制算法：

- 使用 `Row_aet_hybrid` 作为 row manager。
- 使用独立提交协议 `validate_aet_hybrid_cc()`。
- 语义 delta 和 stock update 在提交阶段直接执行，不再转成普通 SILO 写集。

因此 `AET_HYBRID_CC` 更符合论文中“和 SILO/MVCC/2PL 并列的语义感知混合并发控制”的定位。

## 3. 实验设计

### 3.1 Native TPC-C NewOrder

目的：测试单分支、原生 NewOrder 负载下，纯语义感知 CC 的开销和收益。

对比对象：

- 传统 CC：OCC、NO_WAIT、WAIT_DIE、DL_DETECT、MVCC、HEKATON、TICTOC、SILO
- 语义包装：`AET_HYBRID_SILO`
- 新算法：`AET_HYBRID_CC`

命令：

```bash
CASES=tpcc_new_order_all_cc,tpcc_aet_hybrid_silo_new_order_semantic,tpcc_aet_hybrid_cc_new_order_semantic \
CC_ALGS=OCC,NO_WAIT,WAIT_DIE,DL_DETECT,MVCC,HEKATON,TICTOC,SILO \
MAX_TXN=10000 STOP_ON_ATTEMPTS=1 \
WAREHOUSES=8 THREADS=32 \
OUT_DIR=output/native_tpcc_new_order_aet_hybrid_cc_stock_update_compare \
scripts/run_tpcc_compare.sh
```

### 3.2 Agent 多分支 NewOrder

目的：测试 Agent 多候选负载下，传统事务模型和 AET/Tree 多分支事务模型的差异。

对比对象：

- 传统事务 + OCC/SILO/TICTOC，分支数为 1/4/8
- AET + `AET_HYBRID_SILO`
- AET + `AET_HYBRID_CC`

命令：

```bash
CASES=tpcc_agent_new_order_baseline_all_cc,tpcc_agent_new_order_aet_hybrid_silo_standard_b1,tpcc_agent_new_order_aet_hybrid_silo_standard_b4,tpcc_agent_new_order_aet_hybrid_silo_standard_b8,tpcc_agent_new_order_aet_hybrid_cc_standard_b1,tpcc_agent_new_order_aet_hybrid_cc_standard_b4,tpcc_agent_new_order_aet_hybrid_cc_standard_b8 \
CC_ALGS=OCC,SILO,TICTOC \
BRANCHES=1,4,8 \
MAX_TXN=10000 STOP_ON_ATTEMPTS=1 \
WAREHOUSES=8 THREADS=32 \
OUT_DIR=output/agent_aet_hybrid_cc_stock_update_compare \
scripts/run_tpcc_compare.sh
```

## 4. 实验结果

### 4.1 Native NewOrder

| 模式 | CC | 吞吐 | 成功率 |
|---|---:|---:|---:|
| 原生 OCC | OCC | 13,348 | 0.746 |
| 原生 NO_WAIT | NO_WAIT | 35,174 | 0.722 |
| 原生 WAIT_DIE | WAIT_DIE | 34,098 | 0.776 |
| 原生 MVCC | MVCC | 23,658 | 0.987 |
| 原生 TICTOC | TICTOC | 50,413 | 0.800 |
| 原生 SILO | SILO | 53,381 | 0.800 |
| AET_HYBRID_SILO | AET+SILO | 29,636 | 0.917 |
| AET_HYBRID_CC | Semantic CC | 61,245 | 1.000 |

`AET_HYBRID_CC` 加速比：

- vs OCC：4.59x
- vs NO_WAIT：1.74x
- vs WAIT_DIE：1.80x
- vs MVCC：2.59x
- vs TICTOC：1.21x
- vs SILO：1.15x
- vs AET_HYBRID_SILO：2.07x

这说明在加入 `TPCC_STOCK_UPDATE` 后，语义感知 CC 不只在 Agent 多分支场景有效，在单分支 NewOrder 上也超过了 SILO/TicToc。

### 4.2 Agent 多分支 NewOrder

| 分支 | 传统 OCC | 传统 SILO | 传统 TICTOC | AET_HYBRID_SILO | AET_HYBRID_CC |
|---:|---:|---:|---:|---:|---:|
| 1 | 13,627 | 56,817 | 48,874 | 51,757 | 66,013 |
| 4 | 5,225 | 20,253 | 18,982 | 27,230 | 43,240 |
| 8 | 2,632 | 12,208 | 11,897 | 18,579 | 35,365 |

`AET_HYBRID_CC` 相对传统 SILO：

- b1：1.16x
- b4：2.14x
- b8：2.90x

`AET_HYBRID_CC` 相对传统 TICTOC：

- b1：1.35x
- b4：2.28x
- b8：2.97x

`AET_HYBRID_CC` 相对传统 OCC：

- b1：4.84x
- b4：8.27x
- b8：13.44x

`AET_HYBRID_CC` 相对 `AET_HYBRID_SILO`：

- b1：1.28x
- b4：1.59x
- b8：1.90x

结果显示：分支数越大，传统事务模型越容易被 loser 候选的外部 abort 放大拖垮，而 AET 多分支模型只提交 winner，语义 CC 又把库存和计数器类更新转化为提交期语义操作，因此吞吐优势随分支数扩大。

## 5. 结果解释

### 5.1 为什么这次提升明显

之前版本中，`S_QUANTITY` 更新仍然依赖旧值：

1. 先读取旧库存。
2. 根据旧库存计算新库存。
3. 把结果写入本地 row copy。
4. 提交时做版本认证。

这会造成两个问题：

- 并发更新同一 `STOCK` row 时容易因为版本变化 abort。
- 即使不同语义列可以并发，整行写集认证仍会制造冲突。

新版本把库存更新建模为专用语义：

- 不再提前读旧值决定最终写入值。
- 提交阶段锁定 row/column 后基于最新值计算。
- `S_YTD/S_ORDER_CNT/S_REMOTE_CNT` 作为 commit-time delta 合并。

因此 abort 降为 0，成功率达到 1.0。

### 5.2 为什么 Agent b4/b8 提升更大

传统模型中，4/8 个候选分支意味着更多独立事务尝试，loser 候选也会消耗完整事务执行和 abort 成本。

AET 模型中，loser 分支只在同一个事务内部释放，最终只有 winner 分支进入提交。分支越多，传统模型的无效 abort 越多，因此 AET 的相对收益越大。

## 6. 当前局限

1. `TPCC_STOCK_UPDATE` 是针对 TPC-C NewOrder 人工标注的语义，数据库目前不是自动从 SQL 推断语义。
2. 当前 DBx1000 的 NewOrder 中 `ORDER/NEW_ORDER/ORDER_LINE` 插入路径仍是注释状态，所以 insert/delete 语义尚未完整评估。
3. 结果证明了语义感知 CC 在该 workload 上有效，但还需要更多 workload 展示 `CAS/XWRITE/PRED_READ` 等语义。

## 7. 汇报结论

本阶段工作完成了从“语义包装层”到“独立语义感知混合 CC”的关键跨越。

最终结论：

- `AET_HYBRID_CC` 在 native NewOrder 中超过 SILO，说明语义 CC 本身不是纯额外开销。
- `AET_HYBRID_CC` 在 Agent 多分支负载中显著超过传统 OCC/SILO/TICTOC，且分支越多优势越大。
- 专用语义 `TPCC_STOCK_UPDATE` 是核心突破：它把依赖旧值的库存回绕更新改成提交期基于最新值的语义操作，既保持 TPC-C 正确性，又减少 abort。

这组结果可以作为论文实验中的主结果表。
