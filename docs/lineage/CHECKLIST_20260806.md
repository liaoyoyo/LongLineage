# 執行時間、結果數據與合理性檢核表

日期：2026-08-06 ｜ 資料來源：既有已凍結 run（非本輪重跑）
所有數字為實際檔案讀出，無估算值。

---

## A. 執行時間

### A1. topology 階段（唯一有完整計時記錄的階段）

來源：`.../20260724_exact_ps_cpp_topology_af_all_samples/all7_strict_guard1000_v1/summary/all7_summary.json`

**全 cohort `topology_runtime_seconds` = 154.30 秒**（7 樣本合計）

| sample | groups | solver 累計(秒) | solver 單次最大(ms) | search_nodes 累計 |
|---|---:|---:|---:|---:|
| HCC1395 | 11,590 | 3.67 | 29.43 | 407,458 |
| HCC1395_DORADO | 6,865 | 0.99 | 26.92 | 106,383 |
| COLO829 | 13,919 | 1.58 | 25.35 | 275,078 |
| H1437 | 17,598 | 17.83 | 42.59 | 2,185,292 |
| **H2009** | **36,042** | **95.12** | 38.97 | **10,524,720** |
| HCC1937 | 5,195 | 1.66 | 26.91 | 146,442 |
| HCC1954 | 7,746 | 1.19 | 27.89 | 128,192 |

**觀察**：solver 極快。H2009 一家佔 95.12/154.30 = **61.6%** 的時間，
因其 search_nodes 累計是 HCC1395 的 **25.8 倍**。

### A2. 其他階段的時間 — **NOT_FOUND**

| 階段 | 時間記錄 |
|---|---|
| extraction（BAM → molecule_calls） | ❌ repo 內查無計時 |
| strict linkage（molecule_calls → components） | ❌ 查無 |
| k12 partition | ❌ 查無 |

⚠ **extraction 才是真正的耗時瓶頸**（讀 259 GiB BAM），但無任何量測基準。
這是排程 7 樣本前必須先補的空白。

參考量級：LongPhase-S tagging 階段（不同階段，僅供對照）實測
HCC1395 `2026-07-11T09:01:04 → 11:33:39` ≈ **2 小時 33 分**。

---

## B. 結果數據

### B1. strict linkage 階段（7 樣本）

來源：`research/20260723_production_exact_ps_strict_read_linkage/.../data/all7_report_data.json`
（`all_pass: true`，**25 項守恆檢查全過**）

| sample | candidate_loci_S | molecule_rows | all_components | W(HP1+HP2) | W_k>12 |
|---|---:|---:|---:|---:|---:|
| HCC1395 | 79,687 | 3,201,802 | 39,846 | 11,462 | 90 |
| HCC1395_DORADO | 79,739 | 3,507,573 | 43,310 | 6,828 | 34 |
| COLO829 | 37,788 | 988,029 | 42,189 | 13,933 | 30 |
| H1437 | 77,080 | 2,899,543 | 31,588 | 16,326 | 904 |
| H2009 | 154,465 | 7,544,561 | 49,392 | 24,193 | 4,206 |
| HCC1937 | 18,690 | 1,903,289 | 18,612 | 5,155 | 36 |
| HCC1954 | 22,400 | 1,556,809 | 30,815 | 7,724 | 23 |
| **TOTAL** | — | — | — | **85,621** | — |

### B2. topology 階段（cohort totals）

| 指標 | 值 |
|---|---:|
| `groups_total` | **98,955** |
| `mutation_bearing_units` | 85,941 |
| `no_active_alt_units` | 13,014 |
| `objective_certified_units` | 88,238 |
| `objective_abstain_units` | 10,717 |
| `mutation_family_complete_units` | 75,224 |
| `mutation_family_abstain_units` | 10,717 |
| `ranked_units` | 71,955 |
| `best_tree_unique_units` | 39,648 |
| `best_tree_tied_units` | 32,307 |
| `zero_denominator_units` | 3,224 |
| `recurrence_required_units` | 45 |

---

## C. 已驗證通過的守恆（✅ 可直接採信）

