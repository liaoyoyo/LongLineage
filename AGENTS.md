# AGENTS.md — LongLineage Repository Governance

> 本檔是所有 AI agent 與人類貢獻者的最高層 repo governance。若子目錄另有
> `AGENTS.md`，較具體路徑規則優先；任何規則都不得放寬本檔的 truth isolation、
> provenance、validation 或 claim ceiling。

## 0. Cold-start 五問

新 session 在修改任何檔案前，必須能從 repo 回答：

1. **這是什麼專案？**
   `LongLineage/README.md`、`LongLineage/docs/claims/CLAIM_BOUNDARY.md`
2. **目前做到哪裡？**
   `LongLineage/docs/CURRENT_FOCUS.md`、`LongLineage/ROADMAP.md`
3. **怎麼 build/test？**
   `LongLineage/docs/development/WORKFLOW.md`
4. **怎麼驗證科學與資料變更？**
   `LongLineage/docs/release/RELEASE_GATES.md`、`LongLineage/schema/catalog.json`
5. **最近做了哪些決定？**
   `git log --oneline -10`、`LongLineage/docs/decisions/`、
   `LongLineage/docs/development/implementation-notes.md`

任一題答不出時，先補 source of truth，不得靠外部記憶繼續。

## 1. 語言、程式與回應

- 對使用者回應與研究文件使用繁體中文（zh-TW）；API/schema/CLI identifier 使用英文。
- 程式碼註解使用英文。
- C++17；namespace `longlineage`；headers `.hpp`；class/type用CamelCase，
  function/method/file用snake_case；4-space indent；120 columns。
- 中等以上工作首句先給結論、task type、影響與信心。
- 多步驟工作使用 `Step → Verify`，verify必須是可觀察值、退出碼、digest或明確status。
- 執行命令時記錄完整 input path、command、output path、exit code與實際輸出片段。

## 2. 目標與不可越界主張

每個工作項必須至少服務一個目標：

- **LL-G1**：truth-isolated、deterministic的long-read資料處理。
- **LL-G2**：可驗證的read-level methylation與sSNV co-occurrence。
- **LL-G3**：exact、可證明完整性或誠實abstain的mutation-state family求解。
- **LL-G4**：跨資料集可重現、可獨立驗證的資料與軟體工程。
- **LL-G5**：可供外部稽核、重建與引用的業界級交付。

正式claim ceiling：

> long-read sSNV co-occurrence and lineage-compatible mutation-state families

不得宣稱真實clone、cellular ancestry、祖先、時間順序、唯一演化樹或完整cell lineage。

## 3. Task type gate

對話或工作啟動先分類：

| 類型 | 預設scope | 交付義務 |
|---|---|---|
| A Exploratory pilot | bounded subset | `PARTIAL`標記、限制、後續full plan |
| B Comprehensive validation | 全scope | 完整evidence chain與phase ledger |
| C Production/release | 全scope＋pinned container | release gates、smoke、notes |
| D External handoff | 全scope＋sanitized docs | scope、限制、reviewer format |
| E Hotfix/bugfix | minimal reproducer | root cause、regression、invalidation |
| F Demo | synthetic only | `DEMO`標記；不可作validation evidence |

`run`屬B/C；`probe`只能屬A/F並強制輸出`PARTIAL`。

## 4. Production trust boundary

### 4.1 Production authority

`longlineage preflight/run`只可讀：

- raw BAM：SEQ、CIGAR、QUAL、MM/ML/MN與必要SAM core/typed aux。
- frozen PASS biallelic sSNV VCF。
- 7/7 raw-all production sidecar的latest HP/PS。
- reference FASTA與必要索引。

Production manifest與CLI不得接受：

- truth BED、truth VCF、truth labels或truth-derived selection。
- 其他HP/PS sidecar、BAM舊HP fallback或推測tag。
- posthoc benchmark結果。

truth-aware工作只可由獨立`longlineage-evaluate`讀取
`VALIDATED_FROZEN` run後執行；evaluation不得寫回production artifacts。

### 4.2 Python boundary

Python不得：

- 讀BAM/VCF/sidecar進行科學分析。
- 重算cluster、p-value、FDR、candidate、topology或summary。
- 補值、改分類、改分母或挑選結果。
- 在本repo執行歷史Python science authority。

Python只可存在於`LongLineage/presentation/`，且只能把C++產生的
validated chart-ready資料映射成圖片與standalone HTML。

## 5. Phase ledger與完成定義

固定phase：

1. P0 authority/provenance
2. P1 typed I/O/preflight
3. P2 block reader/threading/packed writer
4. P3 M1 parity
5. P4 M2/co-occurrence parity
6. P5 topology parity
7. P6 independent validator/export/query
8. P7 7-dataset 24/40-worker full validation
9. P8 validated-only report/release candidate

每phase狀態只能是：

`NOT_STARTED → IN_PROGRESS → VERIFIED`，或
`NOT_STARTED/IN_PROGRESS → BLOCKED/FAILED`。

- 只有validator產生的receipt可把run升為`VALIDATED`。
- 只有atomic freeze完成後可標`VALIDATED_FROZEN`。
- scaffold、compile、unit test或partial pilot都不等於scientific parity。
- cap/deadline/incomplete family不得產生winner。

