# LongLineage

LongLineage is a truth-isolated C++17/HTSlib pipeline for long-read
sSNV co-occurrence and lineage-compatible mutation-state families.

目前狀態：**P0/P1/P2/P6 foundation implementation in progress；不是production
release。** P3/P4/P5科學parity、P7全量run與P8 release仍明確BLOCKED。

LongLineage讀取raw ONT alignments、MM/ML methylation tags、frozen PASS biallelic
sSNV VCF與authoritative latest HP/PS sidecar，建立：

- read/site與methylation packed records；
- truth-blind M1 methylation groups；
- M2 eligibility與sSNV co-occurrence；
- exact或誠實incomplete/abstain的mutation-state families；
- independent validation、frozen provenance、legacy export與read-only query。

正式工具不接受truth。benchmark由獨立`longlineage-evaluate`在run凍結後執行；
Python只負責呈現validated C++ chart-ready artifacts。

## 與 InterSubMod 的關係（先讀這節再決定要不要裝）

LongLineage 有一個搭檔 repository：**[InterSubMod](https://github.com/liaoyoyo/InterSubMod)**。
兩者**建置完全獨立**（互不引用對方的 CMake），但在執行期透過檔案格式銜接。
不知道這件事會走冤枉路，所以先講清楚誰做什麼、誰先跑。

| | LongLineage | InterSubMod |
|---|---|---|
| 角色 | lineage 求解與標記 | read-level 甲基化與體細胞變異分析 |
| 主要輸出 | 帶 lineage 標籤的 BAM | 區域層級的統計與矩陣 |

### 資料怎麼流動

**主流程是單向的 LongLineage → InterSubMod：**

```
上游（不屬於這兩個 repo）
  dorado basecalling（帶 MM/ML）→ 比對 → LongPhase haplotag → 體細胞 VCF
        │
        ▼
LongLineage  scripts/run_sample.sh
  longlineage-lineage-paths → longlineage-read-assign → longlineage-tag-bam
        │
        ▼  寫入 BAM aux tag：
        │    lc:Z  lineage component
        │    lu:Z  lineage block
        │    lv:Z  lineage path（例：HP2-1-1）
        │    ls:A  status  U | M | P | A
        ▼
InterSubMod  inter_sub_mod --tumor-bam <tagged.bam> --reference <fa> --vcf <somatic.vcf>
```

**InterSubMod 沒有 LongLineage 也能跑。** 輸入 BAM 若沒有經過 `tag-bam`，
InterSubMod 會直接跳過 lineage 軸，而不是把所有 read 當成一個大群組
（見 `InterSubMod/include/core/Stats.hpp` 的 lineage axes 註解）。
所以想單獨使用任一邊都是可以的；一起用只是多拿到 lineage 維度。

**反方向不是相依，是驗證。** `src/compat/regional_crosswalk.cpp` 會讀取
InterSubMod 凍結產出的 manifest（`schema_name = intersubmod.layered_sample_output_manifest`）
做 authority 比對，那是 `longlineage-regional-compat` 這條驗證路徑在用的，
不在主資料流上 —— 不要因為看到它就以為必須先跑 InterSubMod。

## 資源需求（實測記錄，未測的據實標明）

沒有量過的東西這裡不寫估計值。

| 項目 | 實測值 | 量測條件 |
|---|---|---|
| 完整測試套件 | **49 tests 全過，159.97 秒** | 2026-08-17，Debug build，`--parallel 16` |
| 參考稽核主機 | 48 logical CPUs、約 495 GiB 可用 RAM、swap 未使用 | `docs/audits/20260719_HCC1395全速執行就緒度與時間稽核_01.md` |
| 輸出寫入設定 | writer threads 4、buffer 8 GiB | 同上 |
| 單樣本全基因組執行時間 | **未量測** | — |
| 單樣本記憶體上界 | **未量測** | — |
| 7 樣本執行時間與記憶體上界 | **未量測** | 亦記載於 InterSubMod README |

要跑 production 規模之前，請先自己量一個樣本再推估，不要引用上表的主機規格當作需求下限。

## Quick start

```bash
/usr/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLONGLINEAGE_BUILD_TESTS=ON
/usr/bin/cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/bin/longlineage --version
./build/bin/longlineage --help
scripts/ci/check_all.sh build
```

> **上面的 `/usr/bin/cmake` 是刻意寫死的，不是疏漏。** 本專案鎖定工具鏈以保證可重現性，
> `scripts/ci/check_dependency_lock.sh` 會主動驗證 CI 使用該路徑。
> 若你的 cmake 不在 `/usr/bin`（macOS Homebrew、conda、HPC module 等），
> 把指令中的 `/usr/bin/cmake` 換成你的路徑即可建置；但這樣就脫離了工具鏈鎖定，
> 產出不保證與參考環境位元一致。
>
> 相對地，**所有資料路徑都不寫死** —— 一律由 CLI 參數或 `LL_*` 環境變數傳入，
> `scripts/ci/check_repo_hygiene.sh` 會阻擋硬編的絕對路徑。

`preflight`需要完整的八角色locked-input manifest與實際HTS indexes；repo刻意不提交
真實資料，也不以空fixture偽裝production preflight。synthetic typed-I/O負例由CTest
覆蓋。

## Canonical entry points

- Governance: `LongLineage/AGENTS.md`
- Current work: `LongLineage/docs/CURRENT_FOCUS.md`
- Architecture: `LongLineage/docs/architecture/C4.md`
- Data contracts: `LongLineage/docs/data/DATA_CONTRACTS.md`
- 資料紀錄、格式與查詢準則：
  `LongLineage/docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md`
- Development: `LongLineage/docs/development/WORKFLOW.md`
- Release gates: `LongLineage/docs/release/RELEASE_GATES.md`
- Public-preview boundary and fail-closed safety status:
  `LongLineage/docs/release/PUBLIC_PREVIEW.md`
- Branch/commit-specific capability matrix:
  `LongLineage/docs/release/CAPABILITY_MATRIX.md`
- Claim boundary: `LongLineage/docs/claims/CLAIM_BOUNDARY.md`

## License and release status

Source files declare `GPL-3.0-only`; the canonical GPLv3 text is in `LICENSE`.
The source-origin and dependency inventory produced by the public-preview audit is
now in the repository:

- `SBOM.spdx.json` — SPDX software bill of materials
- `THIRD_PARTY_NOTICES.md` — third-party component notices
- `docs/release/PUBLIC_SAFETY_RECEIPT.json` — public safety receipt
- `provenance/source_to_target_manifest.json` — source→target mapping with authority class

**這個 repository 現在是公開的，但它不是一個經過認證的 release。**
`scripts/ci/check_public_preview_gate.sh HEAD` 目前仍回報 `FAIL`，未結案項目：

| Blocker | 意義 |
|---|---|
| `repository_license_review=PENDING_PUBLIC_RELEASE_AUDIT` | 整體授權審查尚未結案 |
| `unresolved_source_rows=4` | 4 筆來源對應為 `NO_GO`，尚未裁決 |
| `unapproved_source_license_rows=21` | 21 筆來源對應的授權尚未核准 |
| `dependency_license_noassertion=11` | SBOM 中 11 個相依項授權為 `NOASSERTION` |
| `history_hygiene` | 歷史中有 4 個舊 blob 含開發機的絕對路徑（**目前工作樹為 0**） |

請據此判斷用途：**程式碼可讀、可建置、可跑測試；但不要把它當作已完成授權清算的
釋出版本來重新散布。** 最新狀態一律以實跑該 gate 的輸出為準，不以本段文字為準。