| # | 守恆 | 驗證 |
|---|---|---|
| C1 | `active_k` 分佈總和 = `groups_total` | 11,590 = 11,590 ✅（HCC1395） |
| C2 | `k=0` = `no_active_alt_units` | 1,966 = 1,966 ✅ |
| C3 | `groups_total − k0` = `mutation_bearing_units` | 11,590 − 1,966 = 9,624 ✅ |
| C4 | `certified + abstain` = `groups_total` | 11,323 + 267 = 11,590 ✅ |
| C5 | `W_pct_of_all_components` | 11,462 / 39,846 = 28.77% ✅ |
| C6 | strict linkage 的 25 項守恆檢查 | `all_pass: true` ✅ |
| C7 | topology 的 8 項守恆檢查 | `all_pass: true` ✅ |
| C8 | 跨系統 sSNV 輸入一致 | `candidate_loci_S` 79,687 = LongLineage 79,687 sites ✅ |
| C9 | cohort funnel | 85,621 W → 98,955 units → 71,955 ranked + 10,717 abstain ✅ |
| C10 | 缺口守恆 | `zero_denominator` 3,224 + `recurrence` 45 補齊五狀態 ✅ |

---

## D. 🔴 需要你確認合理性的項目

| # | 項目 | 數據 | 為何需要你判斷 |
|---|---|---|---|
| **D1** | **COLO829 的 groups < W** | W=13,933 → groups=13,919，**−14** | 其他 6 樣本都是增加（切分），只有 COLO829 減少。表示有 W 未進入 topology。**這 14 個是什麼？** 生物學上合理嗎？ |
| **D2** | **H2009 規模異常** | S=154,465（HCC1395 的 1.94 倍）、molecule_rows 7,544,561、W_k>12=4,206（HCC1395 的 46.7 倍）、solver 佔全 cohort 61.6% 時間 | 這是真實的高突變負荷，還是 artifact？H2009 的 k>12 比例 4,206/24,193 = **17.4%**，而 HCC1395 只有 0.79% |
| **D3** | **COLO829 的 W 比例異常高** | W/all_components = 13,933/42,189 = **33.0%**，但 S 只有 37,788（最低） | 位點少但 W 佔比最高，與其他樣本趨勢相反 |
| **D4** | **HCC1395_DORADO vs HCC1395** | S 幾乎相同（79,739 vs 79,687）但 W 差很多（6,828 vs 11,462，**−40.4%**） | 同一細胞株不同 basecaller，位點數一致但 linkage 結構差 4 成。可接受嗎？ |
| **D5** | **tie 比例偏高** | `best_tree_tied` 32,307 / `ranked` 71,955 = **44.9%** | 近半數拓撲無唯一解。這對下游 read 標籤意味著 `ls != 'U'` 會很常見 |
| **D6** | **abstain 比例** | 10,717 / 98,955 = **10.8%** | 其中 H2009 一家 8,457（佔全部 abstain 的 78.9%） |
| **D7** | **extraction 無計時** | NOT_FOUND | 7 樣本排程無法估算。是否需要先補測？ |

---

## E. 本輪 B 階段交付檢核

| # | 項目 | 狀態 | 證據 |
|---|---|---|---|
| E1 | `pipeline/lineage/` 模組建立 | ✅ | 目錄存在 |
| E2 | build.sh 從零可重現 | ✅ | `--clean` 後 4 支全過 |
| E3 | 4 支 binary 產出 | ✅ | 151,192 / 193,384 / 266,096 / 301,760 bytes |
| E4 | build 依賴文件化 | ✅ | README「build 依賴」節 |
| E5 | 完整鏈條文件化 | ✅ | README「完整鏈條」節 |
| E6 | CLI 契約取得 | ⚠ 3/4 | `signature_census` 無 `--help` |
| E7 | 中間轉換欄位對應 | ❌ | **未做** — `components.tsv → units.tsv → MLHP.json` 無文件 |
| E8 | 統一 driver | ❌ | 未做 |
| E9 | HCC1395 實跑對帳 | ❌ | 未做（需先定位 molecule_calls 快取） |
| E10 | `read_lineage_assignments` 輸出 | ❌ | **現有鏈條完全無 per-read 標籤輸出** |
| E11 | 錯誤等式已更正 | ✅ | 藍圖已標記 COLO829 反例 |

---

## F. 對「是否合理」的我方初判

**合理**：C1–C10 全部通過，且 cohort funnel 與既有記錄逐字吻合（85,621 → 98,955 → 71,955 + 10,717）。
solver 時間量級合理（154 秒 / 98,955 units ≈ 1.56 ms/unit）。

**需你判斷**：D1–D7 七項，其中 **D1（COLO829 −14）** 與 **D5（tie 44.9%）** 影響最大 —
前者是守恆缺口，後者直接決定 read 標籤有多少會是非唯一解。

**明確缺件**：E7 / E8 / E9 / E10 四項，其中 **E10（per-read 標籤輸出）是接 `ll-bam-tag` → ISM 的必要前提**，
現有鏈條完全沒有這層輸出。
