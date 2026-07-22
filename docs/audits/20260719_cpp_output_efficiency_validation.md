<!--
建立時間: 2026-07-19 23:05 +08:00
目標: 驗證目前 LongLineage C++ 的輸出一致性、元件效能與 production claim 邊界
處理範圍: clean-commit Release build、P2 synthetic deterministic matrix、BGZF writer A/B replay、
          executable fail-closed gates；不包含尚不可執行的 P3-P5 或 P7 七資料集全量
關聯檔案:
  - LongLineage/state/benchmarks/20260719-bgzf-row-payload-replay-local.json
  - LongLineage/state/audits/20260719-cpp-output-efficiency-validation-001.json
  - LongLineage/state/tasks/archive/20260719-cpp-output-efficiency-validation.json
-->

# C++ 輸出一致性與效率驗證

Task type: **B — Comprehensive validation**

Goals: **LL-G1、LL-G4、LL-G5**
Verdict: **`PARTIAL_PASS_PRODUCTION_NO_GO`**

## 1. 先講結論

目前可以確認：

1. 已實作的 P2 synthetic C++ path 在 workers `1/2/4/24/40`、block
   `max_sites=1/2`及`40 compute + 4 BGZF writer`共11組replay中，全部產生
   80筆同序logical rows；frozen semantic SHA-256固定為
   `9179c42faf14000c9b1c87386a09cd33cae4bce27078d7d5af2798269c4fead0`。
2. Fresh source-level writer A/B replay以相同500,000-row workload跑8+8次，
   median wall由`0.8436555 s`降至`0.7651245 s`，本機component reduction為
   `9.308420321%`。16/16 runs的row count、logical bytes、physical bytes、
   semantic SHA與本機physical SHA完全相同。
3. 先前獨立receipt的`7.930402067%`算術與source binding仍PASS；兩輪只能支持
   「此BGZF row-payload元件在此主機觀察到約8–10%改善」，不可固定宣稱某一比例。
4. Fresh Release build與完整foundation gate通過；CI/本機程式能正確阻擋未完成
   production run與偽validator成功。

目前不能確認：

1. 新C++已重建7 datasets、469,849 sites的M1/M2/co-occurrence/topology全量輸出。
2. 新C++科學結果已與frozen Python/歷史authority零差異。
3. LongLineage端到端、24-worker或40-worker production run更快。
4. P2、P3、P4、P5、P6或P7已因本輪synthetic/component結果升為`VERIFIED`。

原因不是測試失敗，而是production kernels與獨立validator尚未完整存在。P3、P4、
P5、P7在machine phase ledger中均為`BLOCKED`且evidence count=`0`。

## 2. 固定研究格式

### 2.1 研究問題

目前新的C++是否能：

1. 對相同輸入，在不同worker/block/writer組態產出一致logical data？
2. 在不改semantic output的前提下，較舊writer實作更有效率？
3. 已完整達到七資料集production parity與end-to-end效率主張？

### 2.2 假設與判定

| ID | 假設 | 成功條件 | 結果 |
|---|---|---|---|
| H1 | P2 ordered concurrency不改logical output | 11組replay row count=80且semantic SHA完全一致 | **PASS / synthetic only** |
| H2 | shared row payload減少writer重複配置成本 | 8+8 A/B median下降，logical/physical bytes與semantic SHA不變 | **PASS / component only** |
| H3 | production scientific pipeline已可做全量一致性與效能驗證 | P3-P6可執行、P7有7-dataset 24/40 receipts | **FAIL / capability absent** |

### 2.3 指標

本輪是truth-isolated軟體一致性與效能測試，不是variant-call benchmark。因此以下
生物truth指標均為`NOT_APPLICABLE`，不可填0冒充已測：

| truth_total | calls_total | TP | FP | FN | precision | recall | F1 |
|---|---|---|---|---|---|---|---|
| N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |

實際主指標為：

- ordered logical row count；
- schema-bound semantic SHA-256；
- workers、block size、writer thread matrix；
- 8+8 trial medians與relative wall reduction；
- logical/physical bytes守恆；
- test/gate exit code；
- machine phase status與evidence count。

## 3. 輸入與source binding

| 角色 | 路徑／版本 | 驗證 |
|---|---|---|
| Current source | `LongLineage` commit `9dec6b13150bd38c25d737add945e032707408bd` | build前production source clean |
| Determinism test | `LongLineage/tests/unit/test_block_pipeline.cpp` | fresh Release binary重播 |
| Benchmark baseline | commit `1d986806f4fe1d4c8617bfa7fa2915f9bafd513f` | Git blob SHA replay |
| Benchmark candidate | commit `3ef1f7f18db76f8213c72b2947713a2c73781d6f` | current writer source SHA相同 |
| Historical receipt | `LongLineage/state/benchmarks/20260719-bgzf-row-payload-local.json` | checker PASS |
| Fresh receipt | `LongLineage/state/benchmarks/20260719-bgzf-row-payload-replay-local.json` | checker PASS |
| Topology/VAF handoff | `external://InterSubMod/research/20260718_solver_methyl_edge_probe/20260719_最簡演化樹與VAF排序AI交接重點_01.md` | SHA `7b412e89…feb306` PASS |

