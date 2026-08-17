---
title: lineage 觀察儀表板 — 展示方式清單與檢核表
date: 2026-08-07
status: 規格（待確認後實作）
reference: InterSubMod/docs/methodology/_assets/layered_workstation/index.html
---

# lineage 觀察儀表板規格

## 0. 差距診斷：現況 vs 參考範本

| 面向 | 現況 `build_lineage_report.py` | 參考範本 `layered_workstation/index.html` |
|---|---|---|
| 檔案大小 | 14 KB | **95 KB**（單樣本頁 35 MB） |
| section 數 | 4 | **13+** |
| 圖表 | 純 HTML 長條（`<span style=width>`） | 堆疊比例條、漏斗、熱圖、分佈列、SVG |
| 互動 | 無 | tab 切換（`aria-pressed`）、sticky 表頭、跨區連動 |
| provenance | footer 一行 | **`<meta>` 內嵌 5 組 SHA-256** + sticky authority bar |
| 不確定性 | 文字註記 | **專屬 `.denom` class** + `.boundary` 琥珀警示區 |
| 標題 | 「1 · 演化階層與突變順序」 | 「88.26% 有單一 exact shape；**不等於**唯一 clone tree」 |
| 字體 | 全系統字 | h1/h2 **Georgia 襯線**（雜誌感），數字全等寬 |
| 配色 | 冷灰 `#fbfbfa` | **暖紙質** `--paper:#f3f0e6` / `--panel:#fffdf7` |

**核心差距**：參考範本把「**這個數字的分母是什麼、可信到什麼程度**」當成一等公民；我的版本只是把數字列出來。

---

## 1. 可直接複用的設計語言（已從參考範本解構）

### 1.1 配色（暖紙質）
```css
--paper:#f3f0e6   /* 頁面底：米色紙感 */
--panel:#fffdf7   /* 面板：暖白 */
--ink:#17231e     /* 主文字：近黑帶綠 */
--muted:#667069   /* 次要文字 */
--line:#c9c8bd    /* 分隔線 */
--soft:#e7e5da    /* 長條底槽 */
--accent:#176b58  /* 主色：深綠 */
--accent2:#285f8f /* 次色：藍 */
--amber:#b66e20   /* 警示：琥珀 */
--danger:#a94336  /* 危險：磚紅 */
--purple:#6b5592  /* 第四類 */
--shadow:0 10px 32px rgba(30,42,35,.08)
```

### 1.2 排版
| 元素 | 規格 |
|---|---|
| h1 | `Georgia/Noto Serif TC`，`clamp(2.2rem,5vw,5.4rem)`，`letter-spacing:-.045em` |
| h2 | 同襯線，`clamp(1.55rem,3vw,2.5rem)` |
| body | `Inter`，`line-height:1.55` |
| **所有數字** | `ui-monospace,SFMono-Regular,Menlo`，右對齊 |
| metric 值 | `clamp(1.45rem,2.5vw,2.45rem)`，`font-weight:800` |

### 1.3 關鍵元件（照抄 class 名以便日後對照）

| class | 用途 | 為何重要 |
|---|---|---|
| `.authority` | sticky 頂欄，深綠 `#123c34` 白字，放 SHA-256 | 一眼知道看的是哪份權威資料 |
| `.boundary` | 琥珀左邊框 + `#fff7e7` 底 | **claim 邊界警示**，說清楚不能宣稱什麼 |
| `.metrics` / `.metric` | 5 欄 grid，`gap:1px`＋底色做格線 | KPI 帶 label / value / note 三層 |
| **`.denom`** | `.76rem` muted 小字 | **專門標示分母** —— 這是誠信的核心 |
| `.funnel` / `.funnel-step` | 4 欄 + `::after` 畫 `→` | 資料流失的漏斗 |
| `.stack` / `.profile-stack` | 22px / 30px 堆疊比例條 | 組成比例，不是絕對數 |
| `.dist-row` | `label \| bar \| value` 三欄 grid | 分佈列 |
| `.barline` | 7px 細長條 | 表格內嵌迷你圖 |
| `.callout` | 淺綠 `#edf5f1` 提示框 | 解釋性說明 |
| `.tag` | 邊框小標籤，大寫字距 | 狀態標記（U/M/P/A） |
| `.compare-tabs` / `.compare-btn[aria-pressed]` | tab 切換 | 同一區塊切換不同視角 |
| `.heatmap-table` | `table-layout:fixed` + 置中等寬 | 樣本 × 指標熱圖 |
| `th` sticky | `position:sticky;top:0` | 長表捲動時表頭固定 |
| `.methyl-contract` | 跨欄 `#fff7e7` 底 | 該區塊的契約限制 |

---

## 2. 儀表板 section 清單（13 個）

> 標題採參考範本的敘事手法：**數字 + 但書**，不只是分類名。

