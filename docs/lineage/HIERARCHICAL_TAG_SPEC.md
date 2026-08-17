# 階層式 lineage tag 規格（演化關係直接寫進 BAM）

日期：2026-08-06 ｜ 狀態：設計確認中

## 1. 目標（使用者原話）

> 記錄的資訊包括每一個 read 它是哪一個層級的分類，是哪個 HP 底下的資訊。
> 例如 HP1-1-1 或是 HP2-2 等等 …
> 期望透過 tag 資訊直接知道突變順序與演化分析到 read 的突變狀況。

## 2. ✅ 技術基礎已具備（實測）

`topology.jsonl` 的 `representative_best_edges` 提供完整樹結構：

```json
[
 {"parent_label":"ROOT",  "parent_vertex":0, "child_label":"ARR",   "child_vertex":1,
  "acquired_position":12061318, "acquired_site_index":1, "acquired_active_bit":0,
  "edge_score_fraction":"0/1"},
 {"parent_label":"ARR",   "parent_vertex":1, "child_label":"H_ARA", "child_vertex":5,
  "acquired_position":12072734, "acquired_site_index":3, "acquired_active_bit":2,
  "edge_score_fraction":"1/12"},
 {"parent_label":"H_ARA", "parent_vertex":5, "child_label":"AAA",   "child_vertex":7,
  "acquired_position":12068814, "acquired_site_index":2, "acquired_active_bit":1,
  "edge_score_fraction":"167/588"}
]
```

樹：
```
ROOT ──+12061318──▶ ARR ──+12072734──▶ H_ARA ──+12068814──▶ AAA
depth0             depth1            depth2             depth3
```

⇒ **突變順序 = edges 的 `acquired_position` 依樹深度排序**，直接可得。
⇒ `H_` 前綴 = hidden node（推斷存在但無 read 直接支持的中間態）。

## 3. 階層標籤的構造規則

```
lineage_path = HP{hp_family}                     若 read 落在 ROOT
             = HP{hp_family}-{d1}-{d2}-...-{dk}  否則
```
其中 `d_i` 是從 ROOT 走到該 vertex 的路徑上，**每一步在同層兄弟中的序號（1-based）**。

範例（上圖，`hp_family=1`，鏈狀樹每層只有 1 個子節點）：
| vertex | depth | lineage_path |
|---|---:|---|
| ROOT | 0 | `HP1` |
| ARR | 1 | `HP1-1` |
| H_ARA | 2 | `HP1-1-1` |
| AAA | 3 | `HP1-1-1-1` |

分岔樹範例（同層 2 個子節點）：
```
ROOT ──▶ ARR ──┬──▶ AAR    → HP2-1-1
               └──▶ ARA    → HP2-1-2
```

**排序規則**（保證可重現）：同層兄弟依 `acquired_position` 升冪，再依 `child_vertex` 升冪。

## 4. BAM aux tag 定案（6 個）

| tag | 型別 | 內容 | 範例 |
|---|---|---|---|
| `lc` | `Z` | unit_id（lineage component） | `Uaa263c08cd3c879926566897` |
| `lu` | `Z` | block_id | `Uaa263c08...:B0001` |
| **`lv`** | `Z` | **階層路徑標籤** | `HP1-1-1-1` |
| `ls` | `A` | 狀態 `U`/`M`/`P`/`A` | `U` |
| **`lp`** | `Z` | read 的觀察 pattern（R/A/X） | `ARR` |
| **`lo`** | `Z` | **該 read 路徑上的突變順序** | `12061318>12072734` |

### 為何 `lv` 與 `lp` 並存

- `lp` 是**觀察事實**（這條 read 在各位點測到什麼）
- `lv` 是**拓撲推論**（它在演化樹上的位置）

分開才能讓下游判斷「標籤有多少是事實、多少是推論」。
`ls != 'U'` 時 `lv` 僅為代表值。

### 不變式（防 overclaim，機械保證）

1. `lv` 存在 ⟹ `ls` 必存在
2. `ls == 'P'`（partial，`lp` 含 `X`）⟹ `lv` 對應**相容 vertex 集合**，不得視為唯一
3. `ls == 'A'` ⟹ 不寫 `lv`、`lo`（拓撲未定，寫了就是捏造）
4. `lo` 只列該 read 所在**路徑上**的 acquired position，不是整棵樹

## 5. SAM 規格合規

`lc/lu/lv/ls/lp/lo` 全為雙小寫 → SAM 規格「含小寫字母的 tag 保留給 local use」。
實測 HCC1395 raw BAM 34 個既有 tag 中皆無衝突。

## 6. 下游用途

### IGV
`lv` 可直接用於 Group by tag / Color by tag → 肉眼看到 subclone 分層。

### InterSubMod（甲基 × 標籤關聯）
```
--group-by-tag lv          # 依演化層級分組做甲基分析
--require-tag-status U     # 只用唯一解 read
```
輸出「哪些位點的甲基差異與哪個 `lv` 標籤有關」＋「哪些位點支持該關係」。

### HTML
以 `lv` 為主軸展示：每個突變位點 → 支持它的 read → 甲基狀況 → 分析結論。

## 7. 待確認的設計選擇

| # | 選擇 | 建議 |
|---|---|---|
| Q1 | hidden node（`H_` 前綴）要不要納入 depth 計數 | **要** —— 它是真實的演化中間態，跳過會讓 depth 失真 |
| Q2 | 一條 read 落在多個 block 時 | `lv` 逗號串接，`ls` 取最保守 |
| Q3 | `lo` 用 `>` 還是 `,` 分隔 | `>` —— 明示有序，避免與多 block 的 `,` 混淆 |
| Q4 | ROOT 的 read 要不要標 | 標 `HP{n}`，代表「無 somatic 突變的參考態」 |
