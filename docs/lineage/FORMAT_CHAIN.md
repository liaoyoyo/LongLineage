# 中間檔案格式與流程（E7）

日期：2026-08-06 ｜ 欄位全部由**實際檔案** `zcat | head` 讀出，非源碼推測。

實體 run root（HCC1395，已凍結）：
```
<RESEARCH_ROUNDS>/
  20260723_production_exact_ps_strict_read_linkage/hcc1395_strict_regions_v2/
    ├── chromosomes/{chr1..chr22}/
    │     ├── HCC1395.{chr}.components.tsv.gz
    │     ├── HCC1395.{chr}.container_summary.tsv.gz
    │     ├── HCC1395.{chr}.endpoint_edges.tsv.gz
    │     ├── HCC1395.{chr}.site_component_membership.tsv.gz
    │     ├── receipt.json
    │     └── receipt.json.sha256
    └── summary/
```
chr1 實測大小：components 400,184 B｜site_component_membership 359,251 B｜
endpoint_edges 52,732 B｜container_summary 44,295 B

---

## 1. strict linkage 階段的 4 個輸出檔

### 1.1 `components.tsv.gz` — component 主表

```
dataset | chrom | linkage_basis | phase_set | phase_set_status | inference_role
| component_class | tree_eligible | threshold | component_index | component_id
| start1 | end1 | k | span_bp | max_adjacent_gap_bp
| min_internal_bridge_support | max_internal_bridge_support | …
```

`component_id` 格式（實例）：
```
HCC1395:chr1:PS_HP1:PSa4aacc98fe09:E1:1:100111922-100111922:6aa6a330a512dda8
 dataset:chrom:linkage_basis:PS雜湊:E?:threshold:範圍:內容雜湊
```

關鍵欄位值域（實測）：
| 欄位 | 觀察到的值 |
|---|---|
| `linkage_basis` | `PS_HP1` / `PS_HP2` ← **這就是 PS×HP container** |
| `phase_set_status` | `KNOWN_PS_SINGLETON_ABSTAIN` … |
| `inference_role` | `ABSTAIN_SINGLETON_UNLINKED` … |
| `component_class` | `UNLINKED_SINGLETON_COMPONENT` … |
| **`tree_eligible`** | `true` / `false` ← **決定是否進 topology 階段** |

### 1.2 `site_component_membership.tsv.gz` — site → component 對應

```
dataset | chrom | linkage_basis | phase_set | phase_set_status | inference_role
| component_class | tree_eligible | threshold | site_index | pos1 | component_id | linkage_rule
```
`linkage_rule` 實例：`strict_fixed_ra_endpoint…`

### 1.3 `endpoint_edges.tsv.gz` — 邊表

```
dataset | chrom | linkage_basis | hp_family | phase_set
| site_i_index | site_j_index | pos_i1 | pos_j1 | gap_bp
| support_total | support_RR | support_RA | support_AR | support_AA
| passes_primary_threshold | thresholds_passed
```
🔑 `support_*` 四格拆分（RR/RA/AR/AA）＝ 兩端等位組合的分子支持數。

### 1.4 `container_summary.tsv.gz` — container 層彙總

```
dataset | chrom | linkage_basis | hp_family | phase_set
| molecule_rows | fixed_ra_calls | active_nodes | endpoint_pairs_observed
| graph_digest | threshold | retained_edges | regions
| k1_regions | k_gt1_regions | tree_eligible_regions | max_k
```
🔑 `molecule_rows` 只是**計數**，不是 id 清單。

---

## 2. 🔴 E10 的確鑿答案：現有輸出鏈完全沒有 read 層

逐檔檢查四個輸出的欄位，**沒有任何一個含 `molecule_id` 或 `read_id`**：

| 檔案 | 最細粒度 | 有 read/molecule id？ |
|---|---|---|
| `components.tsv.gz` | component | ❌ |
| `site_component_membership.tsv.gz` | **site → component** | ❌ |
| `endpoint_edges.tsv.gz` | site pair（edge） | ❌ |
| `container_summary.tsv.gz` | container（僅 `molecule_rows` 計數） | ❌ |

