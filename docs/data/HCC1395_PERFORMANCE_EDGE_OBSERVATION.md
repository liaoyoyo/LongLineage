<!--
建立時間: 2026-07-21 03:37 +08:00
目標: 定義 HCC1395 bounded C++ performance、平行化與邊緣案例觀測的紀錄和查詢方式
處理範圍: HCC1395 dataset gate；非七資料集 production、非 C++/Python 語言 benchmark
關聯檔案:
  - LongLineage/schema/presentation/hcc1395_performance_edge_observation.schema.json
  - LongLineage/schema/presentation/hcc1395_execution_evidence.schema.json
  - LongLineage/docs/development/PERFORMANCE_AND_OUTPUT_POLICY.md
-->

# HCC1395 performance／edge observation 查詢指南

## 1. Authority boundary

這份 observation 是 restricted run workspace 內的 `BOUNDED_REAL_DATA_OBSERVATION`。
它把已凍結 run receipt、C++ stdout、GNU time、獨立 validator、edge probes、
cross-run audit 與 presentation QA 的檔案 SHA 綁成一筆 closed-shape JSON。

目前authority record是
`performance_observation/20260721_HCC1395_w24_w40_performance_edge_observation.v3.json`
（41個source bindings）；實體SHA-256由同目錄`checksums_v3.sha256`獨立封存，
避免本指南與綁定本指南的observation形成自我雜湊循環。v1因presentation百分比
與run-local phase scope hotfix而被supersede；v2發現上述循環後保留為診斷證據。
C++ counts、八項science semantic SHA、w24/w40 runtime與edge-case觀測均未改變。

它只能支持：

- 同一 HCC1395 frozen input 下 w24／w40 的工程比較；
- additive stage、nested timing、task latency、thread、RSS 與 I/O 觀測；
- 已執行 edge probe 的失敗時間、修復時間與 bounded parallel observation；
- frozen data、audit 與 HTML QA 的狀態查詢。

它不能支持：

- 「完整 C++ 比 Python 快」；目前歷史成功 baseline 的 compute engine 也是 C++，
  而且 scope、threads、output contract 與 cache 不同；
- 七資料集 production speedup；
- Python-equivalent M1 decision parity；
- 真實非零 topology coverage。

## 2. 時間欄位

`trusted_active_stage_total_seconds` 定義為：

```text
producer_outer_wall_seconds
+ prevalidator_checksum_wall_seconds
+ independent_validator_wall_seconds
```

這是三個 sequential stage wall time 的算術和，不是單一 stopwatch，且不含 cross-run
audit、report generation 或 browser QA。

`science_stages.additive` 的 `handle_open`、`block_execution`、`finalization` 可相加；
`artifact_stream` 已包含在 block execution，`global_pair` 與
`topology_second_pass` 已包含在 finalization，不得重複相加。

`queue_wait_seconds` 是所有 task 從 submit 到 start 的 aggregate task-seconds；
`reorder_wait_seconds` 是 worker 在 ordered sink 等待的 aggregate worker-seconds。
兩者都可能大於 wall time，不能加到總執行時間。

## 3. 驗證 schema

令 `OBSERVATION` 指向 restricted workspace 內的 observation JSON：

```bash
PYTHONPATH=/tmp/longlineage-jsonschema4 python3 - \
  schema/presentation/hcc1395_performance_edge_observation.schema.json \
  "$OBSERVATION" <<'PY'
import json
import sys
from pathlib import Path
from jsonschema import Draft202012Validator

schema = json.loads(Path(sys.argv[1]).read_text())
instance = json.loads(Path(sys.argv[2]).read_text())
errors = sorted(
    Draft202012Validator(schema).iter_errors(instance),
    key=lambda error: list(error.path),
)
print(f"schema_errors={len(errors)}")
for error in errors:
    print("/".join(map(str, error.path)), error.message)
raise SystemExit(bool(errors))
PY
```

正式 QA 必須再做 negative mutation，例如把 `production_claim_allowed` 改成
`true`，並確認 schema 非零退出。

## 4. 常用查詢

完整 w24／w40時間、記憶體與平行化：

```bash
jq '{
  verdict:.comparison_verdict,
  language_speed_claim,
  runs:.run_observations,
  comparisons
}' "$OBSERVATION"
```

只看可信 active-stage 時間：

```bash
jq -r '
  .run_observations
  | to_entries[]
  | [
      .key,
      .value.producer_outer_wall_seconds,
      .value.prevalidator_checksum_wall_seconds,
      .value.independent_validator_wall_seconds,
      .value.trusted_active_stage_total_seconds
    ]
  | @tsv
' "$OBSERVATION"
```

只看 additive／nested science time：

```bash
jq '
  .run_observations
  | with_entries(.value |= {
      science_core_wall_seconds,
      science_stages
    })
' "$OBSERVATION"
```

列出每個 edge case 的執行時間與結果：

```bash
jq -r '
  .edge_cases[]
  | .edge_id as $edge
  | .observations[]
  | [
      $edge,
      .label,
      .scope,
      (.workers // "NA"),
      .wall_seconds,
      .exit_code,
      .result
    ]
  | @tsv
' "$OBSERVATION"
```

確認 data／HTML QA：

```bash
jq '{
  data_validation,
  presentation_validation,
  blockers
}' "$OBSERVATION"
```

重驗全部 source SHA：

```bash
jq -r '.source_bindings[] | [.sha256,.path] | @tsv' "$OBSERVATION" |
while IFS=$'\t' read -r expected path; do
    observed=$(sha256sum "$path" | cut -d' ' -f1)
    test "$observed" = "$expected" || {
        printf 'SHA_MISMATCH\t%s\n' "$path" >&2
        exit 1
    }
done
```

## 5. 查詢紀律

- 先驗 observation schema，再重驗所有 source SHA，最後才引用數字。
- `cache_condition=UNKNOWN` 時不得宣稱受控 scaling。
- `PARTIAL_DIAGNOSTIC_NOT_PRODUCTION` edge probe 不可替代完整 run。
- `PASS` audit 表示稽核完成且其 checks 通過，不代表 legacy M1 parity；應同時查
  `data_validation.legacy_m1_parity.verdict`。
- HTML／JSON 只屬 presentation；正式科學 authority 仍是 C++ frozen artifacts、
  receipts 與獨立 validator。