指定handoff本身的權威結論是`NO_GO_PRODUCTION`：objective certification、
family completion與ranking completion必須分開；censored ratio、incomplete
family與ordinary-float pruning不得產生production winner或speedup claim。

## 4. Step → Verify

### Step 1：fresh Release configure/build

Input：

```text
workspace://LongLineage @ 9dec6b1
```

Command：

```bash
/usr/bin/cmake -S . -B build-cpp-output-validation \
  -DCMAKE_BUILD_TYPE=Release \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_BUILD_BENCHMARKS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
  -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON \
  -DLONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS=ON
/usr/bin/cmake --build build-cpp-output-validation --parallel 4
```

Output：

```text
LongLineage/build-cpp-output-validation/
HTSlib 1.18
Jansson 2.13.1
OpenSSL 3.0.2
[100%] Built target longlineage_governance
```

Exit：configure=`0`，build=`0`。

### Step 2：deterministic output matrix

Command：

```bash
/usr/bin/ctest --test-dir build-cpp-output-validation --output-on-failure \
  -R '^(runtime_solver|determinism_contract|p2_determinism_matrix|performance_benchmark_records)$'
./build-cpp-output-validation/bin/test_block_pipeline determinism
```

Actual：

```text
4/4 tests passed
LongLineage P2 block pipeline synthetic contract: PASS
```

Output root：

```text
/tmp/longlineage_p2_fixture_197217_6992476456254/
```

矩陣：

```text
max_sites_per_block = 1, 2
workers             = 1, 2, 4, 24, 40
extra               = max_sites=2, workers=40, BGZF writers=4
files               = 11
rows per file       = 80
```

獨立從BGZF logical records重建semantic stream：

```text
9179c42faf14000c9b1c87386a09cd33cae4bce27078d7d5af2798269c4fead0
```

首尾實際rows：

```text
0    100    1
1    300    1
...
78   15700  0
79   15900  0
```

本機額外觀察：11個BGZF physical SHA均為
`63dcd5241abf6c8a8781c1eafe225f7559da5c4dc09cc54199913f6b41549620`。
正式跨平台contract仍只以semantic SHA為authority，不把本機physical equality泛化。

### Step 3：fresh BGZF writer A/B replay

完整命令保存在fresh benchmark receipt；核心為：

```bash
git clone --quiet --shared . /tmp/ll-benchmark-replay-r3-baseline
git -C /tmp/ll-benchmark-replay-r3-baseline checkout --quiet \
  1d986806f4fe1d4c8617bfa7fa2915f9bafd513f

git clone --quiet --shared . /tmp/ll-benchmark-replay-r3-candidate
git -C /tmp/ll-benchmark-replay-r3-candidate checkout --quiet \
  3ef1f7f18db76f8213c72b2947713a2c73781d6f
```

兩variant以相同Release/HTSlib flags build，500,000 rows、5 columns、4 writer
threads，依ABBA順序重複，16/16 exit `0`。

| 指標 | Baseline | Candidate |
|---|---:|---:|
| Trials | 8 | 8 |
| Median wall seconds | 0.8436555 | 0.7651245 |
| Local component reduction |  | 9.308420321% |
| Rows | 500,000 | 500,000 |
| Logical bytes | 71,888,972 | 71,888,972 |
| Physical bytes | 1,610,593 | 1,610,593 |
| Semantic SHA | `e6db09…48f06b` | `e6db09…48f06b` |
| Physical SHA | `5a01bd…36d64` | `5a01bd…36d64` |

`cmp` baseline/candidate BGZF exit=`0`。這是本機同toolchain的額外強證據，
不是所有HTSlib/platform的physical-bit contract。

### Step 4：foundation與production fail-closed

Command：

```bash
scripts/ci/check_all.sh build-cpp-output-validation
scripts/testing/test_cli_gates.sh \
  build-cpp-output-validation/bin/longlineage \
  .
```

Actual：

```text
33/33 tests passed
NO-NETWORK RESULT: PASS authority=linux-network-namespace
CHECK ALL RESULT: FOUNDATION_PASS ... (release-phase blockers remain governed)

verified_kernel_gate: PASS exit=6
{"command":"run","status":"ERROR","exit_code":6,
 "message":"release attestation is NOT_READY"}
cli_gates: PASS
```

獨立targets對不存在run root的實際行為：

| Executable | Exit | Actual |
|---|---:|---|
| `longlineage-validate` | 7 | independent replay未實作；未寫receipt |
| `longlineage-query` | 8 | 缺`run_receipt.json`，拒絕 |
| `longlineage-export-legacy` | 8 | 缺`run_receipt.json`，拒絕 |
| `longlineage-evaluate` | 8 | 缺`run_receipt.json`，拒絕 |

