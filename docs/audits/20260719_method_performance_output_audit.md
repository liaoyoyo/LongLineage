<!--
建立時間: 2026-07-19 12:31 +08:00
目標: 稽核 LongLineage 方法加速、歷史策略轉用、資料減量、輸出與效能主張
處理範圍: repository-wide current P0-P2/P5 foundation；不含P7七資料集全量執行
關聯檔案:
  - LongLineage/docs/development/PERFORMANCE_AND_OUTPUT_POLICY.md
  - LongLineage/state/benchmarks/20260719-bgzf-row-payload-local.json
  - LongLineage/state/tasks/archive/20260719-method-performance-publish-audit.json
-->

# LongLineage Method, Performance and Output Audit

Task type: B — Comprehensive repository audit.
Claim ceiling: current implementation plus bounded evidence; no full-production speedup claim.

## 1. 結論

目前方法學採用的加速方向正確：cost-bounded blocks、worker-owned indexed BAM
handles、byte-bounded queue/reorder、BGZF packed output＋semantic SHA，以及exact
topology reductions，都能在不改科學問題下減少重複I/O、記憶體與小檔案。

但production `run`、完整worker input bundle、one-pass multi-marker projection、
M1/M2、q>4 topology、independent validator/index/query/export仍未完成。因此：

- 可以確認「架構與lossless策略適合」。
- 可以確認一個writer熱點在本機component benchmark改善。
- 不可以宣稱LongLineage全流程或七資料集已加速若干倍。

## 2. Current code truth

| 能力 | 狀態 | 目前證據 | 未完成 |
|---|---|---|---|
| 4,096 sites／250,000 estimated alignments／±5 kb block planner | `IMPLEMENTED` | boundary tests＋P2 semantic matrix | dense-site halo重複fetch尚未量測 |
| worker-owned BAM/BAI handle | `IMPLEMENTED` | indexed synthetic BAM | persistent VCF/sidecar/FASTA bundle |
| logical byte queue/reorder＋cancellation | `IMPLEMENTED` | adversarial/forced-order tests | process-wide physical RSS permits |
| BGZF TSV＋1/4 compression threads＋semantic SHA | `IMPLEMENTED` | exact decompressed bytes/digest | JSONL、LLM、site-index writers |
| exact latest-tag parser/join | component `IMPLEMENTED` | typed/synthetic join | worker-local persistent lookup |
| q≤4 brute-force structural oracle | `IMPLEMENTED` | complete family/no-winner tests | production DP/B&B/HiGHS/parent route |
| BQ-aware vertex-set ranking | `NO_GO/PLANNED` | small exhaustive historical oracle only | three-state schema、recurrence family、interval certificate、C++ parity |
| M1、M2/co-occurrence | `PLANNED/SKELETON` | contracts only | frozen parity kernels |
| validator/query/legacy | fail-closed stub | refuses false success | P6 implementation/fault injection |
| performance receipt | schema only | required fields closed | runtime collector與I/O ops缺失 |

程式路徑：

- `LongLineage/src/io/block_reader.cpp`
- `LongLineage/include/longlineage/runtime/ordered_thread_pool.hpp`
- `LongLineage/include/longlineage/runtime/byte_bounded_reorder_sink.hpp`
- `LongLineage/src/artifact/bgzf_tsv_writer.cpp`
- `LongLineage/src/solver/small_q_oracle.cpp`
- `LongLineage/contracts/v1/transform_registry.tsv`

## 3. 歷史策略與可轉用邊界

| 證據 | 分級 | 觀察 | LongLineage可轉用條件 |
|---|---|---|---|
| HCC1395 5kHz TO full vs same 18 regions subset | `MEASURED/BOUNDED` | 18/18 regions、15 metrics全部相同，max abs delta 0；subset約95M＋461K index，full約260G | 只證same-dataset/same-region bounded extraction；不可當全量parity |
| DORADO candidate-specific round | `MEASURED/BOUNDED` | 341 candidates→336 merged ±1 kb regions／674,733 bp；raw 482M、tagged 469M、round 3.1G | 只借用「先合併coordinate windows」；truth pool與±1 kb禁止進production |
| 7/7 latest HP/PS sidecar closeout | `MEASURED` | all_pass、164,253,537 mapped rows、638,259 VCF records、7/7 persisted_bam=false | raw BAM仍供SEQ/CIGAR/BQ/MM/ML；sidecar只供latest HP/PS且exact join |
| sidecar run root footprint | `MEASURED` | 21,491,305,116 B；raw TSV 15,005,688,288 B、BGZF 6,256,168,164 B、TBI 4,826,702 B、regular BAM 0 B | LongLineage直接stream BGZF＋index，不永久保留raw duplicate |
| historical tagged BAM total | `MEASURED CONTEXT` | 約1.674 TiB | 不同payload容量context；不是runtime或full-BAM semantic parity |
| sidecar vs historical footprint | `DERIVED` | 約85.64× smaller／98.832% physical reduction | 明示不同payload與denominator；不可改寫為speedup |
| 重疊window read共享減BAM I/O 30–50% | `ESTIMATED` | 歷史建議，無A/B measurement | 只能列假設；以BGZF blocks/read bytes counters重新量測 |
| packed native 24 files vs 1,409,547 legacy plan | `DERIVED-PLAN` | 約58,731× fewer files | 24來自16 artifacts＋8 indexes；P7須實測final/transient census |
| exact topology bounded probes | `MEASURED/BOUNDED` | active-bit/reduction/B&B與single-terminal route顯著縮小state work且digest一致 | 只移植exact route；不移植time-limit winner或bounded speedup倍數 |
| 2026-07-19 hard25／compressed VAF handoff | `MEASURED/BOUNDED, NO_GO` | optimized objective 25/25、family complete 16/25；small oracle PASS但hard ranking未跑且production promotion=false | 分離objective/family/ranking；BQ只評vertex set；不得宣稱production speedup |

