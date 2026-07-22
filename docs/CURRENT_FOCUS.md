# Current Focus

Last updated: 2026-07-23

## Active phase

P0/P1/P2/P6仍為`IN_PROGRESS`；P3/P4/P5/P7/P8仍為`BLOCKED`。目前最先要解的
science gate是P3 M1 legacy decision parity，不是再增加worker數。

HCC1395已完成一次完整autosome、單資料集、truth-isolated C++ dataset gate：

- w24與w40皆為`VALIDATED_FROZEN_DATASET_GATE`，每次獨立validator為13/13 PASS；
- 79,687 site keys，missing/extra/duplicate皆0；
- 八個run-ID-invariant science artifacts的row count、schema與semantic SHA皆相同；
- input before/after SHA相同，truth fields為0；
- 這是**1/7 bounded dataset evidence**，`production_claim_allowed=false`，不等於P7。

Sanitized machine report為
`docs/reports/20260720_HCC1395完整科學運算與parity報告_01.json`
（SHA-256 `7e0b650b…62eb`）；standalone HTML為同名`.html`
（SHA-256 `9cd86244…5fda`）。完整數字、source binding與查詢欄位見
`docs/data/HCC1395_PERFORMANCE_EDGE_OBSERVATION.md`。

### Python-v2 descriptive regional compatibility endpoint

另有一條**獨立、evaluation/descriptive-only**的
`PYTHON_V2_DESCRIPTIVE_REGIONAL` C++ endpoint，專門重現歷史Python的
50 kb grouping、HP family、MINREAD=3及legacy capped solver；它不取代formal
M2 topology，也不改P3–P8狀態。

- bip7主機以frozen manifest讀取raw HCC1395 BAM、PASS biallelic sSNV VCF、
  truth-free latest HP/PS sidecar與reference；未讀取truth BED/VCF或persisted
  truth-tagged BAM。
- 完整chr1–22、workers=24 run已為`VALIDATED_FROZEN`：8,222 regions、
  20,119 units、106,559 patterns；independent validator 13/13 PASS。
- Python-to-C++逐鍵crosswalk在region、unit、pattern三層的mismatch皆為0；
  兩個CPython 3.9 capped fallback edge case亦逐欄一致。
- science wall 298.8119秒、outer wall 302.26秒。歷史Python觀察為
  5,086.676秒，表面17.023x，但scheduler與cache條件不同，不能宣稱嚴格受控
  full-language benchmark。相同100 regions的w1/w24為14.9467/7.3334秒、
  三份TSV byte-identical，證明可deterministic parallel執行。
- 詳細machine JSON與standalone HTML見
  `docs/reports/20260721_HCC1395_Python相容區域拓撲驗證報告_01.*`。
- 七樣本descriptive v1曾產生7/7 validated bundles，但H2009 crosswalk因
  `C(45,4)=148,995`被舊budget guard誤判而fail closed；因此沒有合法cohort authority
  或七樣本final HTML。修復與regression通過後啟動的v2只完成4/7，之後因輸入形式與
  locked raw-BAM＋exact-sidecar contract解讀分歧而停止；舊process group已終止，未
  resume。兩批都不能替代P7或正式w24/w40 parity。

## Performance verdict

目前**不能**宣稱「完整C++比Python快」。7061.9845秒的歷史HCC工作由Python
編排舊C++ `inter_sub_mod -j6`；它不是同scope的Python science kernel，且沒有
新流程的M1/M2/co-occurrence/topology/獨立validator責任。正式比較狀態為
`NOT_ESTABLISHED_UNMATCHED_BASELINE`。

同一新C++ HCC1395輸入內可下的結論：

| 觀測 | w24 | w40 | 判讀 |
|---|---:|---:|---|
| science core | 3,448.28 s | 3,099.80 s | w40快10.11%，1.112x |
| producer outer | 5,170 s | 5,200 s | w40慢0.58% |
| producer+checksum+validator | 9,946.86 s | 10,039.19 s | w40慢0.93% |
| producer peak RSS | 15.86 GiB | 20.00 GiB | w40高26.05% |
| peak threads | 30 | 46 | 皆符合設定上限 |

