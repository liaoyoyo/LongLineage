<!--
建立時間: 2026-07-19 12:31 +08:00
目標: 定義 LongLineage 可接受的加速、減量、效能主張、紀錄格式與查詢準則
處理範圍: production C++ runtime、artifact writer、validator、query、legacy export、P7 benchmark
關聯檔案:
  - LongLineage/governance/performance_benchmark.schema.json
  - LongLineage/state/benchmarks/
  - LongLineage/schema/core/run_receipt.schema.json
  - LongLineage/schema/catalog.json
  - LongLineage/docs/data/DATA_CONTRACTS.md
-->

# Performance and Output Policy

Status: Accepted for development; production claims remain blocked until P7.

## 1. 一句原則

LongLineage只接受「不改輸入authority、不改科學語意、不漏record、可由獨立
validator重建」的加速。任何更快結果若改變read set、R/A/O/X、RNG consumption、
status、candidate family、parent mapping、canonical order或semantic SHA，都不是
優化，而是方法變更。

## 2. 效能證據分級

| 標籤 | 定義 | 可下的結論 |
|---|---|---|
| `IMPLEMENTED` | code path存在且有功能測試 | 「已實作」；不能報加速比例 |
| `COMPONENT_MEASURED` | synthetic component、固定workload、至少5次重播 | 只報該component與該環境 |
| `BOUNDED_MEASURED` | 明示sample/region的real-data A/B | 只報該bounded scope |
| `FULL_PRODUCTION_MEASURED` | P7 exact 7-dataset scope、同輸入/環境、24/40 workers | 可報production觀察值 |
| `DERIVED` | 由已量測數字算出的比例或file-count | 必須同時列分子、分母 |
| `ESTIMATED` | capacity model或歷史建議 | 不得寫成實測 |
| `PLANNED` | 尚無producer/validator evidence | 只可列為roadmap |

`COMPONENT_MEASURED`與`BOUNDED_MEASURED`紀錄固定
`production_claim_allowed=false`。Production主張只能由兩個
`VALIDATED_FROZEN` P7 run receipts及比較receipt共同授權，不能由開發benchmark
record自行升級。

## 3. 可比性鍵

任何數值A/B比較必須記錄並核對：

1. 相同manifest、input lock、input before/after SHA與dataset/site order。
2. 相同science-parameters、status registry、schema catalog與release attestation。
3. 相同compiler flags、HTSlib、compression backend、host/cgroup、filesystem及
   cache condition；差異要列為confound。
4. 相同worker/writer/thread ceiling、block/halo/buffer設定，除非該設定正是
   自變項。
5. 兩邊均零worker error、零OOM、零missing/extra/duplicate，且所有scientific
   artifact semantic SHA相同。
6. Component benchmark至少5次並報raw trials及median；P7保留24與40 workers各
   一次完整run，不用最佳一次取代。

不符合以上鍵值的歷史數據仍可作capacity context，但必須標
`NON_COMPARABLE_CONTEXT`。

## 4. 不可改變的科學語意

- production不讀truth，不以truth-derived candidate或region減少工作。
- latest HP/PS只由manifest指定的7/7 sidecar exact identity join取得；missing、
  multimatch、conflict不得fallback到BAM舊HP。
- partner universe固定focal inclusive ±5,000 bp；歷史±1 kb window不能搬用。
- O與X不合併到R。
- M1 PCG64 seed、shuffle、replicate consumption、tie-break與cluster relabel不變。
- M2 permutation count、formal exact-state ceiling及fail precedence不變；不採
  asymptotic fallback。
- topology reductions必須exact-preserving；small-q DP只證明h*，B&B仍列完整
  minimum family；cap/deadline/incomplete不得產winner。
- 不使用`-ffast-math`、`-march=native`或會改決策浮點的近似。

## 5. 核准的lossless策略

### 5.1 Input與工作切分

- 每stable worker持有獨立、persistent BAM/BAI、VCF/Tabix、sidecar/Tabix及
  FASTA/FAI handles。
- 同dataset/contig相鄰且halo重疊的blocks可在global permit內合併physical fetch；
  constituent block sequence、site membership與counter grain不可改。
- 每read只走一次CIGAR、typed aux與MM/ML，並同時投影所有sorted markers；消費後
  釋放raw payload。
- queue、reorder、HTSlib/BGZF buffers、thread stacks及callback-local results都
  必須納入global physical-memory budget；caller-declared logical bytes不是RSS
  證明。

### 5.2 Exact topology

固定路由：

`active-bit compression → duplicate/forcing/dominance/downward closure →`
`single-terminal analytic → small-q DP objective → bitset B&B full family →`
`direct pinned HiGHS when required → fixed-node parent mapping once`

Structural result cache只接受完整certificate及其input digest。Incomplete結果只能
作checkpoint，不可作winner或complete cache hit。