主要歷史來源：

- `external://InterSubMod/docs/reports/validated/2026/03/20260312_TO_snapshot_scope_same_scope_control_01.md`
  SHA-256 `5a6f47b2376bf3ff196598aae3582534f7bf78aafb5231b177d7bafb04712747`
- `external://InterSubMod/docs/experiments/in_progress/2026/03/20260311_HCC1395_DORADO_TO_candidate_specific甲基rescue驗證_01.md`
  SHA-256 `04fe75a5000bc217815d9fb8161441a7b9a2e2bfd3505293fa14054684604d57`
- `external://raw_all_production_sidecars_v2/raw_all_receipt_closeout.json`
  SHA-256 `d5360b7422ee1017d71f60ede51bc3283160e57c6abbb78daa3e74090362ab7c`
- `external://InterSubMod/research/20260710_layered_reconstruction_v2/20260711_無TruthBED生產標記_FailClosed與Frozen全量重建完整流程_01.md`
  SHA-256 `be17987c337403dad1f877b64e1c930ccba79444741182a954094f5375e6bcb5`
- `external://InterSubMod/research/20260718_solver_methyl_edge_probe/20260718_Hypercube邊與subcube改良研究計畫_01.md`
  SHA-256 `8fbdf16082a4bd8b4e6ad0a2c976a708b5f29e29713d440a7c6ea7a2a7facad8`
- `external://InterSubMod/research/20260718_solver_methyl_edge_probe/20260719_最簡演化樹與VAF排序AI交接重點_01.md`
  SHA-256 `7b412e89561924f0dff95a42bc69319b6d7e296ab30792cbe236a1bd16feb306`
- `external://InterSubMod/docs/experiments/in_progress/2026/05/20260510_longphase_bam_inventory_proposal_01.md`
  SHA-256 `d8af7fb8dfc70d4818ba34d9be027c7880dcc6cfd0f23d577161f38789bd0c60`

### 3.1 最新topology/VAF交接重播

本輪沒有執行歷史Python科學程式；只重讀SHA-bound receipts與source bindings。

輸入：

```text
InterSubMod/research/20260718_solver_methyl_edge_probe/
  results/solver_stress_panel_v1/.../receipt.json
  results/compressed_vaf_rank_probe_v2/main_reverify_r3/receipt.json
```

驗證：

```text
R3 authority pointer                         true
hard25 FAIL_CLOSED／50 worker rows           true
optimized objective 25／family complete 16   true
incomplete_ranked                            0
performance ratio kind                       NOT_COMPARABLE
compressed small oracle                      PASS
hard ranking_complete                        false
production promotion allowed                 false
receipt SHA sidecars                         2/2 PASS
source bindings                              7/7 PASS
```

採用結果記錄於
`LongLineage/docs/decisions/ADR-0005-topology-ranking-separation.md`。現行
`topology_unit` 1.0.0仍缺獨立`ranking_state`，並把parent additive score與winner
放在同一語意；因此production ranker在schema migration前保持阻擋。Candidate
family若達1.22億等output-sensitive規模，不能默默省略rows；compressed exact
representation必須另立versioned contract與獨立oracle。

## 4. 本輪lossless writer改善

### Step

`BgzfTsvWriter::write_row()`原先：

1. join fields成line；
2. physical write另複製line並追加LF；
3. semantic digest再複製line並追加LF。

本輪改成一次建立canonical `row+LF` payload，同一buffer依序送
`bgzf_write()`與EVP SHA-256。Header/preamble、field validation、physical close
hash及error behavior保留。

### Verify

輸入：

```text
LongLineage/benchmarks/bgzf_writer_microbenchmark.cpp
500,000 rows × 5 columns
71,888,972 logical bytes
4 BGZF writer threads
8 trials before + 8 trials after
```

執行：

```bash
<variant-binary> /tmp/longlineage_bgzf_<variant>_<trial>.tsv.bgz 500000 4
```

結果：

| 指標 | Before | After |
|---|---:|---:|
| median wall seconds | 0.9010325 | 0.8295770 |
| local component reduction |  | 7.930402067% |
| logical bytes | 71,888,972 | 71,888,972 |
| physical bytes | 1,610,593 | 1,610,593 |
| semantic SHA-256 | `e6db09…48f06b` | `e6db09…48f06b` |