因此目前推薦w24。w40只加快block science core，沒有縮短end-to-end；兩次
`cache_condition=UNKNOWN`且read I/O相差4.5867x，不能包裝成受控scaling benchmark。

## Edge-case observations

- IUPAC reference：第一次full attempt在2,578.10秒fail closed；修復後單block
  2.64秒PASS。三block w1/w24為5.39/9.88秒，微小scope平行化反而慢。
- 高深度iterator：第一次full attempt在2,715.83秒fail closed；修復後單block
  62.93秒PASS。704,276 raw hits經固定filter只留4,929 records。
- Dense methylation block：單block 440.03秒，3,055,612 methyl rows，
  peak reorder 224,588,051 bytes。
- 25-block stress：w1 3,143.58秒、w24 571.25秒，semantic SHA相同，
  bounded speedup 5.503x，但RSS與reorder memory明顯增加。
- Zero-group schema：producer 6,039秒完成後，validator用4,529秒正確拒絕；
  14.03秒targeted diagnostic定位到co-occurrence row 432，未freeze錯誤資料。
- Thread sampler：w40舊attempt完成後因量到47 threads被validator立即拒絕；
  v6同步量測46 threads後才通過。

## Phase status mirror

This table is a governed mirror of `state/phase_ledger.json`; it is not an
independent source for changing phase state.

| Phase | Status |
|---|---|
| P0 | IN_PROGRESS |
| P1 | IN_PROGRESS |
| P2 | IN_PROGRESS |
| P3 | BLOCKED |
| P4 | BLOCKED |
| P5 | BLOCKED |
| P6 | IN_PROGRESS |
| P7 | BLOCKED |
| P8 | BLOCKED |

## Current truth

- HCC1395 M1 status census相同，但stable membership不同：old 12,838、new
  12,851；symmetric difference 2,373，Jaccard 11,658/14,031。判定為
  `COMPARABLE_DIFFERENT`，是P3 blocker。
- HCC1395正式C++ co-occurrence已有134,278 pairs與1筆formal BY-confirmed；
  歷史正式full co-occurrence失敗，沒有可宣稱zero-difference的Python authority。
- HCC1395 topology artifact為合法空集合（0 unit）。它證明empty path，不證明
  real nonzero objective/family/parent/tree path。
- Exact topology歷史authority SHA仍凍結；q<=4 oracle與synthetic tests不能取代
  q>4 production family completeness、direct HiGHS與ranking certificate。
- Native frozen root實測為16 artifacts＋8 indexes＝24 files；這只證明HCC1395
  dataset gate，不能外推七資料集 transient/final file census。
- Python report builder未開BAM/VCF/sidecar或science data rows、未重算科學；
  HTML static與keyboard QA均PASS，offline/desktop/mobile/print均PASS。
- `summary@2.0.0`已綁定run-local phase scope與精確M1 representation；但
  2,373-key stable-membership差異尚未由使用該契約的full parity replay解釋，
  因此P3仍為`BLOCKED`。
- Canonical validator已完整重算manifest與八類輸入的SHA/identity、input
  snapshot/lock與lineage；合作式output-base lock及publication snapshot可拒絕已測的
  validation→freeze mutation window。這是P6 implementation evidence，不是phase
  completion，且不代表非合作同UID writer race已消除。

## Active blockers

- `longlineage run`正式入口仍在attestation後回傳`KernelBlocked`；HCC1395使用
  獨立dataset-gate入口。
- P3 stable membership差異2,373 keys；representation現在已由summary v2綁定，
  但尚未完成使用該契約的full parity replay，因此不得猜測根因。
- P5尚無real nonzero topology；direct HiGHS未連結，active bits >=13仍abstain。
- P7七資料集w24/w40尚未執行。
- query row execution、legacy export與posthoc evaluate仍是fail-closed stub。
- FAILED staging尚無durable FAILED receipt/state；`FINAL_UNPUBLISHED`的receipt-only
  recovery已停用，但validator-aware recovery尚未實作；獨立validator亦尚未重播
  HTS semantic preflight。三者都阻止P6升級。
