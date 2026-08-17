# 全 C++ 化可行性評估（LongLineage 從讀 BAM 一路做完）

日期：2026-08-06 ｜ 所有行數為實測 `wc -l`

## 結論

**技術上完全可行，且 LongLineage 已有約 11,300 行可直接複用的 C++ 基礎設施。**
真正的新工作集中在三處，其中兩處有現成邏輯可移植。
**唯一的硬障礙不是技術，是 LongLineage 自己的「禁止寫 BAM」政策**。

---

## 1. LongLineage 現有 C++ 資產（實測）

### I/O 層 — 2,320 行
| 檔案 | 行數 | 職責 |
|---|---:|---|
| `block_reader.cpp` | 544 | **BAM 區間讀取**（`sam_open`/`sam_itr_queryi`）、halo、四道過濾 |
| `mm_ml.cpp` | 348 | **MM/ML 甲基解析**（as-sequenced 方向、skip 語意、MN 檢查） |
| `hts_preflight.cpp` | 323 | 輸入驗證（BGZF 檢查、Tabix、header） |
| `sidecar.cpp` | 296 | **HP/PS sidecar 讀取**（Tabix + exact identity join） |
| `alignment.cpp` | 291 | alignment identity（QNAME/FLAG/CIGAR blake2b） |
| `cigar_projection.cpp` | 229 | CIGAR → 參考座標投影 |
| `variant_sites.cpp` | 157 | VCF 讀取（PASS biallelic 檢查） |
| `reference_reader.cpp` | 132 | FASTA（faidx） |

### 科學核心 — 6,326 行
`solver/`（3,021）＋ `m1/` ＋ `cooccurrence/`（site_cooccurrence 455、statistics 592）

### 產物層 — 2,655 行
`artifact/`（BGZF writer、site_index、receipt、semantic digest）

---

## 2. 逐環節評估

| # | 環節 | 現況 | C++ 化難度 | 可複用零件 |
|---|---|---|---|---|
| 1 | **讀 BAM → molecule_calls** | Python 1,248 行 | **中** | `block_reader` 544 ＋ `alignment` 291 ＋ `mm_ml` 348 ＋ `sidecar` 296 ＋ `cigar_projection` 229 = **1,708 行現成** |
| 2 | **strict linkage graph** | Python 695 行；C++ 325 行但**僅 parity 用** | **低** | 邏輯已是 C++（`strict_endpoint_graph_verify.cpp`），只需從「驗證器」改成「生產者」並補 `site_component_membership` 輸出 |
| 3 | **k12 partition** | C++ 934 行 | ✅ **已完成** | 直接用 |
| 4 | **MLHP 轉換** | Python | **低** | 純資料重組，無演算法 |
| 5 | **read_lineage_assignments** | Python（本輪新寫） | **低** | 路由＋投影邏輯已驗證，約 200 行 C++ |
| 6 | **topology + AF ranking** | C++ 1,437 行 | ✅ **已完成** | 直接用 |
| 7 | **signature census** | C++ 456 行 | ✅ **已完成** | 直接用 |
| 8 | **BAM 寫出（ll-bam-tag）** | Python pysam（本輪新寫） | **中** | htslib 已連結；但見 §3 政策障礙 |

⇒ **7 個環節中 3 個已是 C++、3 個難度低、1 個中等且有 1,708 行現成零件。**

---

## 3. 🔴 三個真障礙（皆非技術不可行）

### ⚠ 障礙 A 的重要修正（2026-08-06）

**對「LongLineage 讀 BAM → 輸出 tagged BAM → ISM 做甲基分析」這條主線，障礙 A 不阻塞。**

理由：tagged BAM 的用途是給 ISM 做甲基分析，那些 read **本來就必須帶 MM/ML**。
沒有 MM/ML 的 read，ISM 也無法對它做任何甲基判讀。
因此 `require_mm_ml=true` 在主線上**沒有壞處**。

障礙 A 只在**降級情境**（樣本完全沒有甲基資料、只想要純遺傳結構）才需要處理，
屬可選優化，非前置條件。

