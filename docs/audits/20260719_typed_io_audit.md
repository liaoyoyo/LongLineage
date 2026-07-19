<!--
建立時間: 2026-07-19
目標: 稽核 P1 typed I/O、manifest input locks、HTS preflight、MM/ML 與 latest HP/PS exact join
處理範圍: C++ value types、ParseResult、BAM/VCF/sidecar/reference probe、typed auxiliary tags、資料紀錄與查詢契約
關聯檔案: LongLineage/include/longlineage/{common,io,manifest}/、LongLineage/src/{common,io,manifest}/、LongLineage/tests/unit/test_typed_io.cpp
-->

# P1 Typed I/O、資料紀錄與查詢稽核

## 結論與任務邊界

本次為 **B 類 bounded comprehensive component validation**，服務：

- **LL-G1**：以 typed C++ I/O 建立 LongLineage production authority 的入口。
- **LL-G4**：以 immutable input locks、closed schemas 與 exact join 支援可重現性。
- **LL-G5**：讓格式、reason、transform 與 provenance 可由機器查詢及獨立稽核。

P1 typed I/O foundation 已完成，並通過 C++17 Debug/Release
warnings-as-errors build、typed unit tests、actual HTS synthetic smoke、
ASan/UBSan、兩種 build 的 full CTest 與 governance check-all。Production
manifest 的 10 個 contract bindings、每個輸入的 byte size 與完整 SHA-256、
HTSlib 1.18、明示 index 與格式 probe 均採 fail-closed。

這些證據只支持 **P1 component foundation 已可整合，P1 phase 仍為
`IN_PROGRESS`**。尚未完成 cross-input contig dictionary closure、全檔 BAM/
sidecar scan、runtime block reader、production duplicate-collapse execution、
release 前後 input rehash、frozen real goldens 與 7-dataset full run。M1、M2、
P5、P6、P7 均不由本文件宣稱完成。Release attestation 維持 `NOT_READY` 是預期的
安全結果，不是 production release pass。

## Step → Verify

1. 建立禁止裸座標與模糊空集合的核心型別
   → 驗證：`Position1`、non-empty `Interval0`、`ContigId`、R/A/O/X 與
   `ParseResult<T>` 的 OK、OK_EMPTY、ERROR 狀態均有 synthetic regression。
2. 鎖定 production manifest 與輸入 identity
   → 驗證：closed-world manifest 必須含 8 個 dataset file roles、10 個
   contract SHA-256 bindings、absolute normalized non-symlink paths、byte size 與
   full SHA-256；修改 BAM digest 後 preflight exit code 為 `3`。
3. 建立 HTSlib fail-closed preflight
   → 驗證：BAM/BAI、BGZF VCF/CSI 或 TBI、sidecar/TBI、FASTA/FAI 均要求明示
   index；缺少任一 index 的 4/4 synthetic cases 全被拒。
4. 建立 sidecar raw vocabulary 與 exact join
   → 驗證：raw HP 只接受 `.` 與 8 個 authority states，`.` canonicalize 為
   `0`；raw `0` 被拒；PS 使用 optional uint64；synthetic alignment exact join
   得 HP/PS，conflict、multimatch 與 full-identity mismatch fail closed。
5. 建立 typed auxiliary tag identity
   → 驗證：scalar 與 B-array subtype 均保存；canonical ordering deterministic；
   RG 預設排除，其餘 typed tags 差異不可被誤 collapse。
6. 建立 frozen MM/ML 解析契約
   → 驗證：只允許一個 target `C+m?`，可有一個 `C+h?` 供 exact ML offset；
   reverse alignment 恢復 original as-sequenced orientation；ML bin 為
   `[raw/256,(raw+1)/256)`；forward、reverse、multi-group offset 均通過。
7. 執行 strict build 與 unit/sanitizer gate
   → 驗證：GCC Debug/Release、`-Wall -Wextra -Wpedantic -Werror` build exit
   `0`；兩種 build 的 typed 4/4 CTest、ASan/UBSan 均 PASS，Release tests
   不依賴會被 `NDEBUG` 移除的 `assert`。
