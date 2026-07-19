<!--
建立時間: 2026-07-19
目標: 稽核 P2 deterministic block planner、indexed BAM reader、logical retained-byte reorder、worker determinism 與 AI readiness
處理範圍: synthetic component only；不含 production VCF/sidecar/FASTA bundle、physical global memory proof、validator/freeze integration或7-dataset run
關聯檔案: LongLineage/state/audits/20260719-p2-block-pipeline-001.json、LongLineage/tests/unit/test_block_pipeline.cpp
-->

# P2 Block Pipeline 元件稽核

> **PARTIAL / Task Type A — 不可作 P2 phase completion 或 production release 證據**

## 結論

P2 的 bounded synthetic component 已建立並可重播：authoritative input order
block planning、每worker獨立的實體 BAM/明示 index handle、fixed read filter、
MM/ML單次解析、forced-out-of-order logical retained-byte reorder、pool/sink
cancellation，以及1/2/4/24/40 worker semantic determinism均通過。

這份證據只把 P2 維持在`IN_PROGRESS`。它不證明process-wide physical 8 GiB
上限，不包含production VCF/Tabix、latest HP/PS sidecar/Tabix、FASTA/FAI
worker bundle，也未接上independent validator與atomic freeze。P7的7-dataset
24/40 worker full replay尚未執行。

## 任務、目標與 source snapshot

- Task：`20260719-p2-block-pipeline`
- 類型／scope：`A / PARTIAL`
- 服務目標：LL-G2、LL-G4、LL-G5
- Source commit：
  `d2636eba1adcf503c675da79c63e8e799c8ac332`
- Canonical 204-blob tree SHA-256：
  `2d48b4c7e8eccc1a022f897781c136bf4eabd8e60bbe717c5a30cc4dc72ab0bc`
- Immutable command envelope：
  `LongLineage/state/audits/20260719-p2-block-pipeline-001.json`

## Step → Verify

1. 依dataset、contig、VCF record order與estimated alignments切block
   → 驗證：4,096/4,097 sites、250,000/250,001 estimated alignments、
   dataset/contig transition、dataset ID/order一對一與±5 kb clipped halo均有
   positive/negative C++ regression。
2. 建立每worker獨立的explicit-index BAM reader
   → 驗證：4個reader各自只fetch一次；缺index、非固定filter policy、
   malformed leading aux、block identity/order/interval/cost竄改均fail closed。
3. 固定production read filter與MM/ML orientation
   → 驗證：unmapped/secondary/supplementary/duplicate排除、MAPQ=20保留、
   length=999排除、missing MM/ML分離計數；reverse stored-G read還原
   as-sequenced C後得到query position 0的`C+m?` call。
4. 建立completion-order-independent reorder
   → 驗證：sequence 1在sequence 0前實際進入buffer；duplicate、late、gap、
   zero-size、writer exception及任何sequence的oversize item皆進terminal
   failure；cancellation喚醒blocked publisher。
5. 重播worker/chunk矩陣
   → 驗證：workers `1,2,4,24,40` × block size `1,2`，另加
   `40 workers + 4 BGZF writers`，全部80 logical rows與semantic SHA一致。
6. 注入worker failure
   → 驗證：canonical minimum failed sequence為0、ordered result batch為空、
   sink為`CANCELLED`且buffer清空、diagnostic staging保留。此測試不宣稱
   尚未整合的validator/freeze atomicity。
7. 驗證AI工作環境
   → 驗證：`scripts/ai/check_readiness.sh build-verify-debug` exit `0`、
   warnings `0`；獨立governance target的staleness判斷另有CTest regression。

## 實作契約

### Block planning

- Input order是唯一authority；planner只切分，不重新排序、刪除或複製site。
- dataset order與dataset ID必須雙向一對一。
- 同dataset內VCF record order嚴格遞增；同dataset/contig內position嚴格遞增。
- 預設上限為4,096 focal sites或250,000 estimated alignments，任一先到即切。
- 單site estimate已超過250,000時fail closed；v1沒有site內靜默分片規則。
- Focal interval使用`Interval0`，site position使用`Position1`。

### Indexed BAM reader

- 僅接受physical BAM；CRAM或其他格式不由副檔名推測接受。
- Caller必須明示index path；每個worker擁有自己的BAM/header/index handles。
- `AlignmentBlock`在建立iterator前重驗dataset、contig、record order、
  position、interval、halo、cost與production ceilings。
- Fixed filters：排除unmapped、secondary、supplementary、duplicate；
  MAPQ≥20；query length≥1,000；uppercase MM與ML存在。
- `bam_aux_get()`只有`ENOENT`可表示missing；corrupt aux導致
  `MALFORMED_VALUE`，不可誤計成missing tag。
- 每個retained read只decode sequence一次；同一decoded sequence交給MM/ML
  parser，並驗證長度等於`alignment.core.l_qseq`。

### Reorder與記憶體邊界

`T::retained_bytes() const noexcept`是sink唯一logical sizing authority。
Non-frontier payload最多使用`capacity - max_item`，保留一筆frontier credit
避免缺口deadlock。任何payload大於`max_item`都拒絕，因此completion order不能
決定相同logical data成功或失敗。

