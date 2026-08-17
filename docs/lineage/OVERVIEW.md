# 純遺傳 lineage 管線（B 階段：整理既有資產）

> **狀態**：B 階段進行中 — 把散落在 `research/` 下 5 個日期目錄的既有資產整理成可用模組。
> **不複製源碼**，只整理 build、介面、文件與 driver，避免產生會 drift 的副本。
> A 階段（移植進 LongLineage `src/linkage/`）待 B 完成後執行。

## 為什麼需要這個目錄

既有實作**品質良好但組織散亂**（2026-08-06 實測）：

| 檢驗 | 結果 |
|---|---|
| 能否編譯 | ✅ 4 支全過（但依賴從未被記載，實測 5 次才湊齊） |
| 驗證證據 | ✅ `all_pass: true`，**25 項守恆檢查全過** |
| 跑過範圍 | ✅ **7 樣本 × chr1-22 全跑完並凍結**（154 個 autosome row） |
| parity 機制 | ✅ Python↔C++ 比對已內建於每一階段 |
| **組織** | ❌ 散落 5 個 `research/2026xxxx_*/` 目錄，依賴無文件 |

## 工程閉環：改動 → 驗證 → 比對

```bash
# 1. 建置（依賴已固化，從零可重現）
bash pipeline/lineage/build.sh [--clean]

# 2. 凍結基準（只需在建立新基準時跑；數字由腳本從真實檔案抽取，絕不手打）
python3 pipeline/lineage/freeze_baseline.py

# 3. 驗證（改動後必跑）
python3 pipeline/lineage/verify.py --self-test              # 驗證器自檢
python3 pipeline/lineage/verify.py --candidate <new.json>   # 驗證新結果
```

**verify.py 三層檢查**

| 層 | 內容 | 違反意義 |
|---|---|---|
| **L1 守恆** | 8 條算術恆等式 × 7 樣本 = **56 項** | 違反 = **一定有 bug**（exit 2） |
| **L2 baseline** | 與凍結基準逐欄比對（容差可設） | 有差異 = 需人工解釋（exit 1） |
| **L3 已知問題** | 追蹤 `KNOWN_ISSUES.md` 登記項 | 自動回報「仍存在／改善／惡化」 |

離開碼：`0`=PASS ｜ `1`=DIFF 需確認 ｜ `2`=FAIL 守恆違反 ｜ `3`=輸入錯誤

**實測自檢結果**（2026-08-06）：L1 **56/56 全過**，L2 完全一致，L3 正確捕捉到 D1（−14）。

### 固定檔案

| 檔案 | 角色 |
|---|---|
| `build.sh` | 唯一建置入口，依賴已文件化 |
| `freeze_baseline.py` | 基準產生器（含來源 SHA-256 provenance，`all_pass` 非 true 即 refuse） |
| `baseline/cohort_baseline.json` | **凍結基準** — 7 樣本全欄位 + cohort totals |
| `verify.py` | 回歸驗證器（守恆規則 + 已知問題清單的程式碼真值） |
| `docs/KNOWN_ISSUES.md` | 缺陷登記簿，**與 verify.py 的 KNOWN_ISSUES 必須同步** |
| `docs/CHECKLIST_20260806.md` | 執行時間 + 結果數據 + 檢核表 |

## 快速開始（僅建置）

```bash
bash pipeline/lineage/build.sh          # 建 4 支 binary 到 pipeline/lineage/bin/
bash pipeline/lineage/build.sh --clean  # 重建
```

實測輸出（2026-08-06）：
```
strict_graph       OK  151192 bytes
k12_partition      OK  193384 bytes
topology_af        OK  266096 bytes
signature_census   OK  301760 bytes
```

## 🔴 build 依賴（本次實測補齊，先前無任何文件記載）

| binary | 依賴 |
|---|---|
| `strict_graph` | 無（自足） |
| `k12_partition` | 無（自足） |
| `topology_af` | **LongLineage `include/` + `src/solver/{obligation_bnb,parent_mapping}.cpp`** + `-lcrypto` + `-ljansson` |
| `signature_census` | 同上 |

⚠ **`exact_ps_topology_af.cpp:29` 直接 `#include "longlineage/solver/obligation_bnb.hpp"`**
—— InterSubMod 的拓撲工具在 **build 層就依賴 LongLineage**。這是兩 repo 最深的耦合點。

⚠ **link 順序敏感**：`-lcrypto -ljansson` 必須排在源碼**之後**，否則出現整片
`undefined reference to EVP_* / json_*`。`build.sh` 已固定正確順序。

## 完整鏈條（既有實作，全部已存在）