8. 執行 actual indexed HTS smoke
   → 驗證：實際建立並讀取 BAM/BAI、VCF/CSI、sidecar/TBI、FASTA/FAI；
   1 個 alignment 與 1 個 sidecar row exact join，4/4 missing-index 與
   2/2 malformed-record cases 全被拒。
9. 執行 repo integration gate
   → 驗證：full CTest `23/23 PASS`；governance check-all 對 cold-start、
   policy、state、catalog、status、gate、truth boundary 全部 PASS；10-binding
   manifest CLI preflight exit `0`。
10. 保留 phase closure 缺口
    → 驗證：release attestation 仍為 `NOT_READY`；未完成項目列於
    「未封閉風險」，不得由 synthetic PASS 推導 production readiness。

## 資料格式與紀錄契約

### Raw、canonical 與 scientific meaning 分層

| 層 | 內容 | 紀錄規則 | 查詢方式 |
|---|---|---|---|
| Raw input | BAM、VCF、latest-tag sidecar、FASTA 與明示 indexes | path、role、byte size、SHA-256；不改寫來源 | production manifest 的 dataset file roles |
| Parsed value | typed coordinate、identity、aux tags、MM/ML、HP/PS | 每次解析回 `status + reason`；錯誤不可表示成空集合 | C++ headers 與 status/reason registry |
| Canonical value | raw HP `.` → canonical `0`；typed aux canonical order | transform 必須命名、版本化、不可覆蓋 raw token | transform registry 與 schema catalog |
| Scientific artifact | packed BGZF/JSON artifacts | schema ID、schema SHA、logical identity、producer receipt | schema ID registry、catalog 與 run receipt |
| Frozen evidence | checksums、validation receipt、release attestation | 只允許經 validator 的 lifecycle transition | run state、checksums 與 attestation evidence |

`O` 與 `X` 是不同 allele states，均不可併入 `R`。Raw token 與 canonical value
也不可混為同一欄位；例如 sidecar raw HP `.` 是合法的 authority token，
canonical `0` 是下游 typed representation，而 raw `0` 本身必須拒絕。

### Production manifest closure

每個 dataset 必須精確提供 8 個 file roles；manifest 額外綁定以下 10 個
lowercase SHA-256：

1. science parameters
2. schema catalog
3. status/reason registry
4. type registry
5. transform registry
6. authority manifest
7. source-to-target manifest
8. production input authority
9. schema ID registry
10. release attestation

缺少、增加或替換未知欄位均拒絕。Input path 必須是 absolute normalized path，
任何已存在的 symlink component 均拒絕；staging root 必須精確對應
`.staging/<run_id>`。Release run 不能從可變 phase ledger 推導 readiness，只能
驗證 manifest-bound release attestation。

### BAM、VCF、sidecar 與 reference

- BAM 必須是實體 BAM，並提供可讀且可 query 的明示 BAI/CSI。
- VCF 必須是 BGZF text，提供明示 Tabix/CSI；record 全掃描時要求 sorted、
  `PASS`、biallelic A/C/G/T、無 exact duplicate。
- Sidecar header、欄位型別、raw HP vocabulary、PS uint64 與 index fetch 均
  fail closed。
- FASTA 必須提供明示 FAI，並可讀出 contig dictionary。
- HTSlib production version 必須精確為 `1.18`。

目前各檔案的 contig dictionary 是個別讀取與驗證；跨 BAM、VCF、sidecar、
FASTA 的 dictionary equality 尚未封閉，因此不能宣稱完整 input compatibility。

### Alignment identity 與 latest-tag join

`ReadProjectionIdentity` 保存 QNAME、contig、start/end、MAPQ、strand。
`FullAlignmentIdentity` 再加入 FLAG、CIGAR 與 typed auxiliary tags。Typed
B-array 必須保留 subtype；RG 可為 RG-only repeated occurrence 計數而排除於
canonical comparison，其餘 SAM core 或 typed auxiliary 差異均 fail closed。

