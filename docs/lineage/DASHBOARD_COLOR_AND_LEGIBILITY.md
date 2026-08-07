---
title: 儀表板配色與可讀性規格
date: 2026-08-07
status: 規格（研究已完成，待確認後實作）
---

# 配色與可讀性規格

## 0. 🔴 先更正一個前提

我先前以為參考頁的產生器是 `build_layered_per_sample.py` / `build_layered_workstation_v5.py`。**錯了。**

真正的產生器是
`InterSubMod/research/20260724_exact_ps_cpp_topology_signature_census/scripts/build_exact_ps_layered_workstation.py`

證據：參考頁 `<head>` 的 `intersubmod-ui-contract=layered-workstation-exact-ps-v5` 與
`intersubmod-authority=20260724-exact-ps-hp-strict-read-linkage` 這兩個字串**只定義在該檔 :132,:134**；
且 `build_layered_per_sample.py:1873-1878` 顯示它預設只是 `subprocess.run()` 轉發過去。

⇒ 要抄就抄這一支。

---

## 1. 哪些資訊「可以清楚顯示」（依可靠度分三級）

### 🟢 可直接呈現（數字硬、分母明確）

| 資訊 | 值 | 分析單位 | 來源 |
|---|---|---|---|
| alignment 總數 | 39,617,373 | **alignment** | tag_bam receipts |
| distinct read | 1,001,670 | **read** | assignments |
| read×block 指派 | 1,009,543 | **read×block** | assignments |
| 拓撲單元 | 11,590 | **unit** | topology |
| 可排序單元 | 9,130 | **unit** | topology |
| vertex 總數 | 26,748（hidden 828 = 35.8%） | **vertex** | lineage_paths |
| depth 分佈 | 0=9,130 / 1=10,064 / 2=5,661 / 3=1,476 / 4=348 / 5=69 | **vertex** | lineage_paths |
| ls 四類 | P 1,105,007 / U 95,768 / M 49,082 / A 1,235 | **alignment** | tag_bam receipts |
| 各染色體計數 | 22 條 | **alignment** | per-chrom receipts |
| 階段耗時 | lineage_paths 1s / read_assign 40s / tag_bam ~200s×22 | — | stages.tsv |

### 🟡 可呈現但**必須標分母與單位**

| 資訊 | 陷阱 |
|---|---|
| `is_full_cov` 11.6% | 分母是 **read×block**，不是 read |
| tie 44.9% | 分母是 **unit**，與 ls 的 alignment 層**不同維度** |
| LCA 提升 4.97× | 分母是 `lv_written`，不是總 read |

### 🔴 **不可**用百分比呈現（分母太小）

| 資訊 | 為何 |
|---|---|
| lineage 軸 5/5 = 100% 顯著 | **n=5 < 30**，依 NCHS 標準必須抑制百分比，只能寫 `5 / 5` |
| 任何 n<30 的比例 | 同上 |

---

## 2. 配色：業界慣例 + 本專案方案

### 2.1 通用原則（研究結論）

| 原則 | 出處 |
|---|---|
| **VSUP**（Value-Suppressing Uncertainty Palette）：不確定性高 → 壓縮到近中性色，讓高不確定格子**在物理上不可能看起來像強訊號** | Correll/Moritz/Heer, CHI 2018 |
| 顏色**不可**作為唯一編碼（色盲 + 列印） | 通用無障礙 |
| 序數資料用單色階；類別資料用色相；發散資料才用雙極色階 | ColorBrewer |
| 長條圖會「藏住分母」，且有 within-the-bar bias | Correll |

### 2.2 本專案配色（沿用參考頁的暖紙質底 + 語意色）

**基底**
```
--paper  #f3f0e6   頁面底（米色紙感，長時間閱讀不刺眼）
--panel  #fffdf7   面板（暖白）
--ink    #17231e   主文字（近黑帶綠）
--muted  #667069   次要文字 / 分母標籤
--line   #c9c8bd   分隔線
--soft   #e7e5da   長條底槽
```