```
LongPhase-S tagged BAM + PASS sSNV VCF
   │
   ▼ extract_lossless_read_linkage_collapsing.py         [1,248 行 Python + samtools]
   │   research/20260718_k_gt8_read_supported_segmentation/scripts/
   │   --manifest --sample --chrom --mapq-min 20 --baseq-min 20 --bridge-thresholds 1,2,3,5
molecule_sparse_calls.tsv.gz
   │
   ▼ strict_graph  [C++ 325 行]  ⟷ build_strict_ps_hp_regions.py [Python 695 行]
   │   --input MOLECULES.tsv --threshold N
   │   --edges-output --components-output --receipt-output
   │   欄位: dataset, chrom, molecule_id, hp_family, phase_set, site_indices, positions1, call_codes
   │   規則: 只有 exact non-missing PS 且 HP1/HP2 的 row 進圖；
   │         只有同一 molecule 兩端皆 fixed R/A 才 +1 edge support
edges.tsv + components.tsv + receipt.json
   │
   ▼ (中間轉換)
   ▼ k12_partition [C++ 934 行] ⟷ exact_ps_k12_partition.py [Python]
   │   --units units.tsv --constraints constraints.tsv --output-dir DIR [--max-block-size 12]
   │   ⚠ 只吃 plain TSV，.gz 需先 gzip -cd
   │   另需 build/bin/exact_ps_partition（InterSubMod 主 build 的 binary）
partition 輸出
   │
   ▼ exact_ps_partition_to_mlhp.py
MLHP.json
   │
   ▼ topology_af [C++ 1,437 行]
   │   --input MLHP.json --output topology.jsonl --receipt topology.receipt.json
   │   [--max-family-size N] [--max-search-nodes N]
topology.jsonl
   │
   ▼ signature_census [C++ 456 行]（無 --help，介面待補）
census
```

編排器：`InterSubMod/scripts/run_layered_v4_strict.py`（`PRIMARY_THRESHOLD=3`, `MAX_BLOCK_SIZE=12`）
與 `research/20260724_exact_ps_cpp_topology_af_all_samples/scripts/run_exact_ps_cpp_topology_all7.py`

### ⚠ 介面不直接銜接

`strict_graph` 出 `components.tsv`，但 `k12_partition` 要 `units.tsv + constraints.tsv`；
`topology_af` 要 `MLHP.json`。中間靠 Python 轉換（`exact_ps_partition_to_mlhp.py` 等）。
**這是 B 階段最需要整理的部分** —— 目前沒有任何文件說明這些轉換的欄位對應。

## HCC1395 canonical 基準（對帳依據）

**strict linkage 階段**
（`research/20260723_production_exact_ps_strict_read_linkage/.../data/all7_report_data.json`，
`all_pass: true`，25 項守恆檢查全過）

| 指標 | 值 |
|---|---|
| `candidate_loci_S` | 79,687 |
| `canonical_molecule_rows` | 3,201,802 |
| `all_components` | 39,846 |
| `active_node_memberships` | 62,651 |
| `active_unique_loci` | 36,384（45.66%） |
| `HP1_W` / `HP2_W` | 5,798 / 5,664（合計 **11,462**） |
| `W_k_gt12` | 90（0.79%） |
| `W_span_gt_50kb` | 1,064（9.28%） |

**topology 階段**
（`.../20260724_exact_ps_cpp_topology_af_all_samples/all7_strict_guard1000_v1/summary/all7_summary.json`）

| 指標 | 值 |
|---|---|
| `groups_total` | **11,590** |
| `mutation_bearing_units` | 9,624 |
| `no_active_alt_units` | 1,966（17.0%） |
| `objective_certified_units` | 11,323 |
| `objective_abstain_units` | 267 |
| `best_tree_unique_units` | 7,047 |
| `best_tree_tied_units` | 2,083 |

`active_k`：`0→1966, 1→3259, 2→4250, 3→1279, 4→405, 5→189, 6→89, 7→49, 8→26, 9→27, 10→13, 11→15, 12→23`
guards：`max_active_bits=12`, `max_family_size=100000`, `max_search_nodes=1000`

### 兩階段對帳等式（已驗證）

```
groups_total = (HP1_W + HP2_W) − W_k_gt12 + blocks_from_k_gt12
11,590       = 11,462          − 90       + 218        (平均 2.42 block/component)
```
內部守恆（實測）：
```
active_k 總和 = 11,590 = groups_total                      ✓
k=0 (1,966)   = no_active_alt_units                        ✓
11,590 − 1,966 = 9,624 = mutation_bearing_units            ✓
11,323 + 267   = 11,590 = groups_total                     ✓
```

### 🔑 跨系統一致性

`candidate_loci_S` = **79,687** 與 LongLineage parity 報告的 **79,687 sites 完全相同**
⇒ 兩套系統吃同一份 sSNV 輸入。

## 待辦（B 階段剩餘）

1. 補齊中間轉換的欄位對應文件（`components.tsv → units.tsv/constraints.tsv → MLHP.json`）
2. `signature_census` 的 CLI 介面（無 `--help`）
3. 統一 driver（`run_lineage.sh`）串起完整鏈條
4. 用 HCC1395 實跑一次並對帳上表數字
5. 新增 `read_lineage_assignments` 輸出（目前鏈條無 per-read 標籤輸出）

## 相關規範

- `InterSubMod/docs/superpowers/specs/2026-08-06-genetic-lineage-pipeline-design.md`
- `InterSubMod/docs/superpowers/specs/2026-08-06-io-contract-spec.md`
- `InterSubMod/docs/superpowers/specs/2026-08-06-implementation-blueprint.md`