| # | section | 敘事式標題（草案） | 圖表 | 資料源 | 必標的 caveat |
|---|---|---|---|---|---|
| S0 | authority bar | （sticky）樣本 · 5 組 SHA-256 · 產生時間 | — | receipts | — |
| S1 | hero + boundary | 39,617,373 條 alignment 帶上了演化位置；但只有 7.7% 是唯一解 | KPI ×5 | 全部 receipts | **這不是 clone tree，是 read 的可證位置** |
| S2 | 管線漏斗 | 從 79,687 個 sSNV 到 9,130 個可排序拓撲單元 | `.funnel` 4-6 步 | canonical + receipts | 每步的分母 |
| S3 | 階層分佈 | HP1/HP2 各自分出幾層？depth 5 只有 69 個 | `.dist-row` 橫條 | `unit_lineage_paths` | hidden node 佔 35.8% |
| S4 | 突變順序 | 9,130 個單元的突變獲得順序 | 表格 + 迷你樹 SVG | `mutation_order` + edges | 只有 `best_tree_unique` 的可信 |
| S5 | read 可信度 | 88.3% 的 read 只能定位到子樹，不是單點 | `.stack` 堆疊條 | `ls` 分佈 | **P 不是失敗，是覆蓋不足** |
| S6 | LCA 效益 | 最近共同祖先讓 341,819 條 read 從「無位置」變「有範圍」 | 前後對照條 | 新舊 receipts | `+` 後綴的語意 |
| S7 | 染色體分佈 | 22 條染色體的 lineage 密度 | ideogram 或橫條 | per-chrom receipts | 長度已正規化與否 |
| S8 | BAM 產出 | 259.6 GB / 22 檔 / 全部可 IGV 讀取 | 表格 + `.barline` | receipts + 檔案大小 | — |
| S9 | 多軸比較 | 甲基差異由哪個軸解釋？四個軸的分母不同 | 分組長條 + **分母標注** | `significance_summary` | 🔴 **分母不同不可直接比高低** |
| S10 | 逐位點 | lineage 軸可檢定的 N 個位點 | sticky 表頭長表 + `.tag` | `significance_summary` | 空白 = 未計算 ≠ 0 |
| S11 | 甲基 × 標籤 | 哪些 CpG 支持這個關聯 | 熱圖 | ISM per-read 甲基 | 需 `--emit-per-read-methylation` |
| S12 | 執行 provenance | 每階段耗時與輸入 SHA-256 | 表格 | `stages.tsv` + receipts | — |
| S13 | 已知限制 | 這份報告不能用來宣稱什麼 | `.boundary` | KNOWN_ISSUES | 全部 caveat 彙整 |

---

## 3. 🔴 誠信檢核表（實作時逐項確認）

| # | 規則 | 為什麼 |
|---|---|---|
| I1 | **分母不同的比例，不可並排長條圖** | lineage 軸 5/5=100% vs HP 15/30=50%，並排會誤導 → 改用「分子/分母」雙數字 + `.denom` 標注 |
| I2 | 未計算的統計 → **空白**，不可寫 0 | 0 是一個值，空白才是「沒算」 |
| I3 | `ls != U` 的 `lv` 標籤 → 必須顯示 `+` 或狀態標記 | 那是子樹範圍不是單點 |
| I4 | 任何比例都要能點開看**絕對數與分母** | 比例會騙人，絕對數不會 |
| I5 | tie（多個等價最佳解）→ 標明 tie 數，不可只顯示代表值 | 代表值是任選的 |
| I6 | 每個 metric 標明**資料源檔案** | 可追溯 |
| I7 | 頁首 `<meta>` 內嵌所有輸入的 SHA-256 | 換了資料就換了 SHA |
| I8 | 缺件面板 → 顯示「不可用 + 原因」，不可靜默省略 | 靜默省略讓人以為沒這回事 |
| I9 | 顏色不可作為唯一編碼 | 色盲可讀性；且列印會失真 |
| I10 | 數字全部等寬字體右對齊 | 位數對齊才能目視比較 |

---

## 4. 技術檢核表

| # | 項目 | 決定 | 理由 |
|---|---|---|---|
| T1 | standalone | 全部 inline，零外部請求 | 可離線、可寄送、CSP 安全 |
| T2 | 圖表技術 | **手刻 inline SVG** + CSS grid 長條 | 零依賴；參考範本已證可行 |
| T3 | 大資料量 | 預先在 Python 端聚合；表格分頁或虛擬捲動 | 39.6M alignment 不可能全塞進 DOM |
| T4 | 互動 | 原生 JS（`aria-pressed` tab、表格排序、`<details>` 展開） | 無函式庫 |
| T5 | 資料注入 | 聚合後的 JSON 內嵌 `<script type="application/json">` | 與 `build_workstation.py` 的 §13-A 一致 |
| T6 | 缺資料 | 該面板 refuse + 標原因，不 render 假值 | 由構造防捏造 |
| T7 | 響應式 | grid `minmax()` + `clamp()` 字級 | 參考範本已用 |
| T8 | 列印 | 淺色底、避免大面積填色 | 暖紙質配色本就適合 |

---

## 5. 實作順序（基本版先，再持續改進）

| 階段 | 內容 | 驗收 |
|---|---|---|
| **V1 基本版** | S0/S1/S2/S5/S9/S10/S13 + 設計語言（配色/排版/`.denom`/`.boundary`） | 7 個 section、誠信檢核 I1–I10 全過 |
| V2 | S3/S4/S6/S7 + 階層與突變順序的 SVG | 樹圖可讀 |
| V3 | S8/S11/S12 + 熱圖 + 互動 tab | 熱圖與 tab 可用 |
| V4 | 逐樣本頁（7 樣本） | 跨樣本比較 |

---

## 6. 待確認

1. section 清單（13 個）是否涵蓋你要看的東西？有沒有缺的？
2. 敘事式標題的語氣可以嗎？（數字 + 但書）
3. V1 先做 7 個 section，還是一次做完 13 個？
4. 要不要沿用參考範本的暖紙質配色，還是另立一套？