**現有鏈條最細只到 `site → component`，缺 `read → component`。**

但 read 層資訊**存在於上游輸入**：`molecule_sparse_calls.tsv.gz` 的欄位為
`dataset, chrom, molecule_id, hp_family, phase_set, site_indices, positions1, call_codes`
（來源：`strict_graph --help`）。

⇒ **`molecule_id → component_id` 的對應在 `strict_graph` 執行時必然存在於記憶體中**
（它就是靠 molecule 的兩端 fixed R/A 來建邊的），只是**沒有被寫出**。

### 因此 E10 的最小改法

在 `strict_endpoint_graph_verify.cpp` 新增第 4 個輸出：
```
--read-membership-output READ_MEMBERSHIP.tsv
```
欄位建議（對齊 I/O 規範 §1.6）：
```
dataset | chrom | linkage_basis | phase_set | molecule_id
| component_id | site_indices_in_component | n_fixed_ra_calls | contributed_edges
```
這支只有 **325 行**，是 4 支中最小的，改動風險最低。

---

## 2b. ✅ W 的語意已完全確認（2026-08-06 實測）

從 `container_summary.tsv.gz` 彙總 HCC1395 全 22 條染色體，**按 threshold 分組**
（⚠ 該檔每個 threshold 各一列，不分組會把 1/2/3/5 全加起來得到無意義的 153,060）：

| threshold | regions | tree_eligible_regions | k1_regions | k_gt1_regions |
|---:|---:|---:|---:|---:|
| 1 | 32,448 | 13,210 | 19,238 | 13,210 |
| 2 | 37,091 | 12,160 | 24,931 | 12,160 |
| **3**（primary） | **39,846** | **11,462** | 28,384 | 11,462 |
| 5 | 43,675 | 10,290 | 33,385 | 10,290 |

**與 baseline 對照（T=3）**：
```
regions        39,846 == all_components  39,846   ✅
tree_eligible  11,462 == W_total         11,462   ✅
```

### 確立的定義與恆等式

```
W  ≡  tree_eligible_regions  ≡  k_gt1_regions        （k > 1，即至少 2 個位點）
k1_regions + k_gt1_regions  ==  regions              （T=3: 28,384 + 11,462 = 39,846）
all_components  ≡  regions(threshold=3)
```

⇒ `tree_eligible` 的判準就是 **k > 1**（單點 component 無法建樹）。

### threshold 敏感度（單調且合理）

T 上升 → 邊變少 → component 更碎 → `regions` 增加、`W` 減少：
```
T=1: W=13,210    T=2: W=12,160    T=3: W=11,462    T=5: W=10,290
```
T=3 相對 T=1 損失 13.2% 的 W，相對 T=5 多保留 11.4%。
**這條曲線可作為未來調整 T 的依據。**

### 應加入 verify.py 的新守恆規則

| 規則 | 內容 |
|---|---|
| C9 | `k1_regions(T) + k_gt1_regions(T) == regions(T)` |
| C10 | `tree_eligible_regions(T) == k_gt1_regions(T)` |
| C11 | `regions(T=3) == all_components` |
| C12 | `tree_eligible_regions(T=3) == HP1_W + HP2_W` |

---

## 3. 🔑 D1（COLO829 −14）的候選解釋

`components.tsv.gz` 與 `site_component_membership.tsv.gz` 都有 **`tree_eligible`** 欄位（`true`/`false`）。

假說：**只有 `tree_eligible=true` 的 component 才進入 topology 階段**，
因此 `groups_total` 對應的不是全部 W，而是 W 中 tree_eligible 的子集，
再加上 k>12 切分產生的新 block。

⇒ D1 的 −14 可能來自 COLO829 有較多 `tree_eligible=false` 的 W。

