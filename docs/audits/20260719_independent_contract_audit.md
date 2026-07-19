<!--
建立時間: 2026-07-19
目標: 獨立稽核 LongLineage 的 AI 工作環境、開發紀律、資料紀錄、格式定義、receipt、provenance 與查詢契約
處理範圍: P0-P8 machine-readable governance、schema/catalog、typed I/O、packed writer、topology/query contract、CI 與 release gate
關聯檔案: AGENTS.md、state/、governance/、schema/、contracts/v1/、provenance/、apps/、src/、scripts/ci/
任務類型: B_COMPREHENSIVE_VALIDATION
-->

# LongLineage 獨立資料契約與 AI 開發紀律稽核

## TL;DR 與裁定

**Foundation gate 可重播通過，但不得升級任何 phase：AI cold-start、task type、
Step→Verify、closed schema catalog、typed I/O、Debug/Release build 與 23/23 CTest
均已建立；獨立 format gate 尚未在此 snapshot 通過，P0/P1 另有 machine-state、
provenance lifecycle、record-schema 與 multi-agent ownership 缺口，P2/P6 仍有
明確 implementation blockers，P3-P5 科學 parity 維持 `BLOCKED`，release
attestation 維持 `NOT_READY`。（影響：高，信心：高）**

本文件採 **Evidence Chain + Step→Verify**。它是獨立稽核快照，不是
`VALIDATION_RECEIPT`、不是 phase evidence 的自動升級，也不授權 production run。

| 稽核面向 | 本次裁定 | 可支持的主張 | 不可支持的主張 |
|---|---|---|---|
| AI 工作環境與流程 | **FOUNDATION PASS / 尚待封閉** | cold-start、task type、Step→Verify、phase ledger、living notes、CI 均有 SoT | 多 agent lease/ownership 已安全協調 |
| Schema／format 契約 | **CONTRACT SHAPE PASS** | 16 artifact、offline schema ID、record schema hash、index key、closed membership 均可機器驗證 | producer/validator 已逐 row 執行所有 semantic invariant |
| P1 typed I/O | **COMPONENT PASS / PHASE IN_PROGRESS** | typed coordinates、R/A/O/X、MM/ML、latest HP/PS exact join 與 synthetic indexed HTS smoke 已通過 | 全檔 cross-input closure、7-dataset 或 release input integrity 已完成 |
| P2 runtime／writer | **IN_PROGRESS** | byte-bounded queue、ordered pool、BGZF physical envelope 與 semantic SHA primitive 已測試 | block reader、schema-aware packed writer、40-worker production determinism 已完成 |
| P3-P5 science | **BLOCKED** | bounded small-q structural oracle與 fail-closed abstain 存在 | M1/M2/co-occurrence/topology parity 或正式 winner authority 已完成 |
| P6 validator／query | **IN_PROGRESS / executable fail closed** | validator/query skeleton 不會偽造 PASS 或回傳未驗證 rows | receipt replay、fault injection、indexed query 或 legacy parity 已完成 |
| Release | **NOT READY（預期安全結果）** | release gate 能阻止未驗證 science 發布 | private remote、P7 full run、v1 release 已完成 |

本次**沒有修改** `state/project_state.json`、`state/phase_ledger.json`、
`state/release_attestation.json` 或任何 producer/science source；沒有執行歷史
Python science authority，也沒有讀 real BAM/VCF/sidecar 進行科學分析。

## 稽核範圍、假設與 Step → Verify

### 關鍵假設

1. `${REPO}` 指向待稽核的 LongLineage source snapshot。
2. `${DEBUG_BUILD}` 與 `${RELEASE_BUILD}` 是獨立、fresh、out-of-tree build roots。
3. `${SOURCE_REPO}` 指向 manifest 宣告的來源 repository snapshot；只做 SHA-256
   replay，不執行來源 Python。
4. `${CLOSEOUT_RECEIPT}` 指向既有 7/7 sidecar closeout receipt；只讀取 receipt。
5. Standard JSON Schema 只能執行 draft 2020-12 keyword；自訂
   `x-longlineage-invariants` 是註解，必須由獨立 C++ validator 執行。

### Step → Verify

1. 稽核 AI governance 與 machine-readable state
   → 驗證：cold-start、task type、Step→Verify、active task、P0-P8 ledger、
   protected decisions 與 release attestation 均有 closed schema；compiled
   governance exit `0`。
2. 稽核 schema、artifact membership 與 record format
   → 驗證：catalog record schema SHA、offline `$id`、primary/index key、type、
   status domain與 closed acyclic membership 全部通過；標準 JSON Schema
   positive/negative fixtures exit 符合預期。