- Publication lock只約束合作式publisher；非合作同UID writer仍可在最後replay與
  receipt發布間改寫path-based artifact，此race尚未以不可變FD/content-addressed
  publication消除，並持續阻止P6升級。
- Strict release gate仍有12個`fixture_only` negative bindings（包含forged
  validator receipt）；最新GCC Debug/Release各47/47 synthetic repository tests不能
  取代這些可執行負例。Strict模式預期exit 1並逐項列出12個blockers；只有明示
  `--allow-declared-blocked`時才exit 0，兩者都不代表P8完成。
- final receipts尚未保存slowest-block identity、edge taxonomy、transient peak
  bytes與I/O operation counts；cache condition仍unknown。
- Report percentage hotfix已重生並重驗：14/79,687文字顯示0.02%，CSS最小
  visual width仍為0.2000%；science projections與八項semantic SHA未改變。
- Report JSON已改為`producer_run_local_phase_status`，scope固定為
  `RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER`，不再與project
  phase ledger碰撞。
- 重要結論尚無`claim_id`／`data-claim-id`，且
  `longlineage.hcc1395_validated_report@2.0.0`尚無獨立closed-shape schema與
  registry binding；兩者都是P8 external handoff blocker。
- Print PDF是A4、17頁且無JavaScript，但`Tagged: no`；屬低嚴重度P8
  accessibility缺口。
- 發布候選已由`origin/main`建立分支`fix/release-repair-verify`並建立private draft
  PR #4。實作commit為`fa1249b9b8873c0b6bc099d8662bd87a62021e8c`，canonical tree
  SHA-256為`689842dc2b64d0e929454e0950d4917729e7e3619073d326f5f19059b22c2e2c`；
  本機fresh Release與GCC ASan+UBSan（`detect_leaks=1`）皆47/47 PASS。Hosted push run
  29938942025與PR run 29938944740 attempt 2皆9/9 PASS，涵蓋history、browser QA、
  GCC/Clang Debug/Release、Clang ASan/UBSan、GCC TSan及pinned container。PR attempt 1
  只有Ubuntu archive mirror timeout，重跑成功；原失敗保留為外部依賴negative evidence。
  Repository與PR仍為PRIVATE/DRAFT，未merge、未tag、未公開。含
  private path/coordinate-shaped blob的舊feature branch只保留為本機recovery reference，
  不可合併或push。
- Restricted HCC performance authority v3仍可由歷史`a3e41c...`snapshot重現，但對目前
  checkout僅40/41 source bindings一致；這不改變已凍結HCC counters，卻禁止宣稱其為
  current-head fully bound或完整C++/Python production speedup證據。
- GPL/source-origin公開稽核尚未完成；repository與draft PR必須保持private，且不得
  建立public release/tag。

## Next legal actions

1. 使用已綁定的run-level M1 representation，逐key定位2,373 membership差異並重播
   RNG、ML scale、tie與cluster relabel vectors。
2. 把已驗證dataset-gate path收斂到正式`longlineage run`，保留attestation、
   validator、atomic freeze與fail-closed boundary。
3. 建立至少一個real nonzero topology bounded gate，再補direct HiGHS與
   >=13 active-bit exact/abstain contract；不完整family不得排名。
4. 對下一次full run先補cache protocol、slowest-block ID、I/O ops與transient
   peak bytes，使用w24作目前預設。
5. Private draft PR #4維持不merge、不公開；任何下一個head仍須通過完整
   GCC/Clang/sanitizer/container/history/browser矩陣，舊feature history不得推送或合併。
6. 把12個fixture-only release bindings升級為可執行negative tests，再重跑strict
   gate。
7. P3-P5與P7-P8維持BLOCKED，直到七資料集w24/w40、semantic SHA與獨立validator
   證據完成。
