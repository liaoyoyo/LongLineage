---
title: 三專案佈局與使用流程規劃（待確認）
date: 2026-08-07
status: 規劃 — 待使用者確認後執行遷移
---

# 三專案佈局規劃

## 0. 你的構想（我的理解）

> LongLineage 為主，改造成獨立可上傳的程式，輸出 tagged BAM；
> ISM 是獨立專案，分析「有標籤 + 有甲基」的數據，得出標籤與數據的關係與結論；
> 另有 Python 整合這些數據與結論，輸出觀察 HTML 顯示樣本分析數據。

**我確認這個架構是對的，而且比我目前的做法正確。**

## 1. 現況：位置錯了

我目前把 3 支新 C++ 寫在 `InterSubMod/pipeline/lineage/cpp/`：

| 檔案 | 現在位置 | **應該在** |
|---|---|---|
| `lineage_paths.cpp` | InterSubMod | **LongLineage** |
| `read_assign.cpp` | InterSubMod | **LongLineage** |
| `tag_bam.cpp` | InterSubMod | **LongLineage** |
| `build.sh` / `run_sample.sh` | InterSubMod | **LongLineage** |
| `build_report.py` | InterSubMod | **整合層（第三個位置）** |
| `verify.py` / `freeze_baseline.py` | InterSubMod | **LongLineage** |

**當初放這裡的理由**：怕撞 LongLineage 的 fail-closed governance。
**重新評估後**：這 3 支是**獨立 executable**，不進 production run 的 9-artifact catalog，
所以 **不需要**同步 `artifact_validator.cpp`、**不需要**改 contract SHA。
遷移成本比我原先估計的低很多。

## 2. 目標佈局

```
LongLineage/                        ← 主專案：從 BAM 到 tagged BAM
├── apps/
│   ├── longlineage_main.cpp        （既有）
│   ├── lineage_paths_main.cpp      ★ 移入
│   ├── read_assign_main.cpp        ★ 移入
│   └── tag_bam_main.cpp            ★ 移入
├── src/linkage/                    ★ 未來：extraction / strict graph 的 C++ 化
├── scripts/
│   └── run_sample.sh               ★ 移入（端到端 driver）
├── presentation/                   （既有，Python 只能在這裡）
└── docs/lineage/                   ★ 移入規格文件

InterSubMod/                        ← 獨立專案：甲基 × 標籤分析
├── src/core/ReadParser.cpp         （已改：讀 lc/lu/lv/lp/lo/ls）
├── include/core/DataStructs.hpp    （已改：ReadInfo +7 欄）
├── include/core/Config.hpp         （已改：group_by_tag 多軸）
└── src/io/RegionWriter.cpp         （已改：reads.tsv +7 欄）
   輸入：tagged BAM + reference + VCF
   輸出：標籤 × 甲基的關聯統計與結論

lineage-report/  或  LongLineage/presentation/   ← 整合與呈現
└── build_report.py                 ★ 從 InterSubMod 移出
   輸入：LongLineage artifacts + InterSubMod 輸出（皆可缺）
   輸出：standalone HTML
```

## 3. 三專案的職責定義（邊界要清楚）

| 專案 | 唯一職責 | 絕不做 |
|---|---|---|
| **LongLineage** | 純遺傳 read linkage → 拓撲 → 標籤 → tagged BAM | 不做甲基分析、不畫圖 |
| **InterSubMod** | 依 BAM 上的標籤做甲基關聯分析 | 不做 lineage 重建、不產 tagged BAM |
| **整合層** | 讀兩者輸出，產 HTML | 不自行計算科學數字 |

⚠ LongLineage 的 CI 已機械強制這個邊界：
`scripts/ci/check_source_boundaries.sh:17` 規定 Python 只能在 `presentation/`，
且禁止 import pysam/cyvcf2 或呼叫 samtools/bcftools。

## 4. 使用流程（三段，各自可獨立執行）

```bash
# ── 第 1 段：LongLineage — 產生帶標籤的 BAM ──────────────────────
cd LongLineage
bash scripts/build.sh
bash scripts/run_sample.sh --sample HCC1395 \
     --partition-root <PARTITION>/chromosomes \
     --topology <TOPOLOGY>/HCC1395.topology.jsonl \
     --sidecar  <SIDECAR>/HCC1395.read_tags.tsv.gz \
     --in-bam <tagged_or_raw.bam> --out-root <OUT> --threads 16
# 四個資料路徑亦可用環境變數提供：
#   LL_PARTITION_ROOT / LL_TOPOLOGY / LL_SIDECAR / LL_IN_BAM
# （站點路徑一律外部傳入、不寫死在 repo — 見 scripts/ci/check_repo_hygiene.sh）
# 產出：
#   OUT/paths/*.unit_lineage_paths.tsv.gz      階層路徑 + 突變順序
#   OUT/assign/*.read_lineage_assignments.tsv.gz  read → block 指派
#   OUT/bam/*.lineage_tagged.bam(+.bai)        帶 lc/lu/lv/lp/lo/ls 的 BAM

# ── 第 2 段：InterSubMod — 甲基 × 標籤關聯 ───────────────────────
cd InterSubMod
./build/bin/inter_sub_mod \
    -t <OUT>/bam/HCC1395.chr1.lineage_tagged.bam \
    -r <reference.fa> -v <pass.vcf.gz> -o <ISM_OUT> \
    --group-by-tag HP,ALT,lv --require-tag-status U
# 產出：significance_summary.csv（多軸統計）、reads.tsv（含 7 個 lineage 欄位）

# ── 第 3 段：整合 — HTML ────────────────────────────────────────
python3 build_report.py --out-root <OUT> --sample HCC1395 \
        --ism-summary <ISM_OUT>/.../significance_summary.csv \
        --output HCC1395.report.html
# 缺任何輸入 → 對應面板標示原因，不失敗
```