3. 稽核 typed I/O 與資料語意
   → 驗證：Position1/Interval0、R/A/O/X、typed aux、MM/ML、latest HP/PS
   exact vocabulary 與 indexed HTS smoke 有可重播 component evidence。
4. 稽核 producer、validator、query 與 freeze boundary
   → 驗證：target link separation成立；未實作 validator/query 明確 fail closed；
   runtime guard與 low-level writer 不被誤認成 filesystem validator。
5. fresh Debug/Release build 與 concurrent replay
   → 驗證：兩個 configure、兩個 build、兩個 CTest 與兩個 foundation
   `check_all` 均 exit `0`；CTest 各 `23/23 PASS`。
6. 執行 strict release negative gate
   → 驗證：strict gate只因 `VALIDATOR_FAULT_INJECTION` 無 test而 exit `1`；
   release gate exit `1`，與 ledger／attestation 的 blocker 一致。
7. replay provenance 與 sidecar closeout
   → 驗證：source hashes `12/12` match；closeout SHA 可重算且為
   `dataset_count=7`、`all_pass=true`、`receipts=7`、
   `total_mapped_alignments=164253537`。
8. 將剩餘缺口分類
   → 驗證：P0/P1 必修、P2/P6 blocker、P3-P5 protected science blocker、
   P8/nice-to-have 分開列出；不以 foundation PASS 覆蓋 phase blocker。

## 已通過的核心證據

### 1. AI 工作環境與開發紀律

- `AGENTS.md:7-23` 定義 cold-start 五問；`AGENTS.md:25-33` 定義語言、
  Step→Verify 與執行證據；`AGENTS.md:51-64` 定義 A-F task type。
- `AGENTS.md:98-120` 鎖定 P0-P8 與「incomplete family 不得有 winner」；
  `AGENTS.md:122-154` 把資料格式與查詢紀律列為 repo governance；
  `AGENTS.md:156-185` 定義 schema-first、negative-test-first、phase honesty 與 Git
  hygiene。
- `.ai/templates/task.json:1-24` 與
  `governance/agent_task.schema.json:1-110` 已把 scope、assumptions、inputs、
  outputs、Step→Verify、evidence、deviation 與 blocker 結構化。
- `state/tasks/active/20260719-foundation.json:4-79` 明示 task type B、FULL
  scope、輸入／預期輸出與可觀察驗證；`evidence=[]` 與 `IN_PROGRESS` 相容，
  沒有偽造完成。
- `apps/governance_main.cpp:438-592` 強制 P0-P8 順序、phase predecessor、
  VERIFIED evidence 與 ROADMAP/CURRENT_FOCUS mirror；`:594-803` 驗 active task。
- `.github/workflows/ci.yml:18-97` 覆蓋 GCC/Clang、Debug/Release、
  ASan/UBSan、pinned HTSlib、format 與 foundation gate。這只證明 workflow
  已接線；format 的 snapshot 結果另見 AUD-P01-011。
- `CMakeLists.txt:70-80` 明確使 validator/query/export/evaluate 不連結
  producer core，符合 trust-domain separation。

### 2. 資料記錄、format 與 provenance graph

- `schema/catalog.json:14-110` 定義無循環 closeout membership：
  scientific artifacts → catalog/lineage → semantic digests → producer receipt
  → checksums；checksums 明確排除 self、validation receipt 與 run receipt。
- `schema/catalog.json:111-330` 對各 artifact 固定 path、physical format、
  record schema SHA、primary/sort/index key、producer、validator與 queryability。
- `schema/id_registry.json:4-155` 提供 offline schema ID 與 catalog schema
  digest locks；`apps/governance_main.cpp:1249-1324` 重算註冊 schema SHA 並解析
  offline `$ref`；`:1326-1437` 驗 artifact/schema/key closure；`:1440-1513`
  驗 run membership。
- `contracts/v1/type_registry.tsv:1-21` 固定整數、SCI17、Position1、Interval0、
  SHA、canonical JSON、status/reason 與 HP state；record schemas 另綁
  `status_reason_codes.tsv`。
- `docs/data/DATA_CONTRACTS.md:68-112` 定義 TSV/JSONL/BGZF physical encoding
  與 semantic SHA；`:114-175` 定義 index、receipt、lineage與 atomic freeze；
  `:177-214` 定義 count grains、per-unit evidence與 lifecycle。