這是`LOCAL_COMPONENT_MICROBENCHMARK`，cache condition=`MIXED`，
`production_claim_allowed=false`。完整raw trials、environment、source/binary
digests及限制在：

`LongLineage/state/benchmarks/20260719-bgzf-row-payload-local.json`

驗證命令：

```bash
scripts/ci/check_performance_benchmarks.sh
ctest --test-dir build-verify-release --output-on-failure \
  -R 'performance_benchmark_records|runtime_solver|determinism_contract|p2_determinism_matrix'
```

實際輸出：

```text
PERFORMANCE RECORD RESULT: PASS records=1
100% tests passed, 0 tests failed out of 4
```

## 5. 下一批優先序

1. **Governed performance collector**
   補wall/user/system、cgroup RSS/OOM、I/O bytes/ops、faults、threads、
   queue/reorder wait、latency quantiles、fetch/open/parse counters。
2. **Persistent full worker bundle＋bounded halo coalescing**
   降低open/index及重疊BGZF解壓；以semantic SHA、exact join與counter守恆拒絕漂移。
3. **Fused one-pass retained-record decoder/projector**
   一次CIGAR、aux、MM/ML處理所有markers並立即釋放raw payload。
4. **Streaming site indexes＋global BGZF permits**
   不per-site flush、不超46 threads，獨立validator重建offset/range digest。
5. **Exact topology production router**
   先完成ADR-0005三態schema migration，再保留獨立q≤4 brute-force oracle，
   不讓production reduction與oracle共用kernel。

## 6. 明確拒絕的「加速」

- truth BED/VCF/label或truth-derived candidate pruning。
- 將歷史±1 kb替代正式±5 kb partner universe。
- sidecar missing時fallback BAM舊HP。
- O/X→R、刪除failed/incomplete rows或只輸出「好看」結果。
- M1/M2 permutation early-stop造成PCG64 consumption改變。
- Endpoint A asymptotic fallback。
- UPGMA／shortest-path／heuristic family取代exact topology。
- incomplete cache當complete、cap/deadline輸出winner。
- 用BQ-aware vertex-set likelihood替同一vertex set選parent edges。
- 用普通float upper bound剪枝後發布best/tie，或把scalar AF heuristic混入
  primary endpoint。
- 在未立exact compressed-family schema前省略candidate rows或只寫count。
- 量化decision floats、略過physical SHA或validator重讀。

## 7. 輸出與查詢判定

- Catalog replay：16 artifacts、8 indexes、14 queryable；目標24 files。
- 已實作：TSV_BGZF writer、semantic SHA、run-state guard。
- 未實作：JSONL/LLM/index writers、production catalog/lineage writer、query row
  replay、legacy exporter、validator/freeze。
- Query若每次重算full artifact SHA會放大I/O；若只驗range digest則需先以ADR固定
  frozen-root threat model。不得自行加入mtime cache。
- 建議P7前補per-artifact logical bytes、I/O operation counts、
  transient peak files/bytes與compression context。

## 8. GitHub狀態

本輪開始時：

```text
gh version 2.65.0
account liaoyoyo active
configured token invalid
local remote absent
```

完成狀態：

```text
repository     liaoyoyo/LongLineage
visibility     PRIVATE
default branch main
main SHA       3a789d3c8b384606dfad01ae0227834df01661ff
P2 SHA         1d986806f4fe1d4c8617bfa7fa2915f9bafd513f
audit SHA      3ef1f7f18db76f8213c72b2947713a2c73781d6f
draft PR       https://github.com/liaoyoyo/LongLineage/pull/1
```

官方device flow完成後，restricted sandbox內的`gh auth status`因網路阻擋誤報
token invalid；在允許網路的驗證邊界重播後，帳號、`repo`與`workflow`scopes均
PASS。Token未寫入repo、task、audit或對話。三個遠端branch SHA已用
`git ls-remote --heads`逐一核對，正式本機repo亦已追蹤audit branch。

## 9. Release blockers

- `PERFORMANCE_COLLECTOR_NOT_IMPLEMENTED`
- `PRODUCTION_WORKER_INPUT_BUNDLE_VCF_SIDECAR_FASTA_NOT_IMPLEMENTED`
- `ONE_PASS_CIGAR_MULTI_MARKER_PROJECTION_NOT_IMPLEMENTED`
- `PHYSICAL_GLOBAL_MEMORY_BOUND_NOT_PROVEN`
- `JSONL_LLM_SITE_INDEX_WRITERS_NOT_IMPLEMENTED`
- `TOPOLOGY_RANKING_THREE_STATE_SCHEMA_NOT_MIGRATED`
- `TOPOLOGY_RECURRENCE_FAMILY_AND_INTERVAL_CERTIFICATES_NOT_VERIFIED`
- `INDEPENDENT_VALIDATOR_QUERY_EXPORT_NOT_IMPLEMENTED`
- `SEVEN_DATASET_COMPARABLE_PERFORMANCE_MATRIX_DEFERRED_TO_P7`