**驗證方法**（尚未執行）：
```bash
# 對每個樣本統計 tree_eligible 的分佈，與 groups_total 對照
zcat <sample>.<chr>.components.tsv.gz | awk -F'\t' 'NR>1{print $8}' | sort | uniq -c
```
`container_summary.tsv.gz` 亦有 `tree_eligible_regions` 欄位可直接彙總。

---

## 4. 🔴 重大更正：C++ strict_graph 是 parity-only 死端

**2026-08-06 深度追蹤推翻了先前的假設。**

先前以為生產鏈是 `strict_graph(C++) → k12_partition(C++) → topology_af(C++)`。**錯誤。**

### 真實資料流

```
molecule_sparse_calls.tsv.gz
   │
   ├─▶ build_strict_ps_hp_regions.py  [Python, 生產]
   │      └─▶ site_component_membership.tsv.gz  ← 🔑 chain 的 load-bearing 檔
   │             │
   │             ▼
   │      exact_ps_k12_partition.py  [Python, 生產]
   │      （輸入 --site-catalog / --site-component-membership / --molecule-calls）
   │             └─▶ units.tsv + constraints.tsv → blocks
   │                    │
   │                    ▼
   │             exact_ps_partition_to_mlhp.py  → MLHP.json
   │                    │
   │                    ▼
   │             topology_af [C++]  → topology.jsonl
   │
   └─▶ strict_graph (C++)  ──▶ edges.tsv + components.tsv  ⛔ 死端
          只有 run_layered_v4_strict.py:780-789 讀它跟 Python 對帳，讀完即丟
```

⇒ **「缺 components.tsv → units.tsv 轉換器」是偽命題，不需要補。**

### C++ vs Python 輸出格式差異（兩者不可互換）

| | C++ `strict_graph` | Python `build_strict_ps_hp_regions.py` |
|---|---|---|
| components 欄數 | **13** | **24** |
| `component_id` | `C000001` 六位序號，**每 container 重置、非全域唯一** | 全域唯一語意字串 `dataset:chrom:basis:PS<sha12>:E<T>:...` |
| `linkage_basis` | ❌ 無 | ✅ `PS_HP1`/`PS_HP2` |
| `hp_family` | 正規化為 `HP1`/`HP2` | 不輸出（改用 linkage_basis） |
| site 表示 | `site_indices`/`positions1` 逗號串接（WIDE） | `start1`/`end1`/`k`/`span_bp` |
| `solver_route` | ❌ 無 | ✅ `DEFER_TO_K12_PARTITION`（**唯一命名 k12 handoff 之處**） |
| 壓縮 | plain TSV | `.tsv.gz` |
| site_component_membership | ❌ **完全沒有此輸出** | ✅ LONG 格式，13 欄 |

⚠ 先前在 §1 記錄的欄位（含 `linkage_basis`）是從**實體檔案**讀的 —— 那些是 **Python 產物**，不是 C++ 產物。

### ⚠ receipt.json 的 invariants 是假的

`strict_endpoint_graph_verify.cpp:261-295` 的整份 JSON 是 hardcoded ostream literal，
其中 6 個 `invariants` 布林**全是寫死的 `true`**，不是實際計算的檢查 ——
**永遠不會回報失敗**。不可當作驗證證據。

---

## 5. ✅ molecule_calls 快取路徑已定位（解除 E9 前置）

| 樣本 | 路徑 |
|---|---|
| HCC1395 | `.../research_rounds/20260722_exact_ps_k12_hcc1395_pilot/hcc1395_chr1_22_direct_big7_v2/chromosomes/{chr}/extraction/` |
| 其餘 6 樣本 | `.../research_rounds/20260723_production_exact_ps_strict_read_linkage/all7_production_v1/samples/{S}/` |

---

## 6. 🎯 E10 的精確實作點

**最佳插入點：`docs/methodology/_assets/20260627_subclone_4axis_teaching/scripts/exact_ps_partition_to_mlhp.py:464-472`**