Join reason 分離為 missing、conflict、multimatch 與
`FULL_IDENTITY_MISMATCH`，不得以「沒有資料」代替 identity error。PS 為
`optional<uint64_t>`；negative 與 overflow 拒絕，uint64 最大值可表示。

目前已具備 typed identity primitive 與 exact-join component；完整 duplicate
collapse 執行尚未納入 runtime，SAM core 的 mate fields、TLEN、SEQ、QUAL
等 full equivalence 尚未完成。

### MM/ML frozen semantics

- MM/ML/MN tag 名稱必須為 uppercase。
- ML 必須是 `B:C`，MN 必須與 sequence length 相符。
- Target group 精確為 `C+m?`；最多一個 `C+h?` 僅用於計算 ML offset。
- 每條 read 的 MM event count 與 ML cardinality 必須完全一致。
- Reverse-aligned BAM sequence 先還原成 original as-sequenced orientation，
  再依 cytosine ordinal 解析。
- ML raw byte `x` 對應 frozen interval `[x/256,(x+1)/256)`。
- `?` 與 `.` 的 missing-modification semantics 不得互換。

知識來源為 `kb-03-file-formats-bam-format`，last verified
`2026-07-12`；本次只採其 as-sequenced MM orientation、ML bins、MN 與
`?`/`.` 格式語意，不讓知識文件取代 frozen vector parity。

## Reason、schema 與查詢紀律

### Reason 不得兼作資料

所有 parser 都必須回傳：

```text
value | status | reason
```

`OK_EMPTY` 只表示成功解析且集合確實為零；`ERROR` 必須有穩定 reason。
新增 reason 時必須先登錄 status/reason registry，再加入 schema/test；
不得以 free-text message 取代 machine-readable reason。

### Schema 查詢入口

實作者與 AI 應按下列順序查詢，不應從範例 row 反推格式：

1. `schema/id_registry.json`：由 stable schema ID 定位 record schema。
2. `schema/catalog.json`：查 artifact membership、record schema SHA 與 index。
3. 對應 JSON Schema：查欄位型別、required、enum 與 closed properties。
4. status/reason、type、transform registries：查錯誤、typed value 與轉換定義。
5. run receipt 與 checksums：查該次 frozen artifact identity。

Catalog、ID registry 與每個 record schema 以 SHA-256 相互鎖定；governance
check-all 已驗證 exact membership，而不是只檢查檔案存在。

### AI 與開發紀律

- 開始修改前先讀 `AGENTS.md`、`docs/CURRENT_FOCUS.md`、architecture decision
  與相關 schema/registry，不從聊天記憶猜測契約。
- 先新增或鎖定 schema、reason、transform，再修改 producer/consumer。
- 每個科學判斷必須由 C++ authority 產生；Python 只能讀
  `VALIDATED_FROZEN` artifact 做 presentation。
- Synthetic、probe、partial artifact 必須明示 scope，不能成為 production
  validation evidence。
- 每個執行紀錄必須含 input、完整 command、output、exit code 與實際輸出片段。
- 任何 worker/parse/index/hash 錯誤均停止 publication，不得留下偽 PASS receipt。
- 變更完成須先跑 targeted tests，再跑 full CTest 與 governance check-all；
  未通過的 gate 必須寫為 blocker，不能以敘述性結論覆蓋。

## 執行與驗證證據

### Input / command / output

輸入 source：

- `LongLineage/include/longlineage/common/`
- `LongLineage/include/longlineage/io/`
- `LongLineage/include/longlineage/manifest/`
- `LongLineage/src/common/`
- `LongLineage/src/io/`
- `LongLineage/src/manifest/`
- `LongLineage/tests/unit/test_typed_io.cpp`

Strict direct compile：

