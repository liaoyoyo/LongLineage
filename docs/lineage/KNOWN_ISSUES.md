# 已知問題登記簿

> 這份文件與 `verify.py` 的 `KNOWN_ISSUES` 清單**必須同步**。
> 每次改動後跑 `verify.py`，L3 會自動回報登記項是「仍存在 / 改善 / 惡化」。
>
> 狀態：`OPEN`（未解）｜`TRACKED`（已知且可接受，持續監看）｜`RESOLVED`（已解，附證據）｜`WONTFIX`（判定不需處理）

最後更新：2026-08-06

---

## 資料層異常（D 系列）

### D1 — COLO829 的 `groups_total` 少於 `W_total`
| | |
|---|---|
| 狀態 | **OPEN** |
| 嚴重度 | 高 |
| 數據 | W=13,933 → groups=13,919，**−14** |
| 對照 | 其他 6 樣本皆為正（HCC1395 +128、H2009 +11,849） |
| 為何重要 | 切分只會增加 group 數，減少表示**有 W 未進入 topology 階段**。這是守恆缺口。 |
| 自動追蹤 | ✅ `verify.py` L3 |
| 下一步 | 找出這 14 個 W 的 id，檢查它們在 topology 輸入端被丟棄的原因 |

### D2 — H2009 規模與計算量異常
| | |
|---|---|
| 狀態 | **TRACKED** |
| 數據 | S=154,465（HCC1395 的 1.94×）、`W_k_gt12`=4,206（**17.4%**，HCC1395 僅 0.79%）、solver 佔全 cohort **61.6%** 時間、search_nodes 10,524,720（25.8×） |
| 判斷 | 需確認是真實高突變負荷還是 artifact |
| 影響 | 排程時 H2009 是瓶頸；abstain 8,457 佔全 cohort 的 78.9% |

### D3 — COLO829 的 W 佔比最高但位點最少
| | |
|---|---|
| 狀態 | **TRACKED** |
| 數據 | W/all_components = 13,933/42,189 = **33.0%**，但 `candidate_loci_S` 只有 37,788（7 樣本最低） |
| 判斷 | 與其他樣本趨勢相反，需確認 |

### D4 — HCC1395_DORADO vs HCC1395 的 linkage 落差
| | |
|---|---|
| 狀態 | **TRACKED** |
| 數據 | `candidate_loci_S` 幾乎相同（79,739 vs 79,687，+0.07%）但 W 差 **−40.4%**（6,828 vs 11,462） |
| 判斷 | 同細胞株不同 basecaller。位點偵測一致但 read linkage 結構差 4 成 |
| 參考 | 既有記錄指出跨 basecaller **只應比較比例、不比較絕對數** |

### D5 — tie 比例 44.9%
| | |
|---|---|
| 狀態 | **TRACKED**（模型固有性質，非 bug） |
| 數據 | cohort `best_tree_tied` 32,307 / `ranked` 71,955 = **44.9%** |
| 為何重要 | 直接決定 `ll-bam-tag` 寫出的 `ls` tag 分佈 —— 近半數 read 標籤會是 `ls:A:M`（TIE_CLASS）而非 `U` |
| 強制要求 | HTML **必須**呈現 tie 分佈；只顯示代表值即為 overclaim |

### D6 — abstain 10.8%
| | |
|---|---|
| 狀態 | **TRACKED** |
| 數據 | 10,717/98,955；其中 H2009 佔 8,457（**78.9%**） |
| 判斷 | 集中於單一樣本，與 D2 同源 |

### D7 — extraction 與 strict linkage 階段無計時記錄
| | |
|---|---|
| 狀態 | **OPEN** |
| 數據 | 僅 topology 有計時（154.30 秒 / 7 樣本）；extraction、strict linkage、k12 partition **全部 NOT_FOUND** |
| 為何重要 | extraction 讀 259 GiB BAM 才是真瓶頸，7 樣本排程目前**無法估算** |
| 下一步 | 補測單樣本 extraction 耗時 |