- `docs/data/LLM_V1.md:3-36` 對 binary matrix 固定 endian、frame layout、
  mask、per-frame checksum、semantic SHA 與 BGZF EOF。
- `apps/governance_main.cpp:811-908` 驗 tabular header、field key、type、
  null、const與 unit；catalog primary/index key 必須 resolve 到 schema field。

### 3. Typed I/O 與 O/X 保真

- `include/longlineage/common/types.hpp:13-67` 用 value type 隔離 Position1 與
  non-empty Interval0；`:94-150` 使 R/A/O/X 四態不可混用。
- `include/longlineage/io/mm_ml.hpp:45-55` 固定 C+m? target、C+h? offset-only
  與 uppercase MM/ML/MN；不 fallback 到舊 tag。
- `include/longlineage/io/sidecar.hpp:16-31` 固定 sidecar header、source role與
  optional uint64 PS；`src/io/sidecar.cpp:61-75,189-203` 固定 raw HP vocabulary、
  `.` → canonical `0` 與 PS parse。
- `src/io/alignment.cpp:68-132` 保留 typed auxiliary tag 的 type/bytes、
  duplicate occurrence與 deterministic tag order。
- `schema/records/cooccurrence_pairs.record.json:47-62,122-137` 明示完整 16 個
  R/A/O/X cells；`:164-173` 禁止 O/X collapse 並定義 pair conservation。
- `docs/audits/20260719_typed_io_audit.md:18-68` 記錄 P1 component
  Step→Verify；`:280-343` 記錄 actual indexed synthetic HTS smoke 與 tampered
  input rejection；`:345-357` 記錄 Debug/Release 23/23。

### 4. Topology、receipt 與 query 的 fail-closed 契約

- `schema/records/topology_unit.schema.json:6-29` 要求 input patterns 與兩種
  evidence digest；`:122-242` 對 incomplete/abstain 強制 winner null；
  `:245-303` 分離 FULL_STATE/PARTIAL_SUBCUBE 與 multiplicity，winner tie固定
  false。
- `schema/core/query_response.schema.json:6-79` 要求 run/artifact/schema、
  normalized filter、query plan、counts與 receipt/checksum/digest anchors；
  `:90-186` 使用 closed AND-only AST；`:188-221` 對 incomplete scan 與 index
  binding做標準 schema 約束。
- `apps/cli_support.cpp:376-504` 驗 immutable manifest-bound release
  attestation；`apps/longlineage_main.cpp:361-380` 在 science未驗證時拒絕 run。
- `apps/validate_main.cpp:31-36` 未實作 validator 固定 exit
  `ValidationFailed`，不寫 receipt；`apps/query_main.cpp:50-59` 在 gate 後仍
  `QUERY_REJECTED` 且不回 rows。
- `docs/data/QUERY_GUIDE.md:3-15` 清楚標示 P6 尚未 VERIFIED；
  `:34-79` 才是 target contract，不冒充目前可用功能。

## 資料定義與查詢的正確使用順序

任何人類或 AI 都不應從範例 row、舊報告或聊天記憶反推格式。固定查詢順序為：

1. **找 schema ID**：`schema/id_registry.json`，確認 `$id`、path 與 SHA。
2. **找 artifact identity**：`schema/catalog.json`，確認 artifact ID、path、
   physical format、record schema SHA、primary/sort/index key與 membership。
3. **讀 record schema**：確認 header/property order、type、null、const、
   unit、enum、status-domain與 invariants。
4. **讀 closed registries**：
   `contracts/v1/{type_registry,status_reason_codes,transform_registry,artifact_roles,query_operators}.tsv`。
5. **查特定 run**：依序重播 producer receipt、checksums、
   validation receipt、run receipt、artifact physical SHA、semantic SHA與 nested
   index binding；不得只看檔名或 `state` 字串。
6. **查 row**：P6 完成後只用 `longlineage-query`；exact key優先，scan必須
   明示 budget/limit。P6 前只可檢查 schema/receipt，不可自行寫 notebook
   重算、補值或聚合 scientific result。
7. **查 lineage**：由 run receipt 的 typed input binding 與
   `data_lineage` 反查 source ID、digest kind、transform ID與 producer
   executable；bare digest 不構成 provenance。

每筆正式 scientific record 至少需能回答：

