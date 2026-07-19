# 資料紀錄、檔案格式與查詢準則

本文件是操作指南；machine-readable權威仍是
`LongLineage/schema/catalog.json`及其綁定的schema／registry。兩者衝突時，
machine-readable契約優先，衝突本身即為release blocker。

## 1. 查閱順序

收到「某欄位是什麼、某筆結果從哪裡來、能不能查」時，依序查：

1. `schema/catalog.json`：artifact ID、格式、schema、primary/sort/index key。
2. 對應的`schema/core/*.json`或`schema/records/*.json`：欄位type、必填、enum與條件。
3. `contracts/v1/type_registry.tsv`與`status_reason_codes.tsv`：型別與失敗語意。
4. `contracts/v1/transform_registry.tsv`：哪個versioned transform可產生該artifact。
5. `schema/core/contract_registry_bindings.schema.json`：catalog與
   artifact／transform／query registries的exact SHA與row-set關係。
6. frozen run的`run_receipt.json`、`artifact_catalog.jsonl.bgz`與
   `data_lineage.jsonl.bgz`：實際檔案、digest、producer與input bindings。
7. `docs/data/DATA_CONTRACTS.md`：人類可讀的設計理由與例子。

不得只看檔名、README摘要或presentation圖表推測欄位語意。

## 2. 必須保存的六層紀錄

| 層級 | Machine record | 必備內容 | 用途 |
|---|---|---|---|
| 工作 | `state/tasks/active/*.json` | task type、目標、Step→Verify、證據、阻塞 | AI／人類開發稽核 |
| 輸入 | production manifest | role、絕對path、size、full SHA-256、authority profile | preflight與before/after不變性 |
| Artifact | `artifact_catalog.jsonl.bgz` | schema、format、key range、row count、physical/semantic SHA | 發現與完整性 |
| Record lineage | `data_lineage.jsonl.bgz` | transform、typed input binding、output binding | 正向／反向provenance |
| Run | producer、validation、final receipts | executable hashes、checksums、state transition、truth count | 獨立驗證與freeze |
| 查詢 | query response envelope | normalized filter、plan、source hashes、count/truncation | 可重播read-only查詢 |

任何一層缺少時，不得以另一層的摘要補寫或推測。

## 3. 格式選擇

| 格式 | 適用資料 | 強制規則 |
|---|---|---|
| BGZF TSV | 大量固定欄位、可依site/unit查詢的rows | exact preamble/header、canonical EOF、site index、physical與semantic SHA |
| BGZF JSONL | 巢狀、每unit結構不同但仍可逐筆驗證的records | 每行完整object、schema name/version、canonical key order |
| JSON | bounded manifest、summary與receipt | closed schema、禁止duplicate key與non-finite number |
| LLM v1 BGZF | dense Bernoulli upper-triangle矩陣 | 依`LLM_V1.md`的binary layout、endianness、offset/index與digest |
| TSV registry | 小型、人工review且Git追蹤的closed vocabulary | exact header、唯一ID、穩定排序；不得放run payload |

不以CSV承載正式artifact，不以SQLite、Parquet或任意Python pickle臨時取代已凍結
格式。需要新格式時，先寫ADR、schema、validator與migration。

## 4. Record定義紀律

每個正式record在寫producer前，必須先定義：

- `schema_name`與SemVer版本。
- primary key、唯一性、canonical sort key與可選index key。
- coordinate convention；位置使用`Position1`，區間使用`Interval0`。
- 每欄type、nullability、unit、enum與missing representation。
- metric的grain、numerator、denominator與material exclusions。
- status/reason precedence；`OK_EMPTY`與parse error不可共用空集合。
- conservation rules及cross-artifact membership。
- logical canonicalization與semantic SHA-256算法。
- producer transform、independent validator與query support。

固定語意：

- allele為`R/A/O/X`；O與X不可合併成R。
- missing只可在schema允許時寫`.`或JSON `null`，二者不可互換。
- float只用`LONGLINEAGE_SCI17`；禁止NaN、Inf與negative zero。
- bool只用`true/false`；integer不得有多餘符號或前導零。
- unknown field/enum、duplicate key、out-of-order row、wrong type一律fail closed。
- record順序是科學結果的一部分；thread completion order不得改變輸出順序。
- 兩個以上scalar共同形成座標或identity時，必須另有machine-readable
  `semantic_groups`；`site_reads.start0/end0`固定綁為
  `Interval0 + LT(start0,end0)`，不可只留free-text invariant。
- `canonical_json`只負責實體canonicalization；科學內容必須再綁offline nested
  schema ID/path/SHA與companion count/digest守恆。v1已封閉group×allele counts、
  compatible relation models及joint partner orders三種shape。

## 5. Artifact membership與provenance

一個run的scientific artifact集合由schema catalog與run membership共同封閉定義。
多一個、少一個、同ID重複或catalog/schema SHA不一致都要拒絕。

每個artifact binding至少記錄：

- producer executable SHA-256與allow-listed transform ID；
- 外部輸入的`MANIFEST_INPUT/CONTRACT + PHYSICAL_SHA256`；
- 同run上游artifact的`RUN_ARTIFACT + SEMANTIC_SHA256`；
- physical path/bytes/SHA-256及logical rows/semantic SHA-256；
- index存在時的schema、path、row count及雙digest；
- sensitivity class與primary-key範圍。