---

## 工程缺件（E 系列）

### E6 — `signature_census` 無 `--help`
| | |
|---|---|
| 狀態 | **OPEN** |
| 影響 | 4 支 binary 中唯一介面不明者 |

### E7 — 中間轉換無欄位對應文件
| | |
|---|---|
| 狀態 | **OPEN**，嚴重度高 |
| 內容 | `strict_graph` 出 `components.tsv`，但 `k12_partition` 要 `units.tsv + constraints.tsv`；`topology_af` 要 `MLHP.json` |
| 影響 | **鏈條無法端到端執行** —— 這是 E9 的前置 |

### E8 — 無統一 driver
| | |
|---|---|
| 狀態 | **OPEN** |
| 內容 | 需要 `run_lineage.sh` 串起 extraction → strict_graph → 轉換 → k12 → 轉換 → topology_af |

### E9 — 未實跑對帳
| | |
|---|---|
| 狀態 | **OPEN**（被 E7 阻塞） |
| 前置 | 需先定位 `molecule_sparse_calls.tsv.gz` 快取（全域搜尋曾超時，需精準路徑） |

### E10 — per-read 標籤輸出 ✅ **RESOLVED（2026-08-06）**
| | |
|---|---|
| 狀態 | **RESOLVED** — `pipeline/lineage/build_read_lineage_assignments.py` |
| 驗證 | HCC1395 chr1：`tree_supported` 的 973 個 region_id 與 `topology.jsonl` **雙向 100% 對齊，零誤差** |

**實作方式**：獨立腳本，**不改動已凍結的 `exact_ps_partition_to_mlhp.py`**
（避免破壞既有 parity 測試）。路由與投影邏輯與該檔 `:317-472` 同源。

**chr1 實測結果**（partition 來源 `hcc1395_partition_v2`）：
| 指標 | 值 |
|---|---:|
| rows written | 67,276 |
| molecule_rows_total | 202,042 |
| blocks_total | 975 |
| distinct region_id | 975 |
| tree_supported region_id | **973** |
| full_cov incidences | 9,202（13.7%） |
| tree_supported incidences | 64,701（96.2%） |
| blocks_without_fixed_ra | 13,471 |
| nonprimary_or_missing_ps | 39,889 |

`975 − 973 = 2` 對應官方的 `blocks_k1_not_tree_eligible` + `blocks_pattern_unsupported`。

**輸出欄位**（15 欄）：
```
dataset, chrom, molecule_id, qname_sha256, hp_family, phase_set,
unit_id, block_id, block_index, region_id, pattern_vector,
k, n_fixed_ra_in_block, is_full_cov, tree_supported
```

---

### M2 `ll-bam-tag` ✅ **完成（2026-08-06）** — `pipeline/lineage/ll_bam_tag.py`

方案 C 實作：讀 **raw BAM**，一次注入 sidecar 的 HP/PS ＋ assignments 的 lineage 標籤。

**實跑驗證**（HCC1395 `chr1:1000000-3000000`，25,700 reads）：

| 驗證 | 結果 |
|---|---|
| `samtools quickcheck` | ✅ PASS |
| read 數 | 25,700 → 25,700 **完全一致** |
| `MM:Z` / `ML:B` 保留 | ✅ 甲基資料完好 |
| 輸出大小 | 168,081,117 bytes |

實際寫入統計：
```
hp_written        9,694     ps_written    9,585
lineage_written     355     no_lineage   25,345
  ls_M               29     ls_P            326
```

**真實 tag 範例（證明三者同源）**：
```
QNAME=7337046f-0edf-460a-a79f-bbf3d8f93f5c
  MM:Z:C+h?,2,15,2,...      ← raw BAM 保留
  ML:B:C,0,10,6,...         ← raw BAM 保留
  HP:Z:1-1                  ← sidecar 注入（九態 somatic HP）
  PS:i:653452               ← sidecar 注入
  lc:Z:U67dd31cf070027f8b352cea8
  lu:Z:U67dd31cf070027f8b352cea8:B0001
  lv:Z:AXX                  ← 含 2 個 X（partial）
  ls:A:P                    ← 正確標為 partial，防 overclaim
```