| 問題 | 權威欄位／檔案 |
|---|---|
| 這是什麼資料？ | artifact ID＋schema name/version |
| 物理 bytes 是哪一份？ | relative path＋physical SHA-256 |
| 邏輯 rows 是否相同？ | semantic SHA-256＋logical row count |
| identity 與順序是什麼？ | primary key＋sort key＋index key |
| 缺值、錯誤、abstain 是什麼？ | null rule＋status/reason registry |
| 誰產生、用什麼轉換？ | producer executable SHA＋transform ID |
| 由哪些輸入而來？ | typed source ID＋digest kind＋SHA＋data lineage |
| 誰驗證？ | validation receipt＋validator executable SHA |
| 是否可查？ | `VALIDATED_FROZEN` run receipt＋queryable flag＋validated index |

## 必須先修：P0／P1

### AUD-P01-001 — project state 與 phase ledger 的 open-gate 語意漂移

**證據**：`state/phase_ledger.json:7-29` 的 P0 是 `IN_PROGRESS` 且有
`PRIVATE_REMOTE_NOT_YET_VERIFIED`；但 `state/project_state.json:20-29` 的
`open_gates` 從 P1 開始，遺漏 `P0_AUTHORITY_AND_PROVENANCE`。
`apps/governance_main.cpp:387-403` 只檢查 open gate 是否屬 allowed vocabulary
及不重複，沒有把 open gates 與 phase ledger 非 VERIFIED 狀態做集合相等檢查。

**影響**：machine reader可能把 P0 誤讀成已關閉；compiled governance 的
「internally consistent」訊息過度寬鬆。

**封閉條件**：定義 open-gate derivation（建議等於所有非 VERIFIED、
非 NOT_STARTED且需追蹤的 phase），由 governance 重算並加入 missing/extra
negative fixtures；當前 state 必須包含 P0。

### AUD-P01-002 — private Git remote 尚未建立／驗證

**證據**：`state/phase_ledger.json:27-29` 明列 blocker；
`scripts/ai/check_readiness.sh:138-146` 在非 Git repo 時只警告；fresh readiness
實際為 exit `0`、warnings `1`。

**影響**：P0 的「新歷史、private remote、唯一 main」尚無可重播證據。

**封閉條件**：初始化新 history，驗 remote exact identity、default branch、
visibility=`PRIVATE`、無 real data/credentials，將 command、exit、remote
receipt/digest寫入 P0 evidence；公開前仍需使用者明示授權。

### AUD-P01-003 — source-to-target provenance 缺 implementation lifecycle

**證據**：`provenance/source_to_target_manifest.json:7-103` 有 12 筆 source hash、
target、transformation與 reuse；本次 source SHA replay為 `12/12` match，但
target presence replay為 `existing=3, absent=9`。manifest 沒有
`implementation_status`、`verification_status`、target digest或 evidence binding。

**影響**：absence目前可合理代表 planned port，但 machine consumer無法區分
`PLANNED`、`SKELETON`、`IMPLEMENTED`、`PARITY_VERIFIED`；不是 source hash
錯誤，卻會造成 provenance claim過度解讀。相同問題存在於
`contracts/v1/transform_registry.tsv:2-13`：planned presentation/science
transform與已實作 transform無 lifecycle差別。

**封閉條件**：增加 closed lifecycle enum、target kind、target digest nullable
規則、verified evidence ID；governance強制 status與 target存在／digest／phase
evidence一致。

### AUD-P01-004 — catalog 與周邊 registries 尚未形成單一 machine binding

**證據**：手動 replay顯示 `contracts/v1/artifact_roles.tsv:2-18` 與 catalog
目前 `checked=17, mismatches=0`；但 `apps/governance_main.cpp:1326-1513` 的
catalog check沒有讀 artifact roles、transform registry或 query operators，
也沒有在 catalog/manifest中 SHA-lock這三份 registry 的 semantic content。

**影響**：兩個各自合法的 SoT 可獨立 drift；人類查到的 queryability、
producer role或 transform語意可能與 runtime catalog不同。

**封閉條件**：選一個 canonical source並產生其他 view，或將 registry SHA
納入 catalog/manifest binding，並加 exact row-set crosscheck與
missing/extra/changed negative fixtures。

### AUD-P01-005 — `canonical_json` 欄位沒有 value-level nested schema

**證據**：

- `cooccurrence_pairs.record.json:94-95` 的 group counts；
- `cooccurrence_pairs.record.json:151-152` 的 compatible relation models；
- `cooccurrence_sites.record.json:53-55` 的 joint partner orders；

都只宣告 `type=canonical_json`。`type_registry.tsv:16` 只約束無 duplicate key
及 insignificant whitespace，沒有物件／陣列 shape、key vocabulary、元素
type或與 companion count的 equality。

