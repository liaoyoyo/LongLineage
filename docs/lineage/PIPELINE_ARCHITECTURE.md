# 整體流程架構確認（定稿）

日期：2026-08-06 ｜ 狀態：架構確認完成，待實作 E10 + ll-bam-tag
所有路徑與 tag 結構均經實測，非設計推測。

---

## 1. 使用者目標（原話對照）

> LongLineage 只完成 C++ 版的 PS×HP 連接分群與 read 標記，還有輸出拓撲分析檔案，
> 然後輸出處理的結果的 BAM 與標籤；
> 讓 ISM 只負責確認 BAM 標籤與甲基的相關性，例如可以確認每個位點的甲基差異是與哪個軸有關；
> 最後用 python 整理完整有效的數據與分析狀況，用 HTML 顯示結果。
> 每個模組分開也有各自意義與用途。
> 避免數據沒有甲基資訊，也有辦法輸出來做有限的觀察。

---

## 2. 定稿架構

```
【輸入】
 raw BAM (292 GB)          MM/ML ✓   HP/PS ✗   ← /big8_disk/data/HCC1395/ONT_5khz.../HCC1395.bam
 sidecar (1.4 GB + .tbi)   HP/PS ✓             ← 20260711_..._sidecars_v2/samples/HCC1395/
 PASS sSNV VCF                                  ← HCC1395.longphase_s.recalibrated.pass.vcf.gz
 reference FASTA + .fai
        │
        ▼
┌──────────────────────────────────────────────────────────────┐
│ M1  lineage 分群與拓撲（純遺傳，不需甲基）                     │
│                                                              │
│  extract_lossless_read_linkage_collapsing.py                 │
│      → molecule_sparse_calls.tsv.gz  (21 欄)                 │
│  build_strict_ps_hp_regions.py                               │
│      → site_component_membership.tsv.gz  ← chain 承重檔       │
│      → components.tsv.gz                                     │
│  exact_ps_k12_partition.py                                   │
│      → units.tsv / constraints.tsv → blocks                  │
│  exact_ps_partition_to_mlhp.py                               │
│      → MLHP.json                                             │
│      → ★ read_lineage_assignments.tsv.gz  (E10 新增)         │
│  topology_af (C++)                                           │
│      → topology.jsonl + receipt                              │
└──────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────┐
│ M2  ll-bam-tag（★ 新建，方案 C）                              │
│                                                              │
│  輸入：raw BAM + sidecar + read_lineage_assignments           │
│  動作：linear 掃描 raw BAM，對每條 alignment：                 │
│        ① 由 sidecar 注入  HP:Z / PS:i                         │
│        ② 由 assignments 注入 lc / lu / lv / ls                │
│        ③ 既有 tag（含 MM/ML/RG）一律保留                       │
│  輸出：完整 tagged BAM（MM/ML + HP/PS + lineage 三者同源）     │
└──────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────┐
│ M3  InterSubMod — 甲基 × 標籤軸關聯                            │
│  --group-by-tag lc|lu|lv|HP                                  │
│  → 每個位點的甲基差異與哪個軸有關                              │
└──────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────┐
│ M4  Python → standalone HTML（缺件時降級，不失敗）             │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 為何選方案 C（D8 裁決）

### 問題
lineage 分析所用的 HP 來自 **2026-07 sidecar**（九態 somatic HP），
但唯一落地的 tagged BAM 是 **2026-03 版**，同一條 read 的 HP 不同（`2-1` vs `2`）。

### 三方實測（read `72ee76c8-9378-492f-8de8-f7c3a3bb3871`, FLAG=16, chr1 start0=89153）

| 來源 | HP | PS |
|---|---|---|
| `molecule_sparse_calls`（分析輸入） | `2-1` | 103318 |
| sidecar 2026-07 | `2-1` ✅ | 103318 ✅ |
| canonical tagged BAM 2026-03 | `2` ❌ | 103318 |

### C 方案為何乾淨（實測驗證）

raw BAM 該 read 的 tag：
```
NM ms AS nn tp cm s1 s2 de rl qs du ns ts mx ch st rn fn sm sd sv dx MN MM ML RG
```
- **有 MM/ML**（甲基，M3 需要）✅
- **無 HP/PS**（未 haplotag）✅

⇒ 注入 HP/PS **不會覆蓋任何既有 tag，零衝突**。
輸出 BAM 的 HP/PS/lineage **三者同源**（皆對應 2026-07 分析基礎）。

---

## 4. join 機制（已實測 3/3 成功）

```
sidecar.QNAME            ──sha256──▶  qname_sha256  ◀── molecule_calls.qname_sha256
raw BAM 的 QNAME         ──sha256──▶  同上
                          ＋ start0 / FLAG 交叉驗證