⚠ `HP:Z:1-1` 證實方案 C 的價值 —— 2026-03 canonical BAM 同區只會是 `HP:Z:1`。

### 📋 待觀察（非 bug，需更大樣本確認）

- 該 2 Mb 區間 `lineage_written` 僅 355/25,700 = **1.4%**
  → sSNV 分佈不均所致（assignments 覆蓋全 chr1，但此區 sSNV 少），需在更大區間確認
- **無 `ls_U`**：326 P + 29 M，該區間全無唯一解 read
  → partial 佔 91.8%，符合 D5（cohort tie 44.9%）的預期方向

---

### ⚠ E10a — partition 來源必須是 production，不可用 pilot
| | |
|---|---|
| 狀態 | **TRACKED**（已知陷阱，須寫進 driver） |

同一條 chr1 有**兩個** partition run，`unit_id` 雜湊不同、**完全不可互換**：

| run | blocks(chr1) | 與 topology 對齊 |
|---|---:|---|
| `20260722_exact_ps_k12_hcc1395_pilot/hcc1395_chr1_22_direct_big7_v2` | 3,606 | ❌ 0% |
| **`20260723_production_exact_ps_strict_read_linkage/hcc1395_partition_v2`** | **975** | ✅ **100%** |

⇒ 統一 driver 必須綁定 production partition，並在 receipt 記錄來源 SHA。

---

### ⚠ E10b — block_id 已內含 unit_id（比對陷阱）

`blocks.tsv.gz` 的 `block_id` 格式為 `U010a2a10013900583b8558f2:B0001`（**已含 unit_id**），
不是單純的 `B0001`。任何用 `(unit_id, block_id)` 或 regex 拆解 `region_id` 的比對
都會因格式不一致而**永遠 0% 命中**（本輪實際踩到）。

**正確做法**：直接比對完整 `region_id` 字串，或直接用 `block_id` 欄位。

---

## 🔴 D8 — tagged BAM 版本不一致（2026-08-06 新發現，需決策）

| | |
|---|---|
| 狀態 | **OPEN — 阻塞 ll-bam-tag** |
| 嚴重度 | **最高** |

### 實測證據（同一條 read，三方比對）

read `72ee76c8-9378-492f-8de8-f7c3a3bb3871`，FLAG=16，chr1 start0=89153：

| 來源 | HP | PS |
|---|---|---|
| `molecule_sparse_calls.tsv.gz`（pilot 分析輸入） | **`2-1`** | 103318 |
| **sidecar** `20260711_..._v2/HCC1395.read_tags.tsv.gz` | **`2-1`** ✅ | 103318 ✅ |
| canonical paired_full `HCC1395_tagged.bam`（2026-03） | **`2`** ❌ | 103318 |

⇒ **lineage 分析所用的 HP 來自 2026-07 sidecar（九態 somatic HP），
   而唯一落地的 tagged BAM 是 2026-03 版，HP 值不同。**

`hp_raw` 值域分佈（chr1 前 50,000 列）證實九態：
`1-1` 15,614｜`2-1` 11,944｜`1` 8,379｜`2` 8,010｜`.` 4,305｜`3` 1,747

### 為何重要

`ll-bam-tag` 要把 lineage 標籤寫回 BAM，但：
- 用 2026-03 canonical BAM → HP 與分析基礎不一致，IGV 上看到的 HP 是錯的
- 產生分析所依據的那份 tagged BAM → 它**從未落地**（`consumed_tagged_bam.fifo`，`persisted_tagged_bam: false`）

### 三個選項（待使用者裁決）