**影響**：一個語法 canonical、科學結構錯誤的 JSON string仍可通過 schema
shape；嵌套內容也不易精確 query。

**封閉條件**：每個 embedded JSON欄位綁 stable nested schema ID＋SHA，或正規化
為 child artifact；producer與 independent validator需重建 companion count/
digest並有 malformed/unknown/duplicate/order fixture。

### AUD-P01-006 — serialized Interval0 仍以兩個裸 uint64 表示

**證據**：C++ value type在 `types.hpp:37-67` 正確拒絕 `end<=begin`，但
`schema/records/site_reads.record.json:42-43` 將 `start0`、`end0` 各自標成
`uint64`，只在 `:60-65` free-text invariant寫 `end0 > start0`。

**影響**：schema consumer看不到兩欄共同構成 Interval0；標準 tabular validator
可接受空／反向 interval。

**封閉條件**：record schema增加 machine-readable semantic group，例如
`interval_groups:[{type:"Interval0",start:"start0",end:"end0"}]`，governance、
writer與 validator執行 cross-field check；加入 equal/reversed boundary
negative fixtures。

### AUD-P01-007 — gate registry 的 negative fixture 只是非空標籤

**證據**：`governance/gate_registry.tsv:2-24` 每 gate都有 negative fixture，
但 `apps/governance_main.cpp:1530-1604` 只檢查欄位非空與 command vocabulary；
`scripts/ci/check_gate_test_coverage.sh:15-30` 只對 `ctest -R` 數 test，
沒有驗標籤解析到 fixture/test，非 CTest command也沒有執行 coverage mapping。

**影響**：registry可以宣告不存在或從未執行的 negative fixture仍 PASS。

**封閉條件**：negative fixture改為 repository-relative path或 stable test ID；
governance要求存在、唯一、能被 command列出；每個 record schema至少覆蓋
unknown field、wrong type、illegal null/const、duplicate key、out-of-order與
truncated physical stream。

### AUD-P01-008 — multi-agent task record缺 ownership／write-set／lease

**證據**：`governance/agent_task.schema.json:6-21,23-98` 沒有 owner、allowed
paths、dependencies、lease、heartbeat或 parent task；目前
`state/tasks/active/20260719-foundation.json` 是一筆涵蓋 P0-P2 的 broad task。

**影響**：多 agent可同時合法修改相同檔案，machine governance無法偵測 stale
worker、重疊 write-set或未滿足 dependency；本次曾由 cross-build scratch
collision暴露並修正一個同類 concurrency風險。

**封閉條件**：在開始 P3-P6 parallel port前，task schema加入
`owner_agent_id`、`parent_task_id`、`allowed_paths`、`depends_on`、
`lease_expires_at`、`heartbeat_at`與 completion evidence；governance拒絕
overlapping active write-set與 expired lease。

### AUD-P01-009 — audit snapshot 缺 machine-readable supersession

**證據**：`docs/audits/20260719_runtime_solver_audit.md:233-244` 正確保留「當次
11/17 PASS」的歷史片段，fresh snapshot已是 23/23；文件雖有自然語言時間與
範圍，但沒有 `snapshot_id`、source tree digest、`captured_at`、
`superseded_by` machine fields。

**影響**：以全文搜尋查「full CTest」會同時得到舊、新結果；若 audit被加入
phase evidence而未綁 digest/時間，容易誤用。

**封閉條件**：定義 audit evidence envelope（snapshot ID、task ID、scope、
source commit/tree digest、command、exit、captured_at、supersedes/superseded_by）；
phase ledger只引用 immutable digest，不覆寫歷史。

### AUD-P01-010 — P1 尚有明列的 production closure 缺口

**證據**：`docs/audits/20260719_typed_io_audit.md:24-29,117-133` 明確保留：
cross-input contig dictionary closure、全檔 BAM/sidecar scan、runtime
duplicate collapse、pre/post full input rehash、durable real/frozen goldens與
7-dataset run未完成。

**封閉條件**：把 ephemeral indexed HTS smoke轉成 repo內可重生 synthetic
fixture；增加 BAM/VCF/sidecar/FASTA dictionary equality與全檔 scan；完成
duplicate equivalence contract；release run前後 SHA相等才可將 P1升級。

### AUD-P01-011 — format workflow 已接線，但此 snapshot 尚未通過獨立 format gate