**語意色（四個，各有固定意義，不可混用）**
```
--accent  #176b58  深綠  = 確定 / 通過 / U
--accent2 #285f8f  藍    = 中性資訊 / best tree
--amber   #b66e20  琥珀  = 警示 / 需注意 / M(tie)
--danger  #a94336  磚紅  = 阻擋 / 失敗 / A(abstain)
--purple  #6b5592  紫    = 第四類別（僅在需要 4 色分類時）
```

### 2.3 `ls` 四類的專屬色（依可信度遞增，**符合 VSUP**）

| 值 | 意義 | 色 | 為何是這個色 |
|---|---|---|---|
| `P` | partial（子樹範圍） | `#b9b8ad` 中性灰 | 佔 88.3%，**最不確定 → 最中性**，不可搶眼 |
| `M` | tie（多解並列） | `#b66e20` 琥珀 | 需注意但非錯誤 |
| `A` | abstain（拓撲未定） | `#a94336` 磚紅 | 明確不可用 |
| `U` | unique（唯一解） | `#176b58` 深綠 | 唯一可當結論的 |

⚠ 直覺會想把「佔比最大的 P」畫成主色 —— **反了**。P 是最不確定的，必須最不顯眼。

### 2.4 染色體 ideogram（UCSC cytoBandIdeo hg38 標準）

```
gneg     #ffffff      gpos25   #c8c8c8      gpos50   #969696
gpos75   #646464      gpos100  #000000      gvar     #e0e0e0（斜線 pattern）
stalk    #708090      acen     三角形缺口（不填色）
```
資料源 UCSC `cytoBandIdeo.txt`，chr1–22 約 1,200 列，欄式內嵌壓縮後數十 KB。

### 2.5 樹拓撲的邊編碼（四層 stroke，來自參考頁）

| 層 | 色 | 語意 |
|---|---|---|
| 底層 | 灰實線 | 所有 minimum trees 的 edge **union** |
| | 灰虛線 | 僅出現於**部分**候選 |
| | 藍 `#285f8f` | 所有 best trees 的 union |
| 頂層 | 綠 `#176b58` | 目前 exemplar |

節點：**實線圓 = observed**、**虛線圓 = hidden/inferred**（`stroke-dasharray`）

---

## 3. 文字與圖示的清楚度規格

### 3.1 字體
| 用途 | 規格 | 理由 |
|---|---|---|
| h1 | `Georgia/Noto Serif TC`，`clamp(2.2rem,5vw,5.4rem)`，`letter-spacing:-.045em` | 襯線體給雜誌感，大標題有份量 |
| h2 | 同襯線，`clamp(1.55rem,3vw,2.5rem)` | |
| 內文 | `Inter`，`line-height:1.55` | 無襯線易讀 |
| **所有數字** | `ui-monospace,SFMono-Regular,Menlo`，**右對齊** | 位數對齊才能目視比較 |
| KPI 值 | 等寬，`clamp(1.45rem,2.5vw,2.45rem)`，`font-weight:800` | |
| 分母標籤 `.denom` | `.76rem`，`--muted` | 存在但不搶眼 |

### 3.2 分析單位徽章（防維度混淆，**本專案特有需求**）

每個數字旁掛徽章，因為我們有五種分析單位且**極易混淆**：

| 徽章 | 意義 | 例 |
|---|---|---|
| `unit` | 拓撲單元 | 11,590 |
| `region` | block | |
| `read` | distinct qname_sha256 | 1,001,670 |
| `alignment` | BAM 行（含 supplementary） | 1,251,092 |
| `locus` | sSNV 位點 | 79,687 |

🔴 **必須並列顯示** `alignment 1,251,092` 與 `read 1,001,670`，並標明 **+24.9% 差異來自 supplementary/secondary**。

### 3.3 三態視覺（不可只有二元）

| 狀態 | 視覺 | 為何 |
|---|---|---|
| `SIG`（顯著） | 綠底粗體 | |
| `NS`（已檢定但不顯著） | 白底 | |
| **`NOT_TESTABLE`**（未檢定） | **灰底斜體 + title 說明原因** | 🔴 目前程式把它靜默當成「不顯著」，這是錯的 |