| 選項 | 做法 | 代價 |
|---|---|---|
| **A** | 用既有 canonical paired_full BAM（259 GiB，已落地） | HP 值與分析不一致，**不建議** |
| **B** | 用 raw BAM + sidecar 重新產生 tagged BAM | 需磁碟（617 GB 餘量）與計算 |
| **C** | `ll-bam-tag` 直接讀 raw BAM，**同時**從 sidecar 注入正確 HP/PS **與** lineage 標籤，一次寫出 | 最一致；符合 sidecar 取代 tagged BAM 的原始設計 |

初判傾向 **C** —— sidecar 本就是為取代 tagged BAM 而設計，一次寫出可保證 HP/PS/lineage 三者同源。

---

## ✅ 已驗證：sha256(QNAME) join 回 BAM 完全可行（2026-08-06）

實測 3/3 全部成功。方法：
```
molecule_calls.qname_sha256  ==  sha256(BAM 的 QNAME)
＋ start0 與 FLAG 交叉驗證
```
`molecule_sparse_calls.tsv.gz` 另有 `start0, end0, flag, mapq, strand`
（21 欄完整清單見 FORMAT_CHAIN），提供 alignment identity 的輔助欄位，
**join 難度低於原先評估**。原先擔心的「qname 反查」問題可用串流即時計算 sha256 解決，不需落地明文。

---

## ✅ HCC1395 全基因組端到端驗證（2026-08-07）

### 與既有研究的五項對照 — 全部吻合

| 本管線輸出 | 官方 canonical | 來源 |
|---|---|---|
| assignments 列數 **1,009,543** | `projected_molecule_block_incidences` 1,009,543 | MLHP receipt |
| tree_supported **960,780** | `tree_supported_molecule_block_incidences` 960,780 | MLHP receipt |
| units_seen **11,590** | `groups_total` 11,590 | all7_summary |
| units_with_paths **9,130** | `ranked_units` 9,130 | all7_summary |
| alignment **39,617,373** | 來源 BAM chr1-22 idxstats 39,617,373 | samtools |

⇒ C++ 重寫與既有研究邏輯**完全一致**，非近似。

### 🔑 read 數 vs alignment 數 — 統計時必須區分

```
assignments 列數          1,009,543   read × block 指派
  distinct qname_sha256   1,001,670   ← 分子（read）層
tag_bam lineage_written   1,251,092   ← alignment 層
```

差異 **249,422（+24.9%）** 來自 **supplementary / secondary alignment**
（原 tagging 使用 `--tagSupplementary`）。

`tag_bam` 逐 alignment 標記是**正確行為** —— IGV 需要每個 alignment 位置都有標籤。
但任何「多少條 read 有標籤」的敘述必須用 distinct QNAME，不可用 `lineage_written`。

### 新獲得的 read 層數字（既有研究未記錄）

| 指標 | 值 |
|---|---|
| `is_full_cov` | **117,100 / 1,009,543 = 11.6%** |
| `ls` 分佈（alignment 層） | P 1,105,007 (88.3%)｜U 95,768 (7.7%)｜M 49,082 (3.9%)｜A 1,235 (0.1%) |

⚠ **不要與既有研究的 tie 率 44.9% 混淆** —— 那是 **unit 層級**（拓撲是否唯一），
本項是 **read 層級**（read 是否覆蓋整個 block）。兩者維度不同，不衝突。

**對 S5 的直接影響**：`--require-tag-status U` 時可用 alignment 僅約 95,768 條
（佔全部 39.6M 的 0.24%）。分軸檢定的樣本量限制必須在輸出中誠實標示。

---

## 已解決

### ✅ 對帳等式誤判（2026-08-06 當日發現並更正）
原宣稱 `groups = W − k>12 + blocks` 為守恆等式，實際**只對 HCC1395 成立**，
COLO829 為反例（−14）。已更正為 D1 登記項，並在藍圖標註不可當守恆使用。

### ✅ build 依賴未文件化
`topology_af` / `signature_census` 需要 LongLineage solver 源碼 + `-lcrypto` + `-ljansson`，
且 link 順序敏感。已固化於 `build.sh`。