**證據**：`.github/workflows/ci.yml:75-77` 會執行
`scripts/ci/check_format.sh clang-format-14`；
`scripts/ci/check_format.sh:9-25` 要求 pinned formatter並以
`--dry-run --Werror` 檢查全部 C++。但 `scripts/ci/check_all.sh:9-25` 不包含
format，因此本次兩個 `FOUNDATION_PASS` 不能證明 format PASS。本稽核環境缺
`clang-format-14`，直接 replay exit `1` 並輸出
`FORMAT FAIL: required formatter is unavailable`；主線 integration owner另已
識別現有 source需純機械 format。

**影響**：若現在推送，GitHub CI的 gcc-debug format step會在 build前阻止
merge；這不是 scientific mismatch，但屬開發流程 hard gate。

**封閉條件**：由主線用 pinned clang-format 14做純機械 rewrite，review diff
確認無語意變更，再重跑 Debug/Release、sanitizer、format與兩個
`check_all`。本文件的 C++ line citation屬 format前 snapshot；格式化後須以
symbol/path為主或重新確認行號。

## P2 blockers：reader、writer 與 filesystem transaction

### AUD-P2-001 — block reader／streaming reorder／worker matrix未完成

`state/phase_ledger.json:42-61` 已誠實列
`BLOCK_READER_NOT_IMPLEMENTED`、
`STREAMING_REORDER_SINK_NOT_BYTE_BOUNDED`、
`WORKER_COUNT_MATRIX_1_2_4_24_40_NOT_REPLAYED`。不得以 ordered pool unit test
替代 production block reader、halo、一次 CIGAR projection、global permit與
40-worker semantic digest matrix。

### AUD-P2-002 — BGZF writer是 physical primitive，不是 schema-aware writer

**證據**：`src/artifact/bgzf_tsv_writer.cpp:138-195` 建立 preamble/digest；
`:201-269` 只驗 UTF-8/control、欄數並直接寫入 caller提供的 strings。它沒有
載入 record schema，也不執行 scalar parse、null、enum、const、SCI17、
primary-key uniqueness或 sort order。

**封閉條件**：以 catalog-bound schema serializer包住 low-level writer；
producer-side先驗 typed value，再由獨立 reader重解壓、重建 semantic stream、
key/order/index與 EOF。producer與 validator不得共享 validation kernel。

### AUD-P2-003 — run-state guard不是 receipt/freezer authority

**證據**：`src/artifact/run_state.cpp:53-92` 接受 booleans、digest-shaped strings
與 `atomic_rename_completed` flag，不自行開檔、重算 receipt/checksum或驗
filesystem rename。此實作是 transition primitive，不能構成 VALIDATED authority。

**封閉條件**：P6 validator重開所有 manifest inputs與 artifacts，驗完整 receipt
graph後寫 validation/run receipt；同一 executable執行 atomic rename並在 final
root重播一次。fault injection需涵蓋偽 digest、symlink、cross-device rename、
partial BGZF與 pre-existing final root。

## P3-P5 protected science blockers

1. **P3**：`phase_ledger.json:64-74` 仍缺 frozen M1 logical/RNG vectors與
   HP-family versioned mapping；foundation test不可取代 PCG64 consumption、
   linkage tie、relabel與真實 golden parity。
2. **P4**：`:77-88` 仍缺 formal full co-occurrence authority、Endpoint-B O/X
   callability precedence vectors與 HP-family mapping；目前 16-cell schema
   只證明 O/X 有被記錄，不證明 exact test/FDR/permutation parity。
3. **P5**：`:91-103` 仍缺 real dual pilot、direct HiGHS、winner tie vectors與
   per-unit evidence membership vectors；q≤4 exhaustive oracle不是 production
   topology authority。
4. `topology_unit.schema.json:305-315` 的 candidate count、tree count、
   evidence membership與 winner resolution，以及
   `query_response.schema.json:223-234` 的 returned rows、canonical order與
   receipt chain，都是 `x-longlineage-invariants`。本次刻意把 valid topology
   fixture的 `candidate_count` 改成與 candidates length不一致、把 valid query
   fixture的 `returned_rows` 改成與 records length不一致；標準 JSON Schema
   兩者仍 exit `0`。這是 annotation的預期行為，但證明 P5/P6 C++ semantic
   validator不可省略。
5. `candidate_family_digest` 與 `vertex_set_sha256` 尚缺各自明確的 canonical
   byte-stream定義與 executable vectors。通用 JSON artifact semantic SHA
   不能自動回答這兩個 field-level digest究竟綁哪些 bytes。

**P5封閉條件**：定義 field-level digest framing／ordering；C++ producer與
獨立 brute-force validator各自重建；加入 candidate_count mismatch、
tree-sum mismatch、unresolved winner digest、tied winner、incomplete winner與
evidence membership mismatch fixtures，再跑 33 tails與 H2009/H1437 stress
panel。

