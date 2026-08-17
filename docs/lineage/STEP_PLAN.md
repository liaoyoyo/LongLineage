# 分步執行計畫（逐項確認用）

日期：2026-08-06 ｜ 用途：每一步先確認再動工

---

## 第一部分：現況重點

### ✅ 已完成且已驗證

| # | 項目 | 證據 |
|---|---|---|
| A1 | 4 支既有 C++ 可編譯 | `build.sh --clean` 全過（151K/193K/266K/301K bytes） |
| A2 | build 依賴文件化 | 需 LongLineage solver 源碼 ＋ `-lcrypto` ＋ `-ljansson`，link 順序敏感 |
| A3 | baseline 凍結 | 7 樣本、含來源 SHA-256、`freeze_baseline.py` 自動抽取 |
| A4 | 回歸驗證器 | `verify.py` L1 守恆 **56/56 全過**、L2 比對、L3 缺陷追蹤 |
| A5 | 格式鏈追清 | 4 個中間檔欄位、真實資料流（C++ strict_graph 是 parity 死端） |
| A6 | W 語意確認 | `W ≡ tree_eligible ≡ k_gt1`；T=3 時 39,846/11,462 與 baseline 吻合 |
| A7 | **E10 read 標籤 writer** | chr1 與 topology **雙向 100% 對齊，零誤差** |
| A8 | **ll-bam-tag（Python 原型）** | `quickcheck` PASS、read 數一致、`HP:Z:1-1` + `lc/lu/lv/ls` 實際寫入 |
| A9 | sha256(QNAME) join | 實測 3/3 成功，不需落地明文 qname |

### ⏳ 未完成

| # | 項目 | 阻塞什麼 |
|---|---|---|
| B1 | `longlineage-tag-bam` C++ 版 | 目標 3（LongLineage 出 tagged BAM） |
| B2 | ISM 讀 `lc/lu/lv/ls` | 目標 4（甲基 × 標籤關聯） |
| B3 | Python → HTML 呈現層 | 最終觀察報告 |
| B4 | 統一 driver | 端到端一鍵執行 |
| B5 | strict graph 生產版 C++ | 消滅 695 行 Python（優化） |
| B6 | extraction C++ 化 | 全 C++ 化（優化，最大工作量） |

### 🔴 未解問題

| # | 問題 | 影響 |
|---|---|---|
| C1 | COLO829 `groups < W`（−14） | 守恆缺口，資料可信度 |
| C2 | extraction/strict/k12 三階段無計時 | 7 樣本排程無法估算 |
| C3 | chr1 @ 1M-3M 的 `lineage_written` 僅 1.4% | 需更大區間確認是否正常 |
| C4 | 該區間無 `ls_U`（全 P/M） | 需確認是否此區特有 |
| C5 | `strict_graph` receipt 的 invariants 是硬編碼 `true` | 不可當驗證證據 |

---

## 第二部分：分步計畫

> 每步格式：**目標｜輸入｜輸出｜要記錄什麼｜驗收標準｜可改進處**

---

### ✅ S1 — 階層路徑計算器（2026-08-06 完成並驗收）

`pipeline/lineage/build_lineage_paths.py`

**HCC1395 chr1 實跑**：
```
units_seen       973      units_with_paths  803
vertices_total  2312      hidden (H_)       828 (35.8%)
invalid_tree       0   ← 803 個樹結構全部合法
depth 分佈: 0=803, 1=889, 2=460, 3=135, 4=23, 5=2
```
depth1(889) > depth0(803) ⇒ **確實有分岔**。

**實際輸出（鏈狀）**
```
ROOT   d=0  HP1              mut=.
ARR    d=1  HP1-1            mut=12061318                     score=0/1
H_ARA  d=2  HP1-1-1 (hidden) mut=12061318>12072734            score=1/12
AAA    d=3  HP1-1-1-1        mut=12061318>12072734>12068814   score=167/588
```

**實際輸出（分岔）**
```
ROOT  d=0  HP2      mut=.
AR    d=1  HP2-1    mut=12213928
RA    d=1  HP2-2    mut=12219338
```

**lineage_path 值域**（chr1）：
`HP2-1 407｜HP2 407｜HP1-1 396｜HP1 396｜HP2-1-1 222｜HP1-1-1 208｜
HP2-1-1-1 61｜HP1-1-1-1 55｜HP1-2 45｜HP2-2 41｜HP2-1-1-1-1 14｜HP1-2-1 8`

18 個輸出欄位含 `lineage_path`、`mutation_order`、`depth`、`is_hidden`、
`edge_score_fraction`、`n_children`、`best_tree_unique`、`family_status`。

---

### STEP 1 — `longlineage-tag-bam`（C++ 版）

**目標**：LongLineage 用 C++ 從 BAM 讀到 BAM 寫，輸出含 lineage tag 的 BAM。

**輸入**
| 角色 | 說明 |
|---|---|
| raw BAM ＋ `.bai` | 有 MM/ML、無 HP/PS |
| HP/PS sidecar ＋ `.tbi` | 九態 HP |
| `read_lineage_assignments.tsv.gz` | E10 產出 |
| `topology.jsonl`（選用） | 供 `ls` 判定 U/M/A |
| `--region` | **必要**，磁碟保護 |

**輸出**
- tagged BAM（保留全部原 tag ＋ `HP:Z` `PS:i` `lc:Z` `lu:Z` `lv:Z` `ls:A`）
- `receipt.json`：輸入 SHA-256、寫入統計、參數

**要記錄什麼**
```
reads_total / hp_written / ps_written / lineage_written / no_lineage
ls_U / ls_M / ls_P / ls_A / multi_block_reads
no_sidecar_row / ps_malformed
每個輸入檔的 path + sha256 + size_bytes
region、binary 版本、執行時間
```