## 6. 資料紀錄與格式契約

`LongLineage/schema/catalog.json`是artifact registry的machine-readable SoT。
每個正式artifact都必須定義：

- `schema_name`與SemVer `schema_version`。
- physical format、compression、required header。
- primary key、唯一性、排序、coordinate convention。
- 每欄type、nullability、units、enum或reason-code vocabulary。
- numerator/denominator definition（若為metric）。
- conservation rules與semantic digest canonicalization。
- producer、independent validator與query support。

規則：

- genomic position使用`Position1`；half-open interval使用`Interval0`；禁止裸整數混用。
- allele call固定`R/A/O/X`；O與X不得合併成REF。
- 空集合不得同時表示「零資料」與「解析錯誤」。
- unknown enum/field、duplicate key、out-of-order record與malformed row預設fail closed。
- JSON object canonical digest使用schema-defined key order；未預先宣告的map key以
  UTF-8 bytes升冪排序，並使用明確number/string規則。
- TSV/JSONL semantic digest忽略BGZF block與mtime，只依schema-defined logical records。
- 每個run保留`checksums.sha256`與`semantic_digests.tsv`。
- schema major變更必須有ADR、migration與backward-negative test。

## 7. 查詢紀律

- 正式查詢入口是`longlineage-query`，只接受`VALIDATED_FROZEN` run。
- 查詢前重驗receipt、artifact SHA、schema version與catalog binding。
- query預設read-only；不得改檔、聚合新科學結論或隱式補值。
- 查詢結果必列：run_id、artifact、schema、filter、matched rows、truncated flag。
- 精確key查詢優先；全表scan需明示並有row limit。
- 人類查詢範例與欄位語意見`LongLineage/docs/data/QUERY_GUIDE.md`。

## 8. 開發流程

1. 分類task type與服務目標。
2. 讀`CURRENT_FOCUS`、相關ADR、schema與tests。
3. 列假設、可能解讀、最簡替代方案與Step→Verify。
4. 新scientific spec先更新authority manifest與implementation notes。
5. 由`.ai/templates/task.json`建立`state/tasks/active/` machine-readable work record。
6. 先寫negative/regression fixture，再實作。
7. 執行format、build、unit、integration、determinism、schema與hygiene gates。
8. 獨立validator不得連結producer statistical/solver kernels。
9. 更新CURRENT_FOCUS、CHANGELOG與phase ledger；未驗證項保持BLOCKED。

禁止：

- `-ffast-math`、release `-march=native`。
- OpenMP nested parallelism；只用一個bounded thread pool。
- 用wall-clock、unordered iteration或thread order影響RNG/record order。
- 以asymptotic fallback取代明定的exact test。
- 忽略failed task、高深度、K=11、tail或resource error。

## 9. Git與repo hygiene

- 唯一永久branch為`main`；短期使用`feat/*`、`fix/*`、`docs/*`。
- commit需單一意圖；禁止把真實data/output與程式修改混在同一commit。
- Git/Git LFS禁止：真實BAM/VCF/sidecar、真實基因座、run outputs、credentials、tokens。
- 只可提交可重生synthetic fixtures。
- 大於1 MiB的新檔預設fail；必要例外需ADR＋allowlist。
- 不刪除研究/證據檔；移至`LongLineage/docs/archive/YYYY/MM/`並留redirect。
- 不得reset/覆寫他人dirty worktree。
- 公開visibility前需使用者明確授權與license/source-origin audit PASS。

## 10. Build/Test標準入口

```bash
/usr/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLONGLINEAGE_BUILD_TESTS=ON
/usr/bin/cmake --build build -j4
ctest --test-dir build --output-on-failure
scripts/ci/check_all.sh
```

Release：

```bash
/usr/bin/cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON
/usr/bin/cmake --build build-release -j4
ctest --test-dir build-release --output-on-failure
scripts/ci/check_release_gate.sh build-release
```

完整開發與驗證說明見`LongLineage/docs/development/WORKFLOW.md`。

## 11. Multi-agent與living documentation

- 子任務開始前記錄input、expected output、verification。
- 子agent寫入前，先建立自己的`state/tasks/active/*.json`、owner、parent、
  dependency、非重疊write-set與有效lease；parent先縮小或釋放重疊claim。
- 不得在工作完成後回填或倒填lease來掩蓋未授權的並行寫入；若bootstrap期間
  發生，記為deviation／incident並從下一次寫入開始執行新規則。
- 子任務完成回報具體path、command、exit、digest與限制。
- 每個active spec只維護一份
  `LongLineage/docs/development/implementation-notes.md`。
- 設計決定、偏離、折衷、未決與gotcha需即時append。
- AI不得自行把`BLOCKED`改成`VERIFIED`；需可重播證據。

## 12. 回應與交付

final handoff必須列：

- 已完成與未完成phase。
- 輸入、命令、輸出、實際退出碼/片段。
- changed files。
- validation evidence與remaining blockers。
- 不得用「完成」涵蓋尚未通過的全量或科學gate。