```bash
/usr/bin/g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -I/usr/local/include \
  tests/unit/test_typed_io.cpp \
  src/common/digest.cpp src/io/alignment.cpp src/io/mm_ml.cpp \
  src/io/sidecar.cpp src/io/hts_preflight.cpp \
  src/manifest/production_manifest.cpp \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  -lhts -ljansson -lcrypto -pthread \
  -o "${TMPDIR}/longlineage_test_typed_io_snapshot"
"${TMPDIR}/longlineage_test_typed_io_snapshot"
```

輸出：`${TMPDIR}/longlineage_test_typed_io_snapshot`；exit code `0`。實際片段：

```text
typed_io: PASS
```

Configure/build：

```bash
/usr/bin/cmake -S "${REPO}" -B "${BUILD_DEBUG}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
  -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON
/usr/bin/cmake --build "${BUILD_DEBUG}" --parallel 4

/usr/bin/cmake -S "${REPO}" -B "${BUILD_RELEASE}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON \
  -DLONGLINEAGE_WARNINGS_AS_ERRORS=ON
/usr/bin/cmake --build "${BUILD_RELEASE}" --parallel 4
```

輸出：`${BUILD_DEBUG}/`、`${BUILD_RELEASE}/`；四個 command exit code 均為
`0`。實際 dependency 片段：

```text
HTSlib: 1.18
Jansson: 2.13.1
OpenSSL: 3.0.2
```

### Targeted tests 與 sanitizer

Typed CTest 為 `4/4 PASS`。ASan/UBSan 使用：

```bash
/usr/bin/g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude -I/usr/local/include \
  tests/unit/test_typed_io.cpp \
  src/common/digest.cpp src/io/alignment.cpp src/io/mm_ml.cpp \
  src/io/sidecar.cpp src/io/hts_preflight.cpp \
  src/manifest/production_manifest.cpp \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  -lhts -ljansson -lcrypto -pthread \
  -o "${TMPDIR}/longlineage_test_typed_io_snapshot_asan"
ASAN_OPTIONS=detect_leaks=0 \
  "${TMPDIR}/longlineage_test_typed_io_snapshot_asan"
```

exit code `0`；實際片段：

```text
typed_io: PASS
```

LeakSanitizer 在目前受 ptrace 管理環境停用，因此不宣稱 leak gate 通過。

### Actual indexed HTS smoke

Synthetic inputs：

- `${TMPDIR}/longlineage_p1_smoke.bam` + `.bai`
- `${TMPDIR}/longlineage_p1_smoke.vcf.gz` + `.csi`
- `${TMPDIR}/longlineage_p1_smoke.sidecar.tsv.gz` + `.tbi`
- `${TMPDIR}/longlineage_p1_smoke.fa` + `.fai`

Compile 與 smoke command：

```bash
/usr/bin/g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -I/usr/local/include \
  "${TMPDIR}/longlineage_p1_preflight_smoke.cpp" \
  src/common/digest.cpp src/io/alignment.cpp \
  src/io/sidecar.cpp src/io/hts_preflight.cpp \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  -lhts -lcrypto -pthread \
  -o "${TMPDIR}/longlineage_p1_preflight_smoke_snapshot"
"${TMPDIR}/longlineage_p1_preflight_smoke_snapshot"
```

exit code `0`；實際輸出：

```text
bam_contigs=1 vcf_records=1 sidecar_contigs=1 reference_contigs=1
fetched=1 joined_hp=1 joined_ps=42
missing_indexes=4/4_REJECTED malformed_records=2/2_REJECTED
```

此 smoke source 位於 ephemeral `${TMPDIR}`，是 component integration evidence，
不是 repo 內 durable golden；P1 phase closure 前須轉成可重生 fixture。

### Manifest CLI integration

輸入：`${TMPDIR}/longlineage_p1_manifest.json`，包含 10 個 contract bindings
與 synthetic inputs 的實際 byte size/SHA-256。

```bash
"${BUILD_DEBUG}/bin/longlineage" preflight \
  --manifest "${TMPDIR}/longlineage_p1_manifest.json" \
  --repo "${REPO}"
```

exit code `0`；實際輸出：