不得只有一串digest而沒有source ID；不得讓index、legacy export或presentation
成為科學authority。

Repo-level registry也必須可追溯：`artifact_roles.tsv`的ARTIFACT rows與catalog
artifact ID集合完全相等，`site_index`是唯一subordinate row；transform/query
registry與catalog的實體SHA由`contract_registry_bindings` const schema綁定。
改任一registry時若未同步binding schema與ID registry hash，視為contract drift。

`source_to_target_manifest.json`不得把「有target字串」解讀為完成。每筆mapping
必填`PLANNED/SKELETON/IMPLEMENTED`、`NOT_VERIFIED/CONTRACT_VERIFIED/
PARITY_VERIFIED`、target kind/presence/digest與nullable evidence ID；closed
vocabulary另由`contracts/v1/lifecycle_codes.tsv`鎖定：

- PLANNED：target實體不存在、digest/evidence為JSON null；
- SKELETON：target存在且有digest，但不得填verified evidence；
- VERIFIED：只能建立在IMPLEMENTED target，且必須指向stable gate ID。

目前M1/M2/co-occurrence/topology全parity仍blocked；P6 phase為`IN_PROGRESS`，
但query row execution、validator replay與export parity仍有blocker。新增契約
不等同完成實作或驗證。

## 6. 寫入、驗證與凍結

正式資料只可依下列狀態機發布：

```text
RUNNING → FAILED
RUNNING → VALIDATED → VALIDATED_FROZEN
```

producer在`.staging/<run_id>`寫完artifact後，只能產生
`READY_FOR_VALIDATION`或`FAILED` receipt。獨立C++ validator重開input、artifact、
index與receipt，通過後才寫validation/final receipt並atomic rename。禁止：

- producer自簽PASS；
- 修改既有receipt；
- 在final root直接寫檔；
- validator失敗後保留看似可查詢的部分成功結果；
- 用檔案mtime、BGZF block layout或worker數作semantic identity。

## 7. 查詢規則

正式入口是`longlineage-query`，而且只讀`VALIDATED_FROZEN` run。查詢前必須重驗
receipt chain、checksums、artifact/catalog/schema/index bindings。

允許的target contract：

- discovery與schema inspection；
- 完整index key的exact lookup；
- closed typed AST：`eq`、`in`、inclusive `range`；
- declared field projection；
- 明示`--allow-scan`的bounded sequential scan。

禁止SQL、regex、join、group-by、derived column、補值、重新排序、隨機抽樣、
p-value/FDR重算或topology re-ranking。operand JSON type須與schema完全相同，不作
字串／數字／null coercion。

每次response必須包含run/artifact/schema、normalized filter、query plan、
`matched_rows`、`truncated`、source/index digests及receipt/executable digests。
scan未完成時，`matched_rows`必須為`null`，不可把partial count寫成census。

> **目前狀態**：P6為`IN_PROGRESS`，但CLI目前只驗證frozen/truth boundary後
> fail closed，尚未讀回row；下列介面是凍結契約，不是已可用功能，相關
> implementation gates仍未關閉。

```bash
longlineage-query --run-root /validated/frozen/run \
  --artifact m1_sites --key dataset_order=0 --key site_order=42 --limit 100
```

目前可用的人工查閱方式，是先讀schema/catalog與synthetic fixtures；不得直接對
未凍結run用`zgrep`後把結果宣稱為正式查詢證據。

## 8. Schema或欄位變更流程

1. 寫ADR與living note，說明語意、grain、相容性與最簡替代方案。
   → 驗證：decision ID與受影響artifact清單存在。
2. 依SemVer新增schema版本，不覆寫歷史schema。
   → 驗證：catalog、schema ID、type/status/transform registry一致。
3. 先加positive、unknown-field、wrong-type、duplicate/order/null負例。
   → 驗證：未實作前至少一個新測試按預期失敗。
4. 實作producer與獨立validator，不共用science/solver kernel。
   → 驗證：linkage audit與fault injection通過。
5. 更新query binding與migration；不支援時明示`query_support=false`。
   → 驗證：舊版仍可讀或有明確major-version rejection。
6. 重算schema/catalog/contract hashes並更新manifest binding。
   → 驗證：compiled governance、nested schema positive/negative fixtures、
   registry exact row-set與全部physical hash replay皆PASS。
7. 跑Debug、Release、determinism與適用sanitizer。
   → 驗證：退出碼、實際輸出與digest寫入task evidence。

## 9. 安全與敏感資料

Git只收可重生synthetic fixtures。真實BAM/VCF/sidecar、真實座標、run output、
credential與token不得進Git、task JSON、audit文件或AI prompt。restricted run
root可記絕對輸入路徑；repo內provenance只保存角色、公開source hash與sanitized
stable ID。

## 10. 快速稽核問題

在引用一個數字或一筆row前，必須能回答：

1. 它屬於哪個artifact/schema version？
2. primary key、grain與denominator是什麼？
3. physical與semantic SHA-256在哪個receipt？
4. 哪個transform及哪些typed inputs產生它？
5. validator是否獨立重算且run是否`VALIDATED_FROZEN`？
6. 查詢是否完整；若不完整，是否明示`truncated`與`matched_rows=null`？

任一題答不出，該數字只能標為未驗證觀察，不得進正式claim或presentation。