且實際改動很小 —— `policy` 結構本就有 `require_mm_ml` 欄位（設計上可配置），
只是 `block_reader.cpp:388-391` 的 guard 把 v1 鎖死：
```cpp
if (policy.minimum_mapq != 20 || policy.minimum_query_length != 1000 || !policy.require_mm_ml) {
    return failure("v1 production block-read policy is fixed at ...");
}
```
放寬 = 改這 4 行 ＋ 換 contract 版本 ＋ 同步 validator。

---

### 障礙 A（原文）— `block_reader` 硬性要求 MM/ML

`src/io/block_reader.cpp:476-487` 丟棄無 MM/ML 的 read，
且 `contracts/v1/science_parameters.json` 凍結 `require_mm=true / require_ml=true`，
綁 governance gate `IO_MM_ML`。

**與「無甲基資料也能跑純遺傳」的目標直接衝突。**
需放寬為可配置，且改動等於換 contract 版本 → 既有 frozen run 不再可比。

### 障礙 B — LongLineage 政策禁止寫 BAM

| 障礙 | 位置 |
|---|---|
| JSON Schema `const false` | `production_input_authority.schema.json:69`（`tagged_bam_output_allowed`）、`:108` |
| 4 處 runtime 硬拒 | `longlineage_main.cpp:188`；`compat/regional_io.cpp:391,465`；`regional_compat_validator.cpp:1121` |
| FILE_CENSUS 拒絕多餘檔案 | `artifact_validator.cpp:3091-3101` |
| typed_aux 摘要契約 | 加 aux tag 會改變 read identity → **讀回自己寫的 BAM 不可重現** |

⇒ **BAM writer 必須是獨立 binary、post-freeze、輸出到 run-root 外**（本輪 `ll_bam_tag.py` 已按此設計）。

### 障礙 C — 改動成本 2×

任何科學邏輯改動必須寫兩遍：producer kernel ＋ 獨立 validator replay
（`artifact_validator.cpp` **5,367 行**，CMake 與 CI 雙重禁止 link producer core）。

---

## 4. 建議路徑

### 階段 1（低風險，高價值）
把 `strict_endpoint_graph_verify.cpp` 從 parity 驗證器改為**生產者**，
補上 `site_component_membership` 輸出（Python 版 13 欄，見 FORMAT_CHAIN §1.2）。
→ 消滅 Python 695 行，且輸出格式已有明確規格。

### 階段 2（最大工作量）
extraction 的 C++ 化。複用 `block_reader` ＋ `sidecar` ＋ `mm_ml`，
新增 molecule 摺疊邏輯（Python 1,248 行中真正的演算法約 400 行）。
⚠ 需先解決障礙 A（MM/ML 硬性要求）。

### 階段 3（薄層）
MLHP 轉換 ＋ read_lineage_assignments writer 併入 C++（各約 200 行）。

### 階段 4（獨立）
`ll-bam-tag` 的 C++ 版，作為**獨立 executable**，不進 LongLineage run
（繞開障礙 B）。htslib 已連結，`bam_aux_append` 直接可用。

---

## 5. 什麼「不建議」全 C++ 化

| 項目 | 理由 |
|---|---|
| HTML 產生 | LongLineage CI 明文限制 Python 只能在 `presentation/`，且禁 pysam/samtools。C++ 產 HTML 無收益 |
| 統計繪圖 | 同上 |
| 探索性分析 | Python 迭代快，且 InterSubMod 已有 268 支分析腳本 |

⇒ **「LongLineage 出 artifact、InterSubMod/Python 出分析與呈現」的分工不該改變** ——
這也是 LongLineage 自己的 CI 邊界所強制的。

---

## 6. 一句話總結

> 從讀 BAM 到輸出拓撲與 read 標籤，**全 C++ 化不僅可行，而且 80% 零件已在 LongLineage 裡**。
> 主要成本在 extraction 的移植與「改一處要寫兩遍（producer + validator）」的治理成本；
> 唯一必須繞開的是 LongLineage 自己禁止寫 BAM 的政策 —— 解法是獨立 binary，不是改政策。