該處**同時持有**四項必要資訊：
| 資訊 | 來源行 |
|---|---|
| `molecule_id` | `:445  molecule_id = row["molecule_id"]` |
| unit_id / component_id / block_id / hp_family / phase_set | `:429-436  route[...] = key` → `block_by_key[key]` |
| `vector`（pattern 向量） | `:464-466` |
| `chrom` | scope 內 |

且 `region_id` 用與 `:495` 同一條 f-string 構造
⇒ **與 `topology.jsonl` 天然對得上，零轉換**。

實測 frozen `HCC1395.topology.jsonl` 首行：
```
region_id = "chr1|PS=103318|HP=2|U0b5ec11fc58c628c59470b4f:B0001"
representative_best_vertices = [{'label':'ROOT','vertex':0},
                                {'label':'H_AR','vertex':1},
                                {'label':'AA','vertex':3}]
active_positions = [98311, 113820]
```

**加 5–8 行即可**輸出 `read_lineage_assignments.tsv.gz`：
```
dataset, chrom, molecule_id, qname_sha256, hp_family, phase_set,
unit_id, component_id, block_id, region_id, pattern_vector,
is_full_cov, tree_supported
```

### 為何不選其他點

| 候選 | 否決理由 |
|---|---|
| `exact_ps_k12_partition.py:562` | 只有 unit_id，無 block 切分（block 在 partitioner 之後才算），也無 X-marginalization 後的 vector，join 不到 vertex |
| C++ `strict_graph` | 內部確有逐分子迭代（`strict_endpoint_graph.hpp:246`、`:54` 有 `molecule_id`），但輸出 schema 是 pairwise 邊 + component 集合，要加需新增第 4 輸出檔＋改 receipt schema＋改 golden test，成本遠高於 Python 一處 |

### 三個必須一併處理的 caveat

1. **一對多**：含 `X` 的 partial read 在 solver 端被展開成**相容 vertex 的集合**
   （`exact_ps_topology_af.cpp:493-513`），**不是單一 vertex**
   → 需輸出 `is_full_cov` 旗標，partial read 只能給 vertex set
2. **min_read 過濾**：adapter `:485` 的 `weight >= min_read`（預設 3）之後才進 tree
   → `:470` 出的 read 數會**多於**樹實際採用的，需 `tree_supported` 欄位
3. 🔴 **接不回 BAM**：`extract_lossless_read_linkage_collapsing.py:684` 只存
   `qname_sha256 = sha256(query_name)`，**原始 qname 全鏈條無任何地方保留**
   → 建議在 `ll-bam-tag` 串流 BAM 時**即時算 `sha256(query_name)` 做 join**（不落地明文）

---

## 7. 其餘真 gap（不阻塞但需處理）

| # | gap | 證據 |
|---|---|---|
| G1 | **資訊理論不可逆** — 即使想寫 components→constraints 轉換器也不可能：constraints 需 per-molecule `call_codes` + `molecule_weight`，strict_graph 最細只到 pairwise 四態 → k-way pattern 無法由 pairwise 邊際還原 | `exact_ps_k12_partition.cpp:38-42` vs `strict_endpoint_graph_verify.cpp:224-225` |
| G2 | 兩支編排器目錄佈局不相容（`{sample}/chromosomes/{chr}/strict_regions` vs `{sample}/strict_regions_v1/chromosomes/{chr}`） | `run_layered_v4_strict.py:929` vs `all7_exact_ps_inputs.local.json:27` |
| G3 | `k12_partition` 硬拒 `.gz`，單跑須手動 gunzip | `exact_ps_k12_partition.cpp:201-208` |
| G4 | `signature_census` 無編排，兩支 driver 都沒接 | 裸跑回 `--input, --canonical and --output are required` |
| G5 | adapter 硬編碼讀 `<partition_root>/run_receipt.json`，partial scope 時檔名不同會失敗 | `exact_ps_partition_to_mlhp.py:574-578` |
| G6 | `component_id` 非全域唯一（C++ 端），不可當 join key | `strict_endpoint_graph.hpp:349-352` |
| G7 | header 完全相等比對，不接受超集 | `exact_ps_k12_partition.cpp:302-303` |