**每一段都能單獨跑**：
- 只跑第 1 段 → 得到 tagged BAM，可直接 IGV 觀察
- 只跑第 2 段 → 需要有標籤的 BAM，否則 lineage 軸為空但 HP/ALT 軸照常
- 只跑第 3 段 → 有多少資料畫多少

## 5. 遷移計畫（4 步，每步可驗證）

| 步 | 動作 | 驗證 |
|---|---|---|
| M1 | 3 支 `.cpp` 移入 `LongLineage/apps/`，改名為 `*_main.cpp` | 用 LongLineage 的 CMake 建置成功 |
| M2 | 加進 `LongLineage/CMakeLists.txt`（既有 `add_executable` 樣板） | `cmake --build` 全過、既有 target 不受影響 |
| M3 | `run_sample.sh` + `build.sh` 移入 `LongLineage/scripts/` | 端到端跑通 HCC1395 一條染色體，結果與現況一致 |
| M4 | `build_report.py` 移入 `LongLineage/presentation/` | 通過 `check_source_boundaries.sh`（不得 import pysam） |

⚠ M4 需檢查：`build_report.py` 目前只讀 TSV/JSON，**沒有 import pysam**，應可通過邊界檢查。

## 6. 決議（2026-08-07 使用者確認）

### ✅ Q1 — 3 支 C++ 放 `LongLineage/apps/`

依性質分類，既有 9 支 executable 分三類：

| 類別 | 既有工具 |
|---|---|
| producer | `longlineage`（production run） |
| **後處理**（讀已凍結 run → 產衍生物） | `longlineage-query`、`-export-legacy`、`-evaluate` |
| 檢查 | `-validate`、`-audit`、`-governance` |

我的 3 支屬**後處理類**，與 `query`/`export-legacy`/`evaluate` 同性質
⇒ 放 `apps/` 並沿用 `longlineage-*` 命名：

```
longlineage-lineage-paths   topology.jsonl → 階層路徑 + 突變順序
longlineage-read-assign     blocks + molecule_calls → read 指派
longlineage-tag-bam         BAM + sidecar + assignments → tagged BAM
```

新建 `tools/` 會造成「同樣是後處理卻分兩處」的解釋負擔。

### ✅ Q2 — 整合層以 LongLineage 為主，ISM 另有簡化層

**核心理由（使用者原話）**：甲基**不一定有**，所以 LongLineage 是更一般的基礎層；
ISM 是可選的加值，用來「加入資訊輔助標記」。

⇒ **兩層 HTML，職責不同**：

| HTML | 位置 | 涵蓋 | 甲基缺席時 |
|---|---|---|---|
| **整合 HTML** | `LongLineage/presentation/` | lineage 結構、拓撲、標籤分佈、BAM 統計 **＋** 甲基面板（若有） | 照常產出，甲基面板標「不可用」 |
| **ISM 簡化 HTML** | `InterSubMod/`（自有呈現層） | 專注「哪些位點的甲基差異與哪個標籤軸有關」 | 不會被呼叫 |

這落實了「模組各自獨立有意義」：
- LongLineage 單獨可用（無甲基資料也完整）
- ISM 單獨可用（給它有標籤的 BAM 就能分析甲基關係）
- 兩者串接時，整合 HTML 才會亮起甲基面板

### ✅ Q3 — `InterSubMod/pipeline/` 只留說明，不留程式

使用者原話：「InterSubMod/pipeline/ 只說明解釋要如何做前置步驟與執行流程」。

| 內容 | 去向 |
|---|---|
| 3 支 `.cpp` | → `LongLineage/apps/` |
| `build.sh` / `run_sample.sh` | → `LongLineage/scripts/` |
| `build_report.py` | → `LongLineage/presentation/` |
| `verify.py` / `freeze_baseline.py` | → `LongLineage/scripts/` |
| 規格文件（9 份） | → `LongLineage/docs/lineage/` |
| **`InterSubMod/pipeline/README.md`** | **保留** — 說明 ISM 的前置步驟：tagged BAM 從哪來、怎麼產、要哪些欄位 |

理由：ISM 的使用者需要知道「我的輸入 BAM 該怎麼準備」，
但那個 BAM 的**產生程式**不屬於 ISM。留說明、不留實作，邊界才乾淨。

### ⏳ Q4 — M1–M4 全部驗證通過後一次 commit

## 7. 遷移的風險

| 風險 | 說明 | 緩解 |
|---|---|---|
| LongLineage 的 C++ commit Hard Gate | 需編譯通過才能 commit | M2 已含建置驗證 |
| `check_source_boundaries.sh` | Python 位置與 import 限制 | M4 已含檢查 |
| 既有 9 支 executable 受影響 | CMake 改動可能波及 | M2 驗證既有 target 全建 |
| 並行 session | 目前偵測到多個 session 共用 HEAD | commit 前先確認、或開 worktree |