## P6 blockers：validator、receipt replay 與查詢

### AUD-P6-001 — validator尚未實作，strict coverage正確失敗

`apps/validate_main.cpp:31-36` 永遠回 validation failure且不寫 receipt；
`state/phase_ledger.json:135-139` 明列 replay/fault/query/legacy blockers。
fresh `check_gate_test_coverage --strict` 只報
`VALIDATOR_FAULT_INJECTION` missing=1並 exit `1`。這是正確 hard stop。

### AUD-P6-002 — query目前只檢查兩個 receipt欄位

`apps/cli_support.cpp:507-522` 只確認 `state=VALIDATED_FROZEN` 與
`truth_fields_seen=0`；尚未重播 run/producer/validation receipt SHA、
checksums、catalog、artifact/index SHA與 semantic digest。
`apps/query_main.cpp:56-59` 隨後拒絕所有 query，因此目前不會洩漏 row，但也
不能宣稱 query功能存在。

### AUD-P6-003 — schema-level membership PASS不等於 runtime run closure

Catalog的 closed DAG已通過，但 P6仍需針對每個實際 run驗：

- required artifact/index exactly once，missing/extra/duplicate為零；
- artifact catalog與 data lineage row set等於 scientific membership；
- physical與 semantic SHA、logical rows、primary range與 nested index一致；
- producer/checksum/validation/run receipt無 self-cycle且 immutable；
- failure不產生 PASS receipt，cap/deadline不產生 winner。

### AUD-P6-004 — exporter／evaluator／presentation transform需 lifecycle gate

Legacy exporter與 evaluator是分離 executable，但仍是 skeleton；presentation
transform registry已列出尚不存在的 builder。這些應使用
AUD-P01-003的 lifecycle status，避免「registry有列」被查詢端解讀成「可用」。

## P8／nice-to-have

1. Repo有 `.clang-tidy`，但沒有 clang-tidy gate。可在 P8前加 pinned
   clang-tidy，但不應以它取代 format、tests或 validator。
2. `scripts/ai/check_readiness.sh:132` 對外部 absolute build root仍前綴
   `LongLineage/`，會產生混合顯示；只影響可讀性，建議分辨 repo-relative與
   external build path。
3. PID-scoped scratch path已讓 concurrent Debug/Release CTest各 23/23；
   長期可再使用 CTest `RESOURCE_LOCK`或 build-specific scratch root，防 shared
   PID namespace下的碰撞。
4. P8 report builder必須把 source transform lifecycle與
   `VALIDATED_FROZEN` gate寫成 executable check，所有 chart-ready資料需有
   schema/digest/claim_id；不得由 Python aggregate或修補資料。

## Fresh replay：命令、輸入、輸出與實際結果

### 變數

以下變數由執行者顯式提供；本文件不保存私人 host path：

```bash
: "${REPO:?set repository root}"
: "${DEBUG_BUILD:?set fresh Debug build root}"
: "${RELEASE_BUILD:?set fresh Release build root}"
: "${SOURCE_REPO:?set source repository root}"
: "${CLOSEOUT_RECEIPT:?set closeout receipt path}"
```

### Configure／build

輸入：`${REPO}` source、pinned local HTSlib/Jansson/OpenSSL。
輸出：`${DEBUG_BUILD}`、`${RELEASE_BUILD}`。

```bash
/usr/bin/cmake -S "${REPO}" -B "${DEBUG_BUILD}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
  -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON
/usr/bin/cmake --build "${DEBUG_BUILD}" --parallel 4

/usr/bin/cmake -S "${REPO}" -B "${RELEASE_BUILD}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
  -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON
/usr/bin/cmake --build "${RELEASE_BUILD}" --parallel 4
```

四個 command exit code均為 `0`。實際 dependency片段：

```text
HTSlib: 1.18
Jansson: 2.13.1
OpenSSL: 3.0.2
GNU C++: 11.4.0
```

### Concurrent Debug／Release CTest

```bash
/usr/bin/ctest --test-dir "${DEBUG_BUILD}" --output-on-failure
/usr/bin/ctest --test-dir "${RELEASE_BUILD}" --output-on-failure
```

兩者同時啟動，exit code均為 `0`：

```text
Debug:   100% tests passed, 0 tests failed out of 23
Release: 100% tests passed, 0 tests failed out of 23
```