`/tmp/longlineage-cpp-output-never-created`檢查結果：
`NO_OUTPUT_CREATED`。

## 5. 方法學與效能判定

### 5.1 已證明適合的lossless做法

- worker-owned indexed BAM handles；
- deterministic block sequence＋bounded reorder；
- BGZF packed TSV；
- schema-defined semantic digest；
- 同一canonical row payload同時供physical write與semantic digest；
- fail-closed cancellation與release attestation。

這些做法減少重複copy、小檔案與thread-order漂移，不改：

- read eligibility；
- output logical order；
- M1/M2 RNG consumption；
- exact topology objective/family；
- truth isolation。

### 5.2 尚未證明的production加速

以下仍不存在或未完成：

- persistent BAM/VCF/sidecar/FASTA worker bundle；
- one-pass CIGAR multi-marker projection；
- process-wide physical RSS/global permit證據；
- JSONL、LLM、site-index writers；
- runtime wall/user/system、RSS/OOM、I/O ops、faults、queue wait與latency collector；
- seven-dataset 24/40-worker comparable receipts。

因此不得把9.31%或7.93%寫成pipeline speedup，也不得宣稱C++已比歷史Python
全流程快若干倍。

## 6. Benchmark紀錄治理deviation

原7.93% receipt的trial算術、baseline/current source與harness SHA都正確；
`production_claim_allowed=false`也有negative gate。但v1.0.0 schema/checker有
以下中等風險缺口：

1. 原build command含placeholder，不是完整argv。
2. 原binary已不存在，clean rebuild binary SHA與原receipt不同。
3. CI只重驗source/harness，未重建或核對binary SHA。
4. schema只驗binary SHA為64位hex；合法格式tamper仍可通過。
5. schema尚未獨立欄位綁candidate commit、physical SHA、trial order與per-trial
   output receipt。

Fresh receipt改為保留exact commands、candidate checkout、binary digest、trial
times與限制，但不假裝已修復schema v1.0.0本身。因此兩筆紀錄都保持
`LOCAL_COMPONENT_MICROBENCHMARK`與`production_claim_allowed=false`。

## 7. Phase與production capability

| Phase | Machine status | 本輪能否升級 |
|---|---|---|
| P0 | IN_PROGRESS | 否 |
| P1 | IN_PROGRESS | 否 |
| P2 | IN_PROGRESS | 否；僅synthetic component PASS |
| P3 M1 | BLOCKED / evidence=0 | 否 |
| P4 M2/co-occurrence | BLOCKED / evidence=0 | 否 |
| P5 topology | BLOCKED / evidence=0 | 否 |
| P6 validator/export/query | IN_PROGRESS | 否；targets正確fail closed |
| P7 seven datasets | BLOCKED / evidence=0 | 否 |
| P8 release | BLOCKED | 否 |

## 8. 查詢方式

查本輪fresh component receipt：

```bash
jq '{
  benchmark_id,
  classification,
  production_claim_allowed,
  workload,
  result,
  semantic_invariants,
  limitations
}' state/benchmarks/20260719-bgzf-row-payload-replay-local.json
```

重算兩筆benchmark並查source binding：

```bash
scripts/ci/check_performance_benchmarks.sh
```

查phase readiness與evidence數：

```bash
jq -r '
  .phases[]
  | [.id,.status,(.evidence|length),(.blockers|join(";"))]
  | @tsv
' state/phase_ledger.json
```

重播determinism：

```bash
/usr/bin/ctest --test-dir build-cpp-output-validation --output-on-failure \
  -R '^(runtime_solver|determinism_contract|p2_determinism_matrix|performance_benchmark_records)$'
```

查本輪task與machine evidence：

```bash
jq . state/tasks/archive/20260719-cpp-output-efficiency-validation.json
jq . state/audits/20260719-cpp-output-efficiency-validation-001.json
scripts/ci/check_audit_source_snapshot.sh \
  state/audits/20260719-cpp-output-efficiency-validation-001.json
```

正式scientific artifact查詢仍只能由`longlineage-query`讀
`VALIDATED_FROZEN` run；目前沒有這種LongLineage production run可供查詢。

## 9. 最短production critical path

1. 完成P2完整worker input bundle、exact latest-tag join、one-pass multi-marker
   projection與staging整合。
2. freeze M1 RNG/logical digest/HP-family vectors，依序完成P3與P4 parity。
3. 先遷移topology三態schema，再完成q>4 DP/B&B/HiGHS、recurrence family與
   outward-rounded ranking certificate。
4. 完成獨立validator/fault injection/atomic freeze、legacy/query。
5. 最後才跑7 datasets × 24/40 workers，核對input SHA、全部semantic SHA與完整
   runtime/RSS/I/O receipt。

在第5步之前，最誠實且可稽核的結論維持：

> 已實作的C++ P2/TSV元件能產出一致logical data，writer在本機component
> workload有約8–10%改善；完整科學輸出一致性與end-to-end效率尚未證明，
> production仍為NO-GO。