```json
{"command":"preflight","status":"PASS","message":"manifest, ten contract bindings, all input locks and indexed HTS probes passed"}
```

將 manifest 內 BAM SHA-256 修改一個 hex digit 後：

```bash
"${BUILD_DEBUG}/bin/longlineage" preflight \
  --manifest "${TMPDIR}/longlineage_p1_manifest_tampered.json" \
  --repo "${REPO}"
```

exit code 為 `3`，實際輸出：

```json
{"command":"preflight","status":"ERROR","exit_code":3,"message":"synthetic: Locked input SHA-256 mismatch for ${TMPDIR}/longlineage_p1_smoke.bam"}
```

### Full repo gates

```bash
/usr/bin/ctest --test-dir "${BUILD_DEBUG}" --output-on-failure
/usr/bin/ctest --test-dir "${BUILD_RELEASE}" --output-on-failure
```

兩個 command exit code 均為 `0`；實際結果：

```text
Debug:   100% tests passed, 0 tests failed out of 23
Release: 100% tests passed, 0 tests failed out of 23
```

```bash
"${BUILD_DEBUG}/bin/longlineage-governance" check-all --repo "${REPO}"
```

exit code `0`；實際結果涵蓋：

```text
PASS cold-start
PASS policy
PASS state
PASS catalog
PASS status codes
PASS gates
PASS truth boundary
```

目前 catalog 為 16 個 offline schema IDs 與 16 個 artifact entries，status
registry 87 rows、gate registry 23 rows；此數量是本次 integration snapshot，
查詢時仍應以 hash-locked registry 為權威。

## 未封閉風險

1. 尚未驗證 BAM、VCF、sidecar、reference 的 cross-input contig dictionary
   exact compatibility。
2. BAM preflight 目前是 header/index probe，sidecar 是 indexed sample fetch；
   尚未 full-scan 每筆 BAM/sidecar record。
3. Runtime block reader 尚未實作，因此「每個 block 解壓一次、每條 read
   CIGAR/MM/ML 各解析一次」尚無 production evidence。
4. Duplicate collapse 尚未串入 production execution；full SAM core
   equivalence 尚未封閉。
5. Release run 的 before/after full input SHA-256、attestation snapshot 與
   freeze 前重新驗證尚未接線；`run` 因 `NOT_READY` 正確拒絕。
6. Frozen real/golden CIGAR、MM/ML parity，以及 malformed/sorted/duplicate VCF
   的完整向量仍待補。
7. 尚未讀取 7 個真實 datasets，更未執行 24/40-worker full run。
8. 本環境缺 Clang/clang-format；本次只由 GCC 驗證。LeakSanitizer 因環境限制
   停用。

任何上述項目未關閉前，不得把 P1 標成 `VERIFIED`，也不得進一步宣稱 M1/M2、
topology、validator 或 full production release 已通過。

## Changed files

- `LongLineage/include/longlineage/common/parse_result.hpp`
- `LongLineage/include/longlineage/common/types.hpp`
- `LongLineage/include/longlineage/common/digest.hpp`
- `LongLineage/src/common/digest.cpp`
- `LongLineage/include/longlineage/io/alignment.hpp`
- `LongLineage/src/io/alignment.cpp`
- `LongLineage/include/longlineage/io/mm_ml.hpp`
- `LongLineage/src/io/mm_ml.cpp`
- `LongLineage/include/longlineage/io/sidecar.hpp`
- `LongLineage/src/io/sidecar.cpp`
- `LongLineage/include/longlineage/io/hts_preflight.hpp`
- `LongLineage/src/io/hts_preflight.cpp`
- `LongLineage/include/longlineage/manifest/production_manifest.hpp`
- `LongLineage/src/manifest/production_manifest.cpp`
- `LongLineage/tests/unit/test_typed_io.cpp`
- `LongLineage/docs/audits/20260719_typed_io_audit.md`

所有新增 C++ source/header/test 首行均含
`SPDX-License-Identifier: GPL-3.0-only`。