```

`molecule_sparse_calls.tsv.gz` 21 欄含 alignment identity：
`dataset, chrom, molecule_id, qname_sha256, read_group, alignment_id,
 start0, end0, flag, mapq, strand, hp_raw, hp_family, phase_set,
 site_indices, positions1, call_codes, base_qualities,
 n_sites_in_span, n_fixed_ra, n_alt`

⇒ **不需落地明文 qname**；`ll-bam-tag` 串流時即時算 sha256 即可。

---

## 5. BAM aux tag 規格（已三重查證）

SAM 官方規格：「含小寫字母的 tag 保留給 local use」。
實測 raw BAM 34 個既有 tag 中無 `lc`/`lu`/`lv`/`ls`。

| tag | 型別 | 內容 | 來源 |
|---|---|---|---|
| `HP` | `Z` | 九態 HP（`1`/`2`/`1-1`/`2-1`/`3`） | sidecar |
| `PS` | `i` | phase set | sidecar |
| `lc` | `Z` | component_id | assignments |
| `lu` | `Z` | unit_id | assignments |
| `lv` | `Z` | vertex_label | assignments |
| `ls` | `A` | `U`/`M`/`P`/`A` | assignments |

**不變式**：`lv` 存在 ⟹ `ls` 必存在；`ls != 'U'` 時 `lv` 僅為代表值。

---

## 6. 各模組獨立價值（符合「每個模組分開也有各自意義」）

| 模組 | 單獨使用時的價值 | 是否需要甲基資料 |
|---|---|---|
| M1 lineage | 純遺傳結構確認與再分群 | **不需要** |
| M2 ll-bam-tag | 產出 IGV 可直接觀察的標籤 BAM | 不需要 |
| M3 ISM | 甲基差異 × 標籤軸關聯檢定 | 需要 |
| M4 Python/HTML | 依實際存在的數據產出觀察報告 | 不需要（降級） |

### 降級契約（符合「避免數據沒有甲基資訊也能觀察」）

```
無 MM/ML → M1 照跑（filter 僅 FLAG + MAPQ≥20）
        → M2 照跑（HP/PS/lineage 仍寫入）
        → M3 甲基面板為空
        → M4 標示「無甲基資料，僅遺傳觀察」+ 列出停用面板
```

---

## 7. 功能合理性檢核

| # | 檢核項 | 狀態 | 證據 |
|---|---|---|---|
| F1 | raw BAM 可讀 | ✅ | 292,055,926,761 bytes + .bai 119,741,784 |
| F2 | raw BAM 有甲基、無 HP/PS | ✅ | tag 實測 |
| F3 | sidecar 有正確九態 HP | ✅ | `HP=2-1 PS=103318` |
| F4 | sha256 join 可行 | ✅ | 3/3 成功 |
| F5 | molecule_calls 有 alignment identity | ✅ | 21 欄含 start0/end0/flag/mapq |
| F6 | tag 命名空間無衝突 | ✅ | SAM 規格 + 56 標準 tag + 34 實測 tag |
| F7 | 拓撲 join key 天然對齊 | ✅ | `region_id` 同一 f-string 構造 |
| F8 | M1 不需甲基即可跑 | ✅ | filter 僅 FLAG + MAPQ |
| F9 | 既有結果可對帳 | ✅ | 56 項守恆全過 + baseline 凍結 |
| F10 | E10 實作點明確 | ✅ | `exact_ps_partition_to_mlhp.py:464-472` |

### 尚未驗證

| # | 項目 | 阻塞 |
|---|---|---|
| U1 | `ll-bam-tag` 實作與 `samtools quickcheck` | 待實作 |
| U2 | ISM 讀 `lc/lu/lv/ls` | 待實作 |
| U3 | 端到端實跑對帳 | 待 E10 + U1 |
| U4 | D1（COLO829 −14） | 待查 |
| U5 | 磁碟（617 GB）是否足夠輸出 tagged BAM | 需 `--regions` 控制 |

---

## 8. 與原目標的對照結論

| 使用者目標 | 架構對應 | 狀態 |
|---|---|---|
| LongLineage 做 PS×HP 連接分群 | M1（既有 Python + C++ 鏈，已跑完 7 樣本） | ✅ 已有 |
| 輸出拓撲分析檔案 | `topology.jsonl` + receipt | ✅ 已有 |
| 輸出處理結果的 BAM 與標籤 | M2 `ll-bam-tag`（方案 C） | ⏳ 待實作 |
| ISM 確認標籤與甲基相關性 | M3 `--group-by-tag` | ⏳ 待實作 |
| 每個位點甲基差異與哪個軸有關 | ISM 既有分軸統計量擴充 | ⏳ 待實作 |
| Python 整理 → HTML | M4 spec builder + build_workstation.py | ⏳ 待實作 |
| 模組各自獨立有意義 | §6 四模組表 | ✅ 架構已保證 |
| 無甲基也能觀察 | §6 降級契約 | ✅ 架構已保證 |

**架構層面全部對齊使用者目標，無衝突。**