### 3.4 圖示與可存取性
- 每個色塊配 `.swatch` 圖例 + 文字標籤（不靠顏色單獨表意）
- SVG 圖附 `<p class="sr-only">` 文字版摘要
- 表頭 `position:sticky` 供長表捲動
- 互動用 `aria-pressed` 而非只有 class

---

## 4. 六條誠信規則（研究結論，優先於美觀）

| # | 規則 | 具體做法 |
|---|---|---|
| **D1** | **分母不同的比例禁止並排長條** | 拆成 4 個獨立 `.axis-card`，每卡標題下第一行固定寫 `分母 = {testable}/{total} region`。**移除「比例」欄**。真要橫比 → 另建「共同可檢定子集」表（本例 n=5） |
| **D2** | **n < 30 不顯示百分比** | `fmt_rate(hits,n)`：n<30 → `"5 / 5"` + `.suppressed` 標記；n≥30 → `"50.0% (15/30)"` + Clopper-Pearson 95% CI；CI 寬度 ≥0.30 也抑制 |
| **D3** | ls 用**單一 100% stacked bar**，四類全列帶絕對計數 | 順序 P→M→A→U，配 §2.3 色。加頻率框架句：「每 1,000 條 alignment 約 883 條僅部分覆蓋」 |
| **D4** | 每個數字掛**分析單位徽章** | 見 §3.2 |
| **D5** | 「未檢定」升為**第三態** | 見 §3.3；並把「無顯著軸」拆成「已檢定但皆不顯著」與「無任一軸可檢定」 |
| **D6** | tie 用 **union 圖 + 四層邊編碼** | 見 §2.5。標題三元式：`tie==1 ? '唯一 selected tree' : '一棵 exemplar（同分 N 棵）'`；固定 caption：「任何聯集都不是一棵樹，edge 次數是 candidate membership，不是 probability 或 read support」 |

---

## 5. 技術決定（研究結論）

| # | 決定 | 依據 |
|---|---|---|
| T1 | 全基因組分佈用 **Canvas 2D** + uniform-grid hit detection（非 SVG） | 98,955 點；參考頁已驗證；grid 約 40 行、比 quadtree 省事 |
| T2 | 樹拓撲用 **inline SVG**，通用 DAG layout（longest-path 求 depth） | HP1-1-1 階層不保證是 hypercube 子圖 |
| T3 | 資料島 **分片 gzip+base64 + `DecompressionStream`** 惰性解壓 | 實測壓縮比 9.9–11.5%；瀏覽器原生、零依賴（Chrome 80/FF 113/Safari 16.4） |
| T4 | 表格資料 **欄式（struct-of-arrays）+ 字串字典編碼** | 壓縮前先縮 3–5×；Canvas 迴圈可跑 TypedArray |
| T5 | 明細表 **虛擬捲動**（固定行高 28–33px + sticky thead） | 取代現行 top-20 截斷 |
| T6 | Python 端 **兩層預聚合**：L0 per-chr×1Mb bin（永遠內嵌）/ L1 per-chr shard（點擊才解壓） | 借 HiGlass tiled multiscale 思路 |
| T7 | **fail-closed provenance**：`AuthorityError` + `require()` + `verify_nested_identities()` + exit 2 | 抄 `build_exact_ps_layered_workstation.py:171-238` |
| T8 | **頁面總和守恆斷言** | 抄 `:4633-4664`；至少斷言 Σunits、ΣU+M+P+A、Σassigned+unassigned |
| T9 | 單頁目標 **< 20 MB** | 現行參考頁 H2009 為 188 MB（反例） |

---

## 6. 待你確認

1. **配色**：沿用暖紙質 + §2.3 的 ls 四色（P 灰 / M 琥珀 / A 紅 / U 綠）？
2. **D2 的百分比抑制**：lineage 軸 n=5 只顯示 `5 / 5` 不顯示 100%，可以嗎？（這會讓數字看起來「不漂亮」但誠實）
3. **分析單位徽章**：五種徽章會讓版面變密，要全上還是只在容易混淆處上？
4. **V1 範圍**：先做哪幾個 section？