這不是physical RSS或global 8 GiB證明。尚未涵蓋：

- HTSlib/BGZF internal buffers；
- allocator/container overhead；
- thread stacks；
- task queue與generic result map；
- callback-local payload；
- process RSS與其他coordinator allocations。

Production仍需global permit pool、固定size acknowledgement整合與RSS/I/O實測。

## 驗證輸入、命令、輸出與結果

### 輸入

- Source：clean Git commit
  `d2636eba1adcf503c675da79c63e8e799c8ac332`
- Synthetic tests：
  `LongLineage/tests/unit/test_block_pipeline.cpp`
- Contracts：
  `LongLineage/include/longlineage/io/block_reader.hpp`、
  `LongLineage/include/longlineage/runtime/byte_bounded_reorder_sink.hpp`
- Build roots：`LongLineage/build-verify-{debug,release,sanitize,tsan}/`

### 命令與實際結果

| Command | Output | Exit／實際片段 |
|---|---|---|
| `scripts/ci/check_format.sh /tmp/clang-format-14-extract/usr/bin/clang-format-14` | formatter stdout digest在machine envelope | `0`; `FORMAT RESULT: PASS ... files=37` |
| `/usr/bin/cmake --build build-verify-debug --parallel 4` | `LongLineage/build-verify-debug/` | `0`; all targets built |
| `scripts/ci/check_all.sh build-verify-debug` | Debug binaries、CTest/governance/no-network results | `0`; `31/31`, `NO-NETWORK RESULT: PASS`, `FOUNDATION_PASS` |
| `/usr/bin/cmake --build build-verify-release --parallel 4` | `LongLineage/build-verify-release/` | `0`; all targets built |
| `/usr/bin/ctest --test-dir build-verify-release --output-on-failure` | Release CTest | `0`; `31/31 PASS` |
| ASan/UBSan build＋CTest（exact argv見envelope） | `LongLineage/build-verify-sanitize/` | `0`; `31/31 PASS` |
| TSan build＋`ctest -R '^p2_'` | `LongLineage/build-verify-tsan/` | `0`; `5/5 PASS` |
| `scripts/ai/check_readiness.sh build-verify-debug` | AI/cold-start/governance readiness | `0`; `failures=0 warnings=0` |

所有command的exact argv、start/end、exit code與stdout/stderr SHA-256位於
`LongLineage/state/audits/20260719-p2-block-pipeline-001.json`。空stderr的
SHA-256固定為
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。

### Determinism authority

- Frozen synthetic semantic SHA-256：
  `9179c42faf14000c9b1c87386a09cd33cae4bce27078d7d5af2798269c4fead0`
- Logical rows：每次80，record order `0..79`
- Worker matrix：`1/2/4/24/40`
- Chunk matrix：`1/2 sites per block`
- Writer sensitivity：另驗`40 compute + 4 BGZF writers`
- 非TSan build保留process thread assertion `<=46`；TSan runtime helper thread
  不作production thread-count evidence。

## AI、資料紀錄與查詢環境

- 跨agent governance：`LongLineage/AGENTS.md`
- AI cold-start與task模板：`LongLineage/.ai/`
- Machine task／lease／write-set：`LongLineage/state/tasks/`
- Machine phase state：`LongLineage/state/phase_ledger.json`
- Immutable audit lookup：`LongLineage/state/audits/`
- Artifact/schema authority：`LongLineage/schema/catalog.json`
- Record/format/query指南：
  `LongLineage/docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md`
- Query只可讀`VALIDATED_FROZEN`；P6 row execution尚未完成，不能用`zgrep`
  或notebook查未凍結資料後宣稱正式結果。

## 已排除或修復的非證據

- 一次未設定`ASAN_OPTIONS=detect_leaks=0`的local replay因ptrace環境使
  LeakSanitizer不可用；該exit非零run未納入PASS envelope。Address/UB checks
  以明示env重新執行並31/31 PASS。
- 第一個clean-commit readiness replay把unlinked producer header誤判為
  governance binary stale而exit 1。staleness scope已改為governance target的
  實際source/CMake inputs，加入positive CTest後重新擷取；舊失敗不列為PASS。
- Synthetic malformed aux fixture會產生HTSlib corruption diagnostic；測試預期
  LongLineage回`MALFORMED_VALUE`，不是忽略stderr後硬判成功。

## Remaining blockers

1. `P1_PREDECESSOR_NOT_VERIFIED`
2. `PRODUCTION_WORKER_INPUT_BUNDLE_VCF_SIDECAR_FASTA_NOT_IMPLEMENTED`
3. `ONE_PASS_CIGAR_MULTI_MARKER_PROJECTION_NOT_IMPLEMENTED`
4. `PRODUCTION_STAGING_VALIDATOR_FREEZE_NOT_INTEGRATED`
5. `PHYSICAL_GLOBAL_MEMORY_BOUND_NOT_PROVEN`
6. `FULL_DATA_WORKER_MATRIX_DEFERRED_TO_P7`

因此task以`BLOCKED + RELEASED`結案，P2 phase維持`IN_PROGRESS`，release
attestation維持`NOT_READY`。上述狀態代表證據邊界，不代表方向放棄。