這個 replay驗證已修正的 cross-build BGZF scratch collision：
`tests/unit/test_runtime_solver.cpp:161-198` 使用 process-scoped prefix。
同時 `tests/unit/test_typed_io.cpp:28-33` 使用 always-on CHECK；對
`tests/ include/ src/ apps/` 搜尋 assert-based test為 `0` matches。

### Foundation governance

輸入：`${REPO}` 與兩個 fresh build。
輸出：stdout gate evidence；不寫 production artifact。

```bash
cd "${REPO}"
scripts/ci/check_all.sh "${DEBUG_BUILD}"
scripts/ci/check_all.sh "${RELEASE_BUILD}"
scripts/ai/check_readiness.sh "${DEBUG_BUILD}"
scripts/ci/check_gate_test_coverage.sh "${RELEASE_BUILD}" --strict
scripts/ci/check_release_gate.sh "${RELEASE_BUILD}"
```

| Command | Exit | 實際關鍵結果 |
|---|---:|---|
| Debug `check_all` | 0 | hygiene、boundary、SPDX、dependency、JSON Schema、compiled governance、23/23 CTest、no-network；`FOUNDATION_PASS` |
| Release `check_all` | 0 | 同上；`FOUNDATION_PASS` |
| readiness | 0 | failures=0、warnings=1；Git/private remote尚未驗證 |
| strict gate coverage | 1 | missing=1：`VALIDATOR_FAULT_INJECTION` |
| release gate | 1 | foundation通過後被 strict validator gate阻止；未產生 release PASS |

`check_all` 的非 strict模式顯示
`PASS_WITH_DECLARED_BLOCKERS missing=1`，因此 `FOUNDATION_PASS` 不能被解讀成
release gate PASS。`check_all` 也不呼叫 format；format狀態須獨立報告。

### 標準 JSON Schema 與 semantic-invariant boundary

```bash
cd "${REPO}"
scripts/ci/check_json_schema_fixtures.sh

jq '.candidate_count = 2' \
  tests/fixtures/contracts/topology_unit.valid_unique_winner.json |
  /usr/bin/jsonschema -i /dev/stdin \
  schema/records/topology_unit.schema.json

jq '.returned_rows = 0' \
  tests/fixtures/contracts/query_response.valid.json |
  /usr/bin/jsonschema -i /dev/stdin \
  schema/core/query_response.schema.json
```

fixture suite exit `0`，包括 invalid partial-count與 tied-winner rejection。
兩個刻意製造的 cross-field mismatch也 exit `0`，證明
`x-longlineage-invariants` 必須由 P5/P6 executable validator執行。

### Provenance source hash 與 closeout receipt

```bash
cd "${REPO}"
jq -r '.mappings[] | [.source_path,.source_sha256] | @tsv' \
  provenance/source_to_target_manifest.json |
while IFS=$'\t' read -r source_path expected; do
  relative="${source_path#InterSubMod/}"
  observed="$(sha256sum "${SOURCE_REPO}/${relative}" | awk '{print $1}')"
  test "${observed}" = "${expected}"
done

sha256sum "${CLOSEOUT_RECEIPT}"
jq '{dataset_count,all_pass,receipt_count:(.receipts|length),
     total_mapped_alignments}' "${CLOSEOUT_RECEIPT}"
```

實際結果：

```text
source hashes: matched=12 total=12
closeout sha256: d5360b7422ee1017d71f60ede51bc3283160e57c6abbb78daa3e74090362ab7c
dataset_count=7
all_pass=true
receipt_count=7
total_mapped_alignments=164253537
```

## Phase 結論與下一個合法 checkpoint

本次稽核**不變更 phase**：

- P0、P1、P2、P6：維持 `IN_PROGRESS`。
- P3、P4、P5、P7、P8：維持 `BLOCKED`。
- release attestation：維持 `NOT_READY`。

下一個合法 checkpoint不是「開始全量 science run」，而是：

1. 封閉 AUD-P01-001 至 011，特別是 state derivation、private remote、
   provenance lifecycle、nested schema、Interval0 machine invariant、
   negative-fixture binding、multi-agent ownership與 pinned format PASS。
2. 完成 schema-aware writer＋independent reader，並把 actual HTS smoke轉成
   durable synthetic fixture。
3. 實作 P6 independent validator與 fault-injection，使 strict gate從唯一
   missing test歸零。
4. 只有在 P3/P4/P5 frozen vectors、dual authority與 replayable evidence齊備後，
   才能依 predecessor chain逐 phase評估升級；任何 cap/deadline/tie仍不得產生
   winner。