**驗收標準**
1. `samtools quickcheck` exit 0
2. 輸出 read 數 == 輸入 read 數
3. `MM`/`ML`/`RG` 等原有 tag 逐一保留
4. 與 Python 原型 `ll_bam_tag.py` 在同一 region 的 tag **逐 read 完全一致**
5. IGV 可依 tag 分組

**可改進處（相對 Python 原型）**
- 🔹 Python 原型把整個 region 的 sidecar 與 assignments **全載入記憶體**；C++ 版可改串流
- 🔹 Python 原型無 `receipt.json`；C++ 版應輸出（可重現性）
- 🔹 Python 原型未記錄執行時間；C++ 版應記
- 🔹 `--require-all-reads` 目前只檢查 lineage，未檢查 sidecar 覆蓋率

---

### STEP 2 — ISM 讀 lineage tag

**目標**：ISM 能依 `lc/lu/lv` 分組做甲基分析，回答「每個位點的甲基差異與哪個軸有關」。

**輸入**：STEP 1 的 tagged BAM ＋ reference ＋ PASS VCF

**輸出**
- 既有 `significance_summary.csv`（**加 `schema_version` 欄**）
- **新增** `per_read_methylation.tsv`：
  ```
  read_name, read_id(sha256), region_key, hp, phase_set,
  lc, lu, lv, ls, allele_call,
  n_cpg_covered, n_cpg_methylated, n_cpg_unmethylated, n_cpg_ambiguous, mean_beta
  ```

**要記錄什麼**
```
每個位點：各軸的檢定統計量與 p 值（HP軸/Allele軸/Cluster軸/lc軸/lu軸/lv軸）
使用的二值化閾值實際值（目前三套不一致）
--group-by-tag 與 --require-tag-status 的實際參數
被排除的 read 數與原因
```

**驗收標準**
1. `run_tests` **258/258 維持**（現況基準）
2. 編譯零新警告
3. `per_read_methylation.tsv` 的 `read_id` 與 assignments **100% join**
4. 分軸統計量在 `ls=U` 與 `ls=P` 子集上分別可算

**必須一併修的既有缺陷**（前面盤點出的 5 項）
| # | 缺陷 | 修法 |
|---|---|---|
| 1 | `methylation.csv` 首欄是矩陣列號，甲基↔read 靠隱含列序 | 首欄改 `read_name` ＋ 加 assert |
| 2 | cluster id 完全不落檔 | 併入 `per_read_methylation.tsv` |
| 3 | 二值化三套閾值（0.8/0.2、0.5、128/255） | 統一由 CLI 驅動，輸出記錄實際值 |
| 4 | `linkage_matrix.csv` 實為 TSV | 改副檔名 |
| 5 | `significance_summary.csv` 無版本欄 | 加 `schema_version` |

**可改進處**
- 🔹 ISM 目前**完全不讀 PS tag**（grep 零命中）→ 應一併加
- 🔹 建議先做「只加讀 tag、不改既有邏輯」的最小版本，確認 258 測試不掛，再做 5 項缺陷

---

### STEP 3 — Python 呈現層 → HTML

**目標**：把 LongLineage 與 ISM 的輸出整合成可觀察的 HTML。

**輸入**（全部可缺，缺件降級不失敗）
`linkage_components` / `read_lineage_assignments` / `topology.jsonl` /
`per_read_methylation.tsv` / `significance_summary.csv`

**輸出**
- `workstation_spec.json`（含 `availability` 區塊）
- standalone HTML

**要記錄什麼**
```
每個面板的 available / reason / input_path
所有 metric 的來源檔案與行號
tie_class 分佈（ls != U 的比例）  ← 防 overclaim，必須顯示
缺件清單
```

**驗收標準**
1. 4 種缺件情境各產出一份 HTML，缺件面板顯示原因而非靜默省略
2. Python **不自行計算科學數字**，只呈現 artifact 內既有值
3. 缺必填 key 即 refuse render（複用 `build_workstation.py` 的 exit 3）

**可改進處**
- 🔹 `build_workstation.py` 全 repo 只有 1 個複用者 → 本次應成為第 2 個，驗證其通用性
- 🔹 缺「spec builder」層（artifact → spec）是目前最大空洞

---

### STEP 4 — 統一 driver

**目標**：一個命令跑完 extraction → linkage → partition → topology → assignments → tag-bam。

**要記錄什麼**
```
每個 stage 的 exit code、耗時、輸入輸出 SHA-256
partition 來源（必須是 production，不可用 pilot ← E10a 陷阱）
```

**可改進處**
- 🔹 現有兩支編排器目錄佈局不相容，需統一或加 symlink 層
- 🔹 `k12_partition` 硬拒 `.gz`，driver 需自動 gunzip
- 🔹 `signature_census` 兩支編排器都沒接

---

## 第三部分：跨步驟的可改進項（獨立於上述）

| # | 可改進 | 理由 | 優先 |
|---|---|---|---|
| I1 | 把 `verify.py` 的守恆規則補到 12 條 | 本輪新發現 C9–C12（`k1+k_gt1==regions` 等） | 高 |
| I2 | 查清 C1（COLO829 −14） | 守恆缺口影響資料可信度 | 高 |
| I3 | 補三階段計時 | 7 樣本排程無法估算 | 中 |
| I4 | `strict_graph` receipt 的假 invariants | 硬編碼 `true`，誤導 | 中 |
| I5 | 更大區間重測 `lineage_written` 比例 | 確認 1.4% 是否正常 | 中 |
| I6 | E10 writer 支援多染色體一次跑 | 目前一次一條 | 低 |
| I7 | strict graph 生產版 C++ | 消滅 695 行 Python | 低 |
| I8 | extraction C++ 化 | 全 C++ 化 | 低 |
