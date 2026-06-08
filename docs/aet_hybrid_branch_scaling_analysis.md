# AET Hybrid Branch Scaling Experiment Analysis

## Experiment Setup

This experiment compares the original multi-candidate transaction model with the AET winner-only transaction model.

Common configuration:

- Workload: TPC-C Agent NewOrder
- Warehouses: 8
- Threads: 32
- Branches: 1, 4, 8
- Stop condition: `MAX_TXN=10000`, `STOP_ON_ATTEMPTS=1`
- Output summary: `output/final_branch_scaling_report/branch_scaling_summary.csv`

Compared systems:

- `OCC`, `SILO`, `TICTOC`: traditional baseline. Each candidate branch is executed as an independent transaction. Non-winner candidates appear as aborts.
- `AET_HYBRID_RULE`: AET multi-branch model with semantic-aware intent handling and OCC-style final certification.
- `AET_HYBRID_SILO`: AET multi-branch model with the same semantic-aware intent layer, but using SILO-style final certification.

Correctness smoke tests passed before performance runs:

- `AET_HYBRID_RULE`: CAS winner-only test passed.
- `AET_HYBRID_RULE`: XWRITE winner-only test passed.
- `AET_HYBRID_SILO`: CAS winner-only test passed.
- `AET_HYBRID_SILO`: XWRITE winner-only test passed.

## Mechanism Summary

The AET model executes one logical Agent transaction with multiple internal branches.

The branch manager collects branch-local intents:

- `DELTA`: stock decrement, represented as semantic delta.
- `CAS`: conditional update, currently used for `DISTRICT.D_NEXT_O_ID`.
- `XWRITE`: winner-only overwrite.
- `READ`: branch read dependency metadata.

Only the selected winner branch is materialized into the database write set. Loser branches are released inside the logical transaction and are not counted as external database transaction aborts.

`AET_HYBRID_RULE` and `AET_HYBRID_SILO` share the same upper-level semantic intent layer. Their difference is the final certification layer:

- `AET_HYBRID_RULE`: OCC-style final validation.
- `AET_HYBRID_SILO`: SILO-style final validation.

## Main Results

| System | Branches | Success Rate | Throughput | Avg Latency |
|---|---:|---:|---:|---:|
| OCC baseline | 1 | 0.749884 | 14242.99 | 0.000070 |
| OCC baseline | 4 | 0.226374 | 5416.39 | 0.000185 |
| OCC baseline | 8 | 0.118582 | 2472.46 | 0.000404 |
| SILO baseline | 1 | 0.797531 | 42474.57 | 0.000024 |
| SILO baseline | 4 | 0.233792 | 20152.28 | 0.000050 |
| SILO baseline | 8 | 0.120978 | 11708.89 | 0.000085 |
| TICTOC baseline | 1 | 0.799111 | 55347.45 | 0.000018 |
| TICTOC baseline | 4 | 0.233782 | 19569.26 | 0.000051 |
| TICTOC baseline | 8 | 0.120746 | 12047.44 | 0.000083 |
| AET_HYBRID_RULE | 1 | 0.742422 | 15195.06 | 0.000066 |
| AET_HYBRID_RULE | 4 | 0.740265 | 14385.78 | 0.000070 |
| AET_HYBRID_RULE | 8 | 0.739781 | 14804.00 | 0.000068 |
| AET_HYBRID_SILO | 1 | 0.769029 | 26099.79 | 0.000038 |
| AET_HYBRID_SILO | 4 | 0.768243 | 31369.37 | 0.000032 |
| AET_HYBRID_SILO | 8 | 0.768560 | 25955.71 | 0.000039 |

## Interpretation

### 1. Traditional baseline suffers from branch-count amplification

In the traditional baseline, each candidate is executed as a separate transaction. As branch count increases, loser candidates are counted as aborts.

For example:

- OCC success rate drops from `0.749884` at 1 branch to `0.118582` at 8 branches.
- SILO success rate drops from `0.797531` at 1 branch to `0.120978` at 8 branches.
- TICTOC success rate drops from `0.799111` at 1 branch to `0.120746` at 8 branches.

This matches the expected loser-abort amplification effect of the traditional transaction model.

### 2. AET keeps success rate stable as branch count grows

AET does not expose internal loser branches as database transaction aborts. Its success rate is therefore stable across branch counts:

- `AET_HYBRID_RULE`: about `0.74` for 1, 4, and 8 branches.
- `AET_HYBRID_SILO`: about `0.768` for 1, 4, and 8 branches.

This is the key semantic advantage of the AET model.

### 3. OCC-backed Hybrid fixes branch amplification but is still limited by OCC certification

`AET_HYBRID_RULE` significantly improves over traditional OCC at larger branch counts:

- b4: `14385.78 / 5416.39 = 2.66x` vs OCC b4.
- b8: `14804.00 / 2472.46 = 5.99x` vs OCC b8.

However, it does not consistently outperform traditional SILO/TICTOC at b4:

- b4: `0.71x` vs SILO b4.
- b4: `0.74x` vs TICTOC b4.

This shows that winner-only semantics alone is not enough when the final certification layer is still OCC-style.

### 4. Silo-backed Hybrid is the strongest configuration

`AET_HYBRID_SILO` combines:

- AET winner-only branch semantics.
- Semantic intent handling for DELTA/CAS/XWRITE.
- SILO-style final certification.

It outperforms traditional SILO and TICTOC at multi-branch settings:

- b4 vs SILO b4: `1.56x`
- b4 vs TICTOC b4: `1.60x`
- b8 vs SILO b8: `2.22x`
- b8 vs TICTOC b8: `2.15x`

This result supports the design claim that the AET model should be paired with a strong final certification layer. The semantic branch model removes loser-abort amplification, while SILO-style validation reduces commit-time overhead.

### 5. One-branch AET still has model overhead

At 1 branch, `AET_HYBRID_SILO` is slower than traditional SILO and TICTOC:

- `AET_HYBRID_SILO b1`: `26099.79`
- `SILO b1`: `42474.57`
- `TICTOC b1`: `55347.45`

This is expected because AET still pays branch manager and intent-layer overhead even when there is only one branch. The advantage appears when branch count grows and the traditional model starts paying loser-abort amplification.

## Conclusion

The experiment supports three conclusions:

1. The traditional transaction model is not well suited for Agent multi-candidate decision tasks because its success rate decreases sharply as branch count grows.
2. The AET winner-only model keeps success rate stable across branch counts by treating loser branches as internal exploration paths instead of external transaction aborts.
3. `AET_HYBRID_SILO` is currently the best implementation because it combines semantic-aware multi-branch execution with a stronger final certification layer.

The recommended main comparison for reports is:

- Traditional `SILO` and `TICTOC` at b1/b4/b8.
- `AET_HYBRID_RULE` at b1/b4/b8 to show the effect of AET semantics with OCC-style validation.
- `AET_HYBRID_SILO` at b1/b4/b8 to show the final optimized design.

## Remaining Work

Suggested next steps:

1. Add a CAS/XWRITE-heavy Agent task workload. TPC-C NewOrder mainly stresses DELTA and branch semantics, but does not fully demonstrate CAS/XWRITE-heavy Hybrid CC.
2. Add stronger correctness tests for concurrent CAS conflicts and bounded DELTA release.
3. Add root read sharing for branch-independent reads such as warehouse/customer/item reads.
4. Add planned-loser release cost counters so internal branch management overhead can be separated from external database aborts.