`objective_state`、`family_state`與`ranking_state`是三個獨立完成軸；後者不得
由前兩者推定。Primary ranking固定為
`BQ_AWARE_READ_PATTERN_MIXTURE_V1`，每個distinct candidate vertex set只fit
一次，不能拿同一分數挑選該vertex set內的parent edges。歷史scalar AF
monotonicity只能是另名sensitivity endpoint，不得混為primary likelihood或
silent tie-break。

普通floating-point upper bound只可安排搜尋順序或作diagnostic。只有replayable
outward-rounded interval certificate可排除published rank之外的branch；
certificate不完整時不得輸出best candidate或tie class。完整規範見
`docs/decisions/ADR-0005-topology-ranking-separation.md`。

### 5.3 Output

- Native SoT為catalog中16個artifacts及8個sibling indexes；目標frozen root為
  24個regular files。此為catalog推導值，需由P7實測file census確認。
- BGZF/JSONL/LLM直接stream到staging；禁止另外保留raw TSV duplicate。
- Site index與artifact同步streaming建立，禁止每site `bgzf_flush`。
- Row canonical bytes以單一payload同時送BGZF writer與semantic digest。
- BGZF compression threads必須由global permit分配；不可每個concurrent writer
  各自無界建立threads/queue。
- Physical SHA在close後完整重讀保留，因為它是獨立validator與freeze的證據；不可
  為速度略過。
- Freeze只允許同filesystem atomic directory rename；不提供silent copy fallback。
- Legacy export為明示opt-in、輸出到run root外，先做inode/space preflight，再驗
  schema、row order與logical digest parity。
- Candidate family目前只允許explicit vertex-set records。若family達到
  output-sensitive規模，cap/deadline仍須標為incomplete；exact compressed
  family/count/tie representation必須先有新ADR、schema、獨立展開/count oracle
  及query semantics，不能為減少輸出而靜默替換。

## 6. File與byte計數定義

- `final_file_count`：`VALIDATED_FROZEN` root內所有regular files，包含artifacts、
  indexes及receipts；不含目錄、symlink、presentation及legacy export。
- `logical_records`：catalog中scientific/artifact records總數，不含BGZF preamble。
- `logical_bytes`：各artifact semantic stream bytes總和。
- `transient_file_count`：run-owned staging與temporary roots在任一時點的regular
  file peak；不含immutable inputs。
- 現有run receipt缺`transient_peak_bytes`與I/O operation counts。P7前必須升版
  補上，不可用final bytes推測peak storage。

## 7. Performance record格式

Bounded benchmark紀錄位於：

```text
LongLineage/state/benchmarks/<YYYYMMDD>-<component>-<scope>.json
```

Schema：

```text
LongLineage/governance/performance_benchmark.schema.json
```

每筆必含workload、environment、baseline/candidate source digests、exact commands、
raw trials、median、semantic invariants、limitations與
`production_claim_allowed=false`。CI同時重播baseline Git blob、candidate file及
harness SHA，並注入偽production claim確認schema fail closed。

## 8. 查詢方式

驗證全部benchmark records：

```bash
scripts/ci/check_performance_benchmarks.sh
```

列出component結果與主張上限：

```bash
jq -r '
  [.benchmark_id,.classification,.component,
   .result.before_median_seconds,.result.after_median_seconds,
   .result.relative_wall_reduction_percent,
   .production_claim_allowed] | @tsv
' state/benchmarks/*.json
```

查catalog的final artifact/index census：

```bash
jq '{
  artifacts:(.artifacts|length),
  indexes:([.artifacts[]|select(.index != null)]|length),
  queryable:([.artifacts[]|select(.queryable)]|length)
}' schema/catalog.json
```

正式run效能只能從validated receipt查：

```bash
jq '{run_id,state,performance}' <VALIDATED_FROZEN_ROOT>/run_receipt.json
```

Query對artifact integrity採「每次全檔hash」或「frozen-root＋validated range
digest」尚需ADR固定threat model；在此之前不得用mtime cache或略過驗證來降低
query I/O。

## 9. Step → Verify

1. 建persistent worker bundle
   → 驗證：open/index counts下降；exact join、R/A/O/X rows及semantic SHA零差異。
2. 合併overlapping halo physical fetch
   → 驗證：BGZF blocks、iterator records、read bytes下降；site order/counters不變。
3. 完成one-pass decoder/projector
   → 驗證：CIGAR/aux/MMML visits每read一次；slow oracle與全部status/digest一致。
4. Streaming writers/indexes
   → 驗證：decompressed canonical bytes、row/range digest及1/4 writer-thread semantic
   SHA一致；全域threads≤46。
5. P7 24/40 workers
   → 驗證：7 datasets兩次semantic SHA相同、input SHA不變、validator PASS，並完整
   記錄wall/user/system、RSS/OOM、I/O bytes/ops、faults、threads、wait與latency。
